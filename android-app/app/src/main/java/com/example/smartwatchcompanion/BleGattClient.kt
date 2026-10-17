package com.example.smartwatchcompanion

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.*
// java.util.* above also brings in java.util.ArrayDeque, which would otherwise shadow
// this one -- Java's version isn't List-backed and doesn't have removeFirstOrNull(),
// which the write queue below relies on. This explicit import forces the right one.
import kotlin.collections.ArrayDeque

// Must match the UUIDs in the firmware's src/ble_service.cpp exactly.
object BleUuids {
    val NOW_PLAYING_SERVICE: UUID = UUID.fromString("c9a01000-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_TRACK_INFO: UUID = UUID.fromString("c9a01001-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_PLAYBACK_STATE: UUID = UUID.fromString("c9a01004-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_PLAYBACK_CMD: UUID = UUID.fromString("c9a01005-1000-4bd9-ba21-70e224d9b8a0")  // watch -> phone
    val CHAR_LYRICS_LINE: UUID = UUID.fromString("c9a01006-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_ALBUM_COLOR: UUID = UUID.fromString("c9a01007-1000-4bd9-ba21-70e224d9b8a0")

    val NAV_SERVICE: UUID = UUID.fromString("c9a02000-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_NAV_INSTRUCTION: UUID = UUID.fromString("c9a02001-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_NAV_ACTIVE: UUID = UUID.fromString("c9a02002-1000-4bd9-ba21-70e224d9b8a0")

    val TIME_SERVICE: UUID = UUID.fromString("c9a03000-1000-4bd9-ba21-70e224d9b8a0")
    val CHAR_SET_TIME: UUID = UUID.fromString("c9a03001-1000-4bd9-ba21-70e224d9b8a0")

    // Standard BLE descriptor used to enable notifications on a characteristic
    val CLIENT_CHARACTERISTIC_CONFIG: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

private const val TARGET_DEVICE_NAME = "ESP32 Smartwatch"

enum class PlaybackCommand(val code: Int) {
    TOGGLE(0), NEXT(1), PREVIOUS(2);

    companion object {
        fun fromCode(code: Int) = values().firstOrNull { it.code == code }
    }
}

/**
 * Handles the phone's BLE central role: scan, connect, write now-playing/nav/time data,
 * and subscribe to playback commands the watch's BOOT button sends back.
 */
@SuppressLint("MissingPermission")
object BleGattClient {
    private const val TAG = "BleGattClient"
    private var gatt: BluetoothGatt? = null
    private var connected = false
    private var connecting = false
    private var connectionStatus = "Not connected"
    private var userDisconnected = false
    private var scanCallback: ScanCallback? = null
    private var scanningContext: Context? = null
    private val reconnectHandler = Handler(Looper.getMainLooper())
    private val reconnectRunnable = Runnable {
        scanningContext?.let { startScanAndConnect(it) }
    }
    private val writeQueue: ArrayDeque<() -> Unit> = ArrayDeque()
    private var writeInFlight = false

    // Set this from your app (e.g. the foreground service) to react to watch button presses.
    var onPlaybackCommand: ((PlaybackCommand) -> Unit)? = null
    var onServicesReady: (() -> Unit)? = null
    var onConnectionStatusChanged: ((String) -> Unit)? = null

    fun setConnectionStatusListener(listener: ((String) -> Unit)?) {
        onConnectionStatusChanged = listener
        listener?.invoke(connectionStatus)
    }

    private fun updateConnectionStatus(status: String) {
        connectionStatus = status
        onConnectionStatusChanged?.invoke(status)
    }

    fun startScanAndConnect(context: Context, userInitiated: Boolean = false) {
        if (userInitiated) userDisconnected = false
        if (userDisconnected) return
        if (connected || connecting || scanCallback != null) return
        reconnectHandler.removeCallbacks(reconnectRunnable)
        val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter
        if (adapter == null) {
            updateConnectionStatus("This device does not support Bluetooth.")
            return
        }
        if (!adapter.isEnabled) {
            updateConnectionStatus("Turn on Bluetooth to connect to the watch.")
            return
        }
        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            updateConnectionStatus("Bluetooth scanning is unavailable.")
            return
        }

        scanningContext = context.applicationContext
        scanCallback?.let { scanner.stopScan(it) }
        updateConnectionStatus("Searching for watch...")
        val filters = listOf(ScanFilter.Builder().setDeviceName(TARGET_DEVICE_NAME).build())
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()

        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                Log.i(TAG, "Found watch, connecting...")
                scanner.stopScan(this)
                scanCallback = null
                updateConnectionStatus("Watch found; connecting...")
                connect(context, result.device)
            }

            override fun onScanFailed(errorCode: Int) {
                scanCallback = null
                Log.e(TAG, "Scan failed: $errorCode")
                updateConnectionStatus("Watch scan failed ($errorCode). Tap Connect to watch to retry.")
            }
        }
        scanCallback = callback
        scanner.startScan(filters, settings, callback)
    }

    fun disconnect() {
        userDisconnected = true
        reconnectHandler.removeCallbacks(reconnectRunnable)
        val context = scanningContext
        if (context != null) {
            val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
            scanCallback?.let { callback -> manager.adapter?.bluetoothLeScanner?.stopScan(callback) }
        }
        scanCallback = null
        connected = false
        connecting = false
        writeQueue.clear()
        writeInFlight = false
        val activeGatt = gatt
        gatt = null
        activeGatt?.disconnect()
        activeGatt?.close()
        updateConnectionStatus("Disconnected from watch.")
    }

    private fun connect(context: Context, device: BluetoothDevice) {
        connecting = true
        gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (gatt !== g) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    connected = false
                    connecting = false
                    gatt = null
                    g.close()
                    updateConnectionStatus("Watch connection failed ($status); retrying...")
                    Log.e(TAG, "Connection state change failed: $status")
                    if (!userDisconnected) reconnectHandler.postDelayed(reconnectRunnable, 3000)
                } else if (newState == BluetoothProfile.STATE_CONNECTED) {
                    reconnectHandler.removeCallbacks(reconnectRunnable)
                    connecting = false
                    connected = true
                    updateConnectionStatus("Connected to watch; syncing...")
                    Log.i(TAG, "Connected, negotiating MTU")
                    g.requestMtu(247)
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    connected = false
                    connecting = false
                    if (gatt === g) gatt = null
                    g.close()
                    updateConnectionStatus(if (userDisconnected) "Disconnected from watch." else "Disconnected; searching for watch...")
                    Log.i(TAG, "Disconnected, retrying scan in 3s")
                    if (!userDisconnected) reconnectHandler.postDelayed(reconnectRunnable, 3000)
                }
            }

            override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
                if (gatt !== g) return
                g.discoverServices()
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                if (gatt !== g) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    Log.e(TAG, "Service discovery failed: $status")
                    updateConnectionStatus("Connected, but watch services could not be read ($status).")
                    return
                }
                Log.i(TAG, "Services discovered")
                updateConnectionStatus("Connected to watch.")
                subscribeToPlaybackCommands(g)
                sendTimeSync()
                drainQueue()
                onServicesReady?.invoke()
            }

            override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
                if (gatt !== g) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    Log.e(TAG, "GATT write failed for ${characteristic.uuid}: $status")
                }
                writeInFlight = false
                drainQueue()
            }

            override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
                if (gatt !== g) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    Log.e(TAG, "GATT descriptor write failed for ${descriptor.uuid}: $status")
                }
                writeInFlight = false
                drainQueue()
            }

            override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
                if (characteristic.uuid == BleUuids.CHAR_PLAYBACK_CMD && value.isNotEmpty()) {
                    val cmd = PlaybackCommand.fromCode(value[0].toInt())
                    Log.i(TAG, "Playback command from watch: $cmd")
                    if (cmd != null) onPlaybackCommand?.invoke(cmd)
                }
            }
        })
    }

    /** Enables notifications for the watch's playback-command characteristic. */
    private fun subscribeToPlaybackCommands(g: BluetoothGatt) {
        val service = g.getService(BleUuids.NOW_PLAYING_SERVICE) ?: return
        val char = service.getCharacteristic(BleUuids.CHAR_PLAYBACK_CMD) ?: return
        g.setCharacteristicNotification(char, true)
        val descriptor = char.getDescriptor(BleUuids.CLIENT_CHARACTERISTIC_CONFIG) ?: return
        // Routed through the same write queue as everything else -- Android's BLE
        // stack allows only one outstanding GATT operation at a time, and this used
        // to fire immediately alongside the time-sync write that follows it, which
        // could silently drop one of the two.
        enqueueWrite {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(descriptor)
            }
        }
    }

    private fun enqueueWrite(block: () -> Unit) {
        writeQueue.addLast(block)
        drainQueue()
    }

    private fun drainQueue() {
        if (writeInFlight) return
        val next = writeQueue.removeFirstOrNull() ?: return
        writeInFlight = true
        next()
    }

    private fun writeChar(serviceUuid: UUID, charUuid: UUID, value: ByteArray) {
        val g = gatt
        val service = g?.getService(serviceUuid)
        val char = service?.getCharacteristic(charUuid)
        if (g == null || char == null) {
            Log.e(TAG, "GATT characteristic unavailable: service=$serviceUuid characteristic=$charUuid")
            writeInFlight = false
            drainQueue()
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val result = g.writeCharacteristic(char, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
            if (result != BluetoothStatusCodes.SUCCESS) {
                Log.e(TAG, "Could not queue GATT write for $charUuid: $result")
                writeInFlight = false
                drainQueue()
            }
        } else {
            @Suppress("DEPRECATION")
            char.value = value
            @Suppress("DEPRECATION")
            val accepted = g.writeCharacteristic(char)
            if (!accepted) {
                Log.e(TAG, "Could not queue GATT write for $charUuid")
                writeInFlight = false
                drainQueue()
            }
        }
    }

    fun sendTrackInfo(track: String, artist: String, isPlaying: Boolean, durationMs: Long = 0L, positionMs: Long = 0L) {
        if (!connected) return
        val durationSec = (durationMs / 1000L).coerceAtLeast(0L)
        val positionSec = (positionMs / 1000L).coerceAtLeast(0L)
        val payload = "$track\n$artist\n$durationSec\n$positionSec\n${if (isPlaying) 1 else 0}".toByteArray()
        Log.i(TAG, "Queueing track state (${track.length} title chars, duration=${durationSec}s, playing=$isPlaying)")
        enqueueWrite { writeChar(BleUuids.NOW_PLAYING_SERVICE, BleUuids.CHAR_TRACK_INFO, payload) }
    }

    fun sendPlaybackState(isPlaying: Boolean) {
        if (!connected) return
        enqueueWrite {
            writeChar(BleUuids.NOW_PLAYING_SERVICE, BleUuids.CHAR_PLAYBACK_STATE, byteArrayOf(if (isPlaying) 1 else 0))
        }
    }

    fun sendNavInstruction(instruction: String, distance: String, maneuverCode: Int) {
        if (!connected) return
        val payload = "$instruction\n$distance\n$maneuverCode".toByteArray()
        Log.i(TAG, "Queueing navigation state (${instruction.length} instruction chars)")
        enqueueWrite { writeChar(BleUuids.NAV_SERVICE, BleUuids.CHAR_NAV_INSTRUCTION, payload) }
    }

    fun sendNavActive(active: Boolean) {
        if (!connected) return
        enqueueWrite { writeChar(BleUuids.NAV_SERVICE, BleUuids.CHAR_NAV_ACTIVE, byteArrayOf(if (active) 1 else 0)) }
    }

    fun sendLyricsLine(line: String) {
        if (!connected) return
        enqueueWrite { writeChar(BleUuids.NOW_PLAYING_SERVICE, BleUuids.CHAR_LYRICS_LINE, line.toByteArray()) }
    }

    fun sendAlbumColor(rgb: Int?) {
        if (!connected) return
        val color = rgb ?: 0
        val payload = byteArrayOf(
            ((color shr 16) and 0xFF).toByte(),
            ((color shr 8) and 0xFF).toByte(),
            (color and 0xFF).toByte(),
            if (rgb == null) 0 else 1
        )
        enqueueWrite { writeChar(BleUuids.NOW_PLAYING_SERVICE, BleUuids.CHAR_ALBUM_COLOR, payload) }
    }

    fun sendTimeSync() {
        if (!connected) return
        val epoch = (System.currentTimeMillis() / 1000).toInt()
        val offsetMinutes = (TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000).toShort()
        val buffer = ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(epoch)
        buffer.putShort(offsetMinutes)
        enqueueWrite { writeChar(BleUuids.TIME_SERVICE, BleUuids.CHAR_SET_TIME, buffer.array()) }
    }

    fun isConnected() = connected
}
