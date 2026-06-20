/*
sv_direct.c - create_server command with P2P / NAT traversal
Part of Xash3D-NG fork, version 0.19.4.1 build 61326

Provides:
  create_server <map> [maxplayers]

  1. Starts a multiplayer listen server on the map
  2. Discovers the external (internet-facing) IP via STUN (RFC 5389)
  3. Attempts UPnP/IGD automatic port forwarding on the local router
  4. Prints "connect IP:PORT" so any player in the world can join directly
*/

#include "common.h"
#include "server.h"

// ─────────────────────────────────────────────────────────────
// Platform socket helpers
// ─────────────────────────────────────────────────────────────
#if defined(_WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
# define SHUT_RDWR   SD_BOTH
  typedef int socklen_t;
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
  typedef int SOCKET;
# define INVALID_SOCKET  (-1)
# define closesocket(s)  close(s)
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────
// STUN – RFC 5389 Binding Request
// We use Google's public STUN server to get our external IP.
// Same protocol used by WebRTC, Discord, Zoom, VoIP apps, etc.
// ─────────────────────────────────────────────────────────────
#define STUN_SERVER_HOST  "stun.l.google.com"
#define STUN_SERVER_PORT  19302
#define STUN_MAGIC_COOKIE 0x2112A442UL
#define STUN_TIMEOUT_SEC  4

// STUN attribute types
#define STUN_ATTR_MAPPED_ADDRESS     0x0001
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020

