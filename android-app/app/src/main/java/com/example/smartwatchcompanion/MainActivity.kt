package com.example.smartwatchcompanion

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    private lateinit var statusText: TextView
    private lateinit var notificationAccessStatus: TextView
    private lateinit var notificationAccessButton: Button
    private var connectionStatus = "Not connected"
    private var spotifyStatus = NotificationReaderService.spotifyStatus
    private var mapsStatus = NotificationReaderService.mapsStatus
    private var sourceStatusReceiverRegistered = false
    private val notificationAccessHandler = Handler(Looper.getMainLooper())
    private val notificationAccessRetry = object : Runnable {
        override fun run() {
            refreshNotificationAccessStatus()
            notificationAccessHandler.postDelayed(this, NOTIFICATION_ACCESS_CHECK_INTERVAL_MS)
        }
    }

    private val sourceStatusReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: android.content.Context, intent: Intent) {
            val status = intent.getStringExtra(NotificationReaderService.EXTRA_STATUS) ?: return
            when (intent.getStringExtra(NotificationReaderService.EXTRA_SOURCE)) {
                NotificationReaderService.SOURCE_SPOTIFY -> spotifyStatus = status
                NotificationReaderService.SOURCE_MAPS -> mapsStatus = status
            }
            updateStatusText()
        }
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        if (results.values.all { it }) {
            BleGattClient.startScanAndConnect(applicationContext, userInitiated = true)
        } else {
            statusText.text = "Bluetooth permissions are required to connect to the watch."
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        notificationAccessStatus = findViewById(R.id.notificationAccessStatus)
        notificationAccessButton = findViewById(R.id.notificationAccessButton)
        notificationAccessButton.setOnClickListener {
            val accessGranted = NotificationReaderService.hasNotificationAccess(this)
            if (accessGranted) {
                NotificationReaderService.requestRebindIfNeeded(
                    applicationContext,
                    force = true
                )
                Toast.makeText(
                    this,
                    "Reconnect requested. If it stays disconnected, turn notification access off and back on in Settings.",
                    Toast.LENGTH_LONG
                ).show()
            }
            val listenerComponent = ComponentName(this, NotificationReaderService::class.java)
            val detailIntent = Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS)
                .putExtra(
                    Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME,
                    listenerComponent.flattenToString()
                )
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    startActivity(detailIntent)
                } else {
                    startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                }
            } catch (e: ActivityNotFoundException) {
                openNotificationListenerSettingsFallback()
            } catch (e: SecurityException) {
                Toast.makeText(
                    this,
                    "Android could not open notification access settings.",
                    Toast.LENGTH_LONG
                ).show()
            }
        }
        findViewById<Button>(R.id.connectButton).setOnClickListener {
            requestBlePermissionsAndConnect()
        }
        findViewById<Button>(R.id.disconnectButton).setOnClickListener {
            BleGattClient.disconnect()
        }
    }

    override fun onStart() {
        super.onStart()
        spotifyStatus = NotificationReaderService.spotifyStatus
        mapsStatus = NotificationReaderService.mapsStatus
        ContextCompat.registerReceiver(
            this,
            sourceStatusReceiver,
            IntentFilter(NotificationReaderService.ACTION_SOURCE_STATUS),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        sourceStatusReceiverRegistered = true
        BleGattClient.setConnectionStatusListener { message ->
            runOnUiThread {
                connectionStatus = message
                updateStatusText()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshNotificationAccessStatus()
        notificationAccessHandler.removeCallbacks(notificationAccessRetry)
        notificationAccessHandler.postDelayed(
            notificationAccessRetry,
            NOTIFICATION_ACCESS_CHECK_INTERVAL_MS
        )
    }

    override fun onPause() {
        notificationAccessHandler.removeCallbacks(notificationAccessRetry)
        super.onPause()
    }

    override fun onStop() {
        BleGattClient.setConnectionStatusListener(null)
        if (sourceStatusReceiverRegistered) {
            unregisterReceiver(sourceStatusReceiver)
            sourceStatusReceiverRegistered = false
        }
        super.onStop()
    }

    private fun updateStatusText() {
        statusText.text = "$connectionStatus\n\n$spotifyStatus\n$mapsStatus"
        refreshNotificationAccessStatus()
    }

    private fun refreshNotificationAccessStatus() {
        val granted = NotificationReaderService.hasNotificationAccess(this)
        notificationAccessButton.visibility = Button.VISIBLE
        notificationAccessStatus.text = when {
            !granted -> "Notification access is off. Enable it in Android settings."
            NotificationReaderService.isListenerConnected -> "Notification access is on and connected."
            else -> "Access is enabled, but Android has not connected the listener. Tap the button to retry or toggle access in Settings."
        }
        if (granted && !NotificationReaderService.isListenerConnected) {
            NotificationReaderService.requestRebindIfNeeded(applicationContext)
        }
    }

    companion object {
        private const val NOTIFICATION_ACCESS_CHECK_INTERVAL_MS = 5_000L
    }

    private fun openNotificationListenerSettingsFallback() {
        try {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        } catch (e: ActivityNotFoundException) {
            Toast.makeText(
                this,
                "Notification access settings are unavailable on this device.",
                Toast.LENGTH_LONG
            ).show()
        } catch (e: SecurityException) {
            Toast.makeText(
                this,
                "Android blocked access to notification settings.",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun requestBlePermissionsAndConnect() {
        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed += Manifest.permission.BLUETOOTH_SCAN
            needed += Manifest.permission.BLUETOOTH_CONNECT
        } else {
            needed += Manifest.permission.ACCESS_FINE_LOCATION
        }
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            BleGattClient.startScanAndConnect(applicationContext, userInitiated = true)
        } else {
            permissionLauncher.launch(missing.toTypedArray())
        }
    }
}
