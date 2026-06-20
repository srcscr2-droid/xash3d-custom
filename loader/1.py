#!/usr/bin/env python3
# Converted from Python 2 to Python 3
# Original: generates OpenGL wrapper stubs from gldef.in

import re

with open('gldef.in', 'r') as lines_file:
    for line in lines_file:
        line = line.rstrip('\n')
        if not line.strip():
            continue

        out = (line
               .replace("BEGIN ", "")
               .replace("END", "")
               .replace("PREFIX", "exp")
               .replace("SECOND", "STDCALL"))
        print(out + "{")

        # Extract function name between PREFIX (now "exp") and "("
        try:
            funcname = line[line.index("PREFIX") + 6:line.index("(")].split(" ")[0]
        except ValueError:
            print("}")
            print("")
            continue

        # Parse argument list to extract variable names only
        raw_args = line[line.index("(") + 1:line.index(")")]
        args = ""
        first = True
        for arg in raw_args.split(", "):
            arg = arg.strip()
            if arg and arg != "void":
                argsplit = re.split(r'[ *]', arg)
                argsplit = [a for a in argsplit if a]  # remove empty strings
                if argsplit:
                    if not first:
                        args += ", "
                    first = False
                    args += argsplit[-1]

        # Return type is second token on the line
        tokens = line.split()
        return_type = tokens[1] if len(tokens) > 1 else "void"
        return_str = "" if return_type == "void" else "return "

        print("    " + return_str + "ldrp" + funcname + "(" + args + ");")
        print("}")
        print("")