static qboolean SV_STUN_GetExternalAddr( char *out_ip, int out_ip_size, int *out_port )
{
	SOCKET            sock;
	struct sockaddr_in stun_addr;
	struct hostent   *he;
	unsigned char     req[20];
	unsigned char     resp[512];
	int               i, len;
	fd_set            fds;
	struct timeval    tv;
	uint32_t          magic = htonl( STUN_MAGIC_COOKIE );

	MsgDev( D_NOTE, "STUN: resolving %s:%d\n", STUN_SERVER_HOST, STUN_SERVER_PORT );

	he = gethostbyname( STUN_SERVER_HOST );
	if( !he )
	{
		MsgDev( D_ERROR, "STUN: cannot resolve %s\n", STUN_SERVER_HOST );
		return false;
	}

	sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( sock == INVALID_SOCKET )
	{
		MsgDev( D_ERROR, "STUN: cannot create UDP socket\n" );
		return false;
	}

	memset( &stun_addr, 0, sizeof( stun_addr ) );
	stun_addr.sin_family      = AF_INET;
	stun_addr.sin_port        = htons( STUN_SERVER_PORT );
	stun_addr.sin_addr        = *( (struct in_addr *)he->h_addr );

	// Build binding request (20-byte header, no attributes)
	memset( req, 0, sizeof( req ) );
	req[0] = 0x00; req[1] = 0x01;  // Message Type: Binding Request
	req[2] = 0x00; req[3] = 0x00;  // Message Length: 0
	req[4] = (magic >> 24) & 0xFF;
	req[5] = (magic >> 16) & 0xFF;
	req[6] = (magic >>  8) & 0xFF;
	req[7] = (magic      ) & 0xFF;
	// Transaction ID: 12 bytes of pseudo-random
	for( i = 8; i < 20; i++ )
		req[i] = (unsigned char)(rand() & 0xFF);

	if( sendto( sock, (char *)req, 20, 0,
	            (struct sockaddr *)&stun_addr, sizeof( stun_addr ) ) < 0 )
	{
		MsgDev( D_ERROR, "STUN: sendto failed\n" );
		closesocket( sock );
		return false;
	}

	// Wait for response (non-blocking select)
	FD_ZERO( &fds );
	FD_SET( sock, &fds );
	tv.tv_sec  = STUN_TIMEOUT_SEC;
	tv.tv_usec = 0;

	if( select( (int)sock + 1, &fds, NULL, NULL, &tv ) <= 0 )
	{
		MsgDev( D_WARN, "STUN: no response from %s (timeout %ds)\n",
		        STUN_SERVER_HOST, STUN_TIMEOUT_SEC );
		closesocket( sock );
		return false;
	}

	len = recvfrom( sock, (char *)resp, sizeof( resp ), 0, NULL, NULL );
	closesocket( sock );

	if( len < 20 )
	{
		MsgDev( D_ERROR, "STUN: short response (%d bytes)\n", len );
		return false;
	}

	// Check for Binding Success Response (0x0101)
	if( resp[0] != 0x01 || resp[1] != 0x01 )
	{
		MsgDev( D_ERROR, "STUN: unexpected response type 0x%02X%02X\n", resp[0], resp[1] );
		return false;
	}

	// Parse attributes starting at byte 20
	{
		int pos = 20;
		while( pos + 4 <= len )
		{
			uint16_t attr_type   = ((uint16_t)resp[pos] << 8) | resp[pos+1];
			uint16_t attr_len    = ((uint16_t)resp[pos+2] << 8) | resp[pos+3];
			pos += 4;
			if( pos + attr_len > len ) break;

			if( attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8 )
			{
				// XOR-MAPPED-ADDRESS: byte 1 = family (0x01=IPv4)
				//   port = uint16 XOR (magic >> 16)
				//   addr = uint32 XOR magic
				uint16_t xport = ((uint16_t)resp[pos+2] << 8) | resp[pos+3];
				uint32_t xaddr = ((uint32_t)resp[pos+4] << 24) |
				                 ((uint32_t)resp[pos+5] << 16) |
				                 ((uint32_t)resp[pos+6] <<  8) |
				                  (uint32_t)resp[pos+7];
				uint16_t real_port = xport ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
				uint32_t real_addr = xaddr ^ (uint32_t)STUN_MAGIC_COOKIE;
				struct in_addr ia;
				ia.s_addr = htonl( real_addr );
				Q_strncpy( out_ip, inet_ntoa( ia ), out_ip_size );
				if( out_port ) *out_port = (int)real_port;
				return true;
			}
			else if( attr_type == STUN_ATTR_MAPPED_ADDRESS && attr_len >= 8 )
			{
				// Fallback: plain MAPPED-ADDRESS
				uint16_t rport = ((uint16_t)resp[pos+2] << 8) | resp[pos+3];
				uint32_t raddr = ((uint32_t)resp[pos+4] << 24) |
				                 ((uint32_t)resp[pos+5] << 16) |
				                 ((uint32_t)resp[pos+6] <<  8) |
				                  (uint32_t)resp[pos+7];
				struct in_addr ia;
				ia.s_addr = htonl( raddr );
				Q_strncpy( out_ip, inet_ntoa( ia ), out_ip_size );
				if( out_port ) *out_port = (int)rport;
				return true;
			}
			// Advance, attributes are padded to 4-byte boundary
			pos += (attr_len + 3) & ~3;
		}
	}

	MsgDev( D_ERROR, "STUN: could not find address in response\n" );
	return false;
}

// ─────────────────────────────────────────────────────────────
// UPnP / IGD – auto port-forward on the local router
// Same mechanism used by Steam, BitTorrent, Xbox Live.
// ─────────────────────────────────────────────────────────────
#define UPNP_SSDP_ADDR  "239.255.255.250"
#define UPNP_SSDP_PORT  1900
#define UPNP_SSDP_TIMEOUT 2

// Extract a substring between two tokens (returns NULL-term static buffer)
static const char *SV_UPnP_Extract( const char *src, const char *open, const char *close )
{
	static char buf[512];
	const char *s, *e;
	size_t n;

	s = Q_strstr( src, open );
	if( !s ) return NULL;
	s += Q_strlen( open );
	e = Q_strstr( s, close );
	if( !e ) return NULL;
	n = (size_t)(e - s);
	if( n >= sizeof(buf) ) n = sizeof(buf) - 1;
	Q_memcpy( buf, s, n );
	buf[n] = '\0';
	return buf;
}

