package org.xash3d.skeleton

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import org.xash3d.skeleton.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.statusText.text = buildString {
            appendLine("APK skeleton is working.")
            appendLine("This project compiles as a normal Android app.")
            appendLine("Place the engine code and native dependencies separately.")
        }
    }
}