static qboolean SV_UPnP_AddPortMapping( int port )
{
#ifndef _WIN32
	// Simplified UPnP via shell: try miniupnpc if available on Android/Linux
	char cmd[512];
	// upnpc is part of miniupnpc package; silently skip if not found
	Q_snprintf( cmd, sizeof(cmd),
	    "upnpc -a $(hostname -I | awk '{print $1}') %d %d UDP 2>/dev/null",
	    port, port );
	if( system( cmd ) == 0 )
	{
		MsgDev( D_NOTE, "UPnP: port %d mapped via upnpc\n", port );
		return true;
	}
#endif

	// Built-in lightweight SSDP + SOAP implementation
	SOCKET       sock;
	struct sockaddr_in ssdp;
	char         request[512], response[4096];
	int          rlen;
	fd_set       fds;
	struct timeval tv;
	const char  *location;
	char         ctrl_url[512] = {0};

	sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( sock == INVALID_SOCKET ) return false;

	// Send SSDP M-SEARCH multicast
	memset( &ssdp, 0, sizeof(ssdp) );
	ssdp.sin_family = AF_INET;
	ssdp.sin_port   = htons( UPNP_SSDP_PORT );
	inet_aton( UPNP_SSDP_ADDR, &ssdp.sin_addr );

	Q_snprintf( request, sizeof(request),
	    "M-SEARCH * HTTP/1.1\r\n"
	    "HOST: 239.255.255.250:1900\r\n"
	    "MAN: \"ssdp:discover\"\r\n"
	    "MX: 2\r\n"
	    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n" );

	sendto( sock, request, Q_strlen(request), 0,
	        (struct sockaddr *)&ssdp, sizeof(ssdp) );

	FD_ZERO( &fds );
	FD_SET( sock, &fds );
	tv.tv_sec = UPNP_SSDP_TIMEOUT;
	tv.tv_usec = 0;

	if( select( (int)sock + 1, &fds, NULL, NULL, &tv ) <= 0 )
	{
		closesocket( sock );
		MsgDev( D_NOTE, "UPnP: no gateway found (SSDP timeout)\n" );
		return false;
	}

	rlen = recvfrom( sock, response, sizeof(response) - 1, 0, NULL, NULL );
	closesocket( sock );
	if( rlen <= 0 ) return false;
	response[rlen] = '\0';

	location = SV_UPnP_Extract( response, "LOCATION: ", "\r" );
	if( !location )
		location = SV_UPnP_Extract( response, "location: ", "\r" );
	if( !location )
	{
		MsgDev( D_NOTE, "UPnP: no LOCATION in SSDP response\n" );
		return false;
	}

	Q_strncpy( ctrl_url, location, sizeof(ctrl_url) );
	MsgDev( D_NOTE, "UPnP: gateway at %s\n", ctrl_url );

	// We have the gateway URL — do SOAP AddPortMapping via HTTP
	// Build the SOAP body
	{
		char local_ip[64] = "127.0.0.1";
		char soap_body[1024], http_req[2048];
		char host[256], path[512];
		int  http_port = 80;
		SOCKET tcp;
		struct sockaddr_in gw_addr;
		struct hostent *gw_he;
		const char *p;

		// Parse host:port/path from control URL
		p = ctrl_url;
		if( !Q_strncmp(p, "http://", 7) ) p += 7;
		{
			const char *slash = Q_strchr( p, '/' );
			const char *colon = Q_strchr( p, ':' );
			if( colon && (!slash || colon < slash) )
			{
				size_t n = (size_t)(colon - p);
				if(n >= sizeof(host)) n = sizeof(host)-1;
				Q_memcpy( host, p, n ); host[n] = '\0';
				http_port = Q_atoi( colon + 1 );
			}
			else if( slash )
			{
				size_t n = (size_t)(slash - p);
				if(n >= sizeof(host)) n = sizeof(host)-1;
				Q_memcpy( host, p, n ); host[n] = '\0';
			}
			if( slash )
				Q_strncpy( path, slash, sizeof(path) );
			else
				Q_strncpy( path, "/", sizeof(path) );
		}

		// Get local IP (best-effort)
		{
			SOCKET tmp = socket( AF_INET, SOCK_DGRAM, 0 );
			if( tmp != INVALID_SOCKET )
			{
				struct sockaddr_in dummy;
				socklen_t sl = sizeof(dummy);
				memset( &dummy, 0, sizeof(dummy) );
				dummy.sin_family = AF_INET;
				dummy.sin_port   = htons(80);
				inet_aton( "8.8.8.8", &dummy.sin_addr );
				if( connect(tmp, (struct sockaddr*)&dummy, sizeof(dummy)) == 0 )
					if( getsockname(tmp, (struct sockaddr*)&dummy, &sl) == 0 )
						Q_strncpy( local_ip, inet_ntoa(dummy.sin_addr), sizeof(local_ip) );
				closesocket(tmp);
			}
		}

		Q_snprintf( soap_body, sizeof(soap_body),
		    "<?xml version=\"1.0\"?>"
		    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		    "<s:Body><u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
		    "<NewRemoteHost></NewRemoteHost>"
		    "<NewExternalPort>%d</NewExternalPort>"
		    "<NewProtocol>UDP</NewProtocol>"
		    "<NewInternalPort>%d</NewInternalPort>"
		    "<NewInternalClient>%s</NewInternalClient>"
		    "<NewEnabled>1</NewEnabled>"
		    "<NewPortMappingDescription>Xash3D-NG</NewPortMappingDescription>"
		    "<NewLeaseDuration>0</NewLeaseDuration>"
		    "</u:AddPortMapping></s:Body></s:Envelope>",
		    port, port, local_ip );

		Q_snprintf( http_req, sizeof(http_req),
		    "POST %s HTTP/1.1\r\n"
		    "Host: %s:%d\r\n"
		    "Content-Type: text/xml; charset=\"utf-8\"\r\n"
		    "SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"\r\n"
		    "Content-Length: %d\r\n"
		    "Connection: close\r\n\r\n%s",
		    path, host, http_port, Q_strlen(soap_body), soap_body );

		gw_he = gethostbyname( host );
		if( !gw_he ) return false;

		tcp = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
		if( tcp == INVALID_SOCKET ) return false;

		memset( &gw_addr, 0, sizeof(gw_addr) );
		gw_addr.sin_family = AF_INET;
		gw_addr.sin_port   = htons( (uint16_t)http_port );
		gw_addr.sin_addr   = *(struct in_addr *)gw_he->h_addr;

		if( connect( tcp, (struct sockaddr *)&gw_addr, sizeof(gw_addr) ) < 0 )
		{
			closesocket( tcp );
			return false;
		}
		send( tcp, http_req, Q_strlen(http_req), 0 );
		rlen = recv( tcp, response, sizeof(response) - 1, 0 );
		closesocket( tcp );
		if( rlen > 0 )
		{
			response[rlen] = '\0';
			if( Q_strstr(response, "200 OK") )
			{
				MsgDev( D_NOTE, "UPnP: port %d forwarded OK\n", port );
				return true;
			}
		}
	}
	return false;
}

// ─────────────────────────────────────────────────────────────
// create_server command
// Usage: create_server <map> [maxplayers]
// ─────────────────────────────────────────────────────────────
void SV_CreateServer_f( void )
{
	char     mapname[MAX_QPATH];
	char     external_ip[64] = {0};
	int      external_port   = -1;
	int      maxplayers      = 8;
	int      port;
	int      flags;
	qboolean upnp_ok         = false;

	if( Cmd_Argc() < 2 )
	{
		Msg( "Usage: create_server <map> [maxplayers]\n" );
		Msg( "  Starts a public multiplayer server.\n" );
		Msg( "  Players can connect from anywhere using: connect IP:PORT\n" );
		return;
	}

	Q_strncpy( mapname, Cmd_Argv(1), sizeof(mapname) );
	FS_StripExtension( mapname );

	if( Cmd_Argc() >= 3 )
	{
		maxplayers = bound( 2, Q_atoi( Cmd_Argv(2) ), 32 );
	}

	// Validate map
	flags = SV_MapIsValid( mapname, GI->mp_entity, NULL );
	if( !( flags & MAP_IS_EXIST ) )
	{
		Msg( "create_server: map '%s' not found\n", mapname );
		return;
	}

	port = Cvar_VariableInteger( "port" );
	if( port <= 0 ) port = 27015;

	Msg( "\n╔══════════════════════════════════════╗\n" );
	Msg( "║      Xash3D-NG  create_server        ║\n" );
	Msg( "╚══════════════════════════════════════╝\n" );
	Msg( "Map: %s  |  MaxPlayers: %d  |  Port: %d\n\n", mapname, maxplayers, port );

	// Step 1: Discover external IP via STUN
	Msg( "[1/3] Discovering external IP via STUN...\n" );
	if( SV_STUN_GetExternalAddr( external_ip, sizeof(external_ip), &external_port ) )
	{
		Msg( "      External IP: %s  (STUN port seen: %d)\n", external_ip, external_port );
	}
	else
	{
		Msg( "      STUN failed - will show LAN IP only.\n" );
	}

	// Step 2: UPnP port mapping
	Msg( "[2/3] Attempting UPnP port mapping on router...\n" );
	upnp_ok = SV_UPnP_AddPortMapping( port );
	if( upnp_ok )
		Msg( "      UPnP: port %d opened automatically.\n", port );
	else
		Msg( "      UPnP: not available (manual port forward may be needed).\n" );

	// Step 3: Start the server
	Msg( "[3/3] Starting server on map '%s'...\n\n", mapname );

	Cvar_FullSet( "maxplayers",  va( "%d", maxplayers ), CVAR_LATCH );
	Cvar_FullSet( "deathmatch",  "1",  CVAR_LATCH );
	Cvar_FullSet( "coop",        "0",  CVAR_LATCH );
	Cvar_FullSet( "teamplay",    "0",  CVAR_LATCH );

	Q_strncpy( host.finalmsg, "", MAX_STRING );
	SV_Shutdown( false );
	NET_Config( true, true );

	sv.changelevel = false;
	sv.background  = false;
	sv.loadgame    = false;

	SV_SpawnServer( mapname, NULL );
	SV_LevelInit( mapname, NULL, NULL, false );
	SV_ActivateServer();

	// Print connection info
	Msg( "══════════════════════════════════════════\n" );
	if( external_ip[0] )
	{
		Msg( "✓ SERVER IS PUBLIC — share this with friends:\n\n" );
		Msg( "    connect %s:%d\n\n", external_ip, port );
	}
	else
	{
		Msg( "✓ SERVER STARTED — LAN connection:\n\n" );
		Msg( "    connect <your-local-ip>:%d\n\n", port );
		Msg( "  (Could not auto-detect external IP.\n" );
		Msg( "   Find it at https://api.ipify.org and share: connect YOURIP:%d)\n\n", port );
	}

	if( !upnp_ok )
	{
		Msg( "⚠  NAT/Firewall: if friends can't connect, forward UDP port %d\n", port );
		Msg( "   on your router to this device, then try again.\n" );
	}
	Msg( "══════════════════════════════════════════\n" );
}

// Registered in SV_InitOperatorCommands (sv_cmds.c)
