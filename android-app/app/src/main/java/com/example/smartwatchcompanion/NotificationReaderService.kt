package com.example.smartwatchcompanion

import android.app.Notification
import android.graphics.Bitmap
import android.graphics.Color
import android.content.ComponentName
import android.content.Intent
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

private const val SPOTIFY_PACKAGE = "com.spotify.music"
private const val MAPS_PACKAGE = "com.google.android.apps.maps"
// Re-sync periodically (not just on connect) so the watch's clock catches drift,
// DST changes, or a manual clock adjustment without needing a full reconnect.
private const val TIME_SYNC_INTERVAL_MS = 30 * 60 * 1000L  // 30 minutes
// How often to check playback position against the loaded lyrics for a line change.
private const val LYRICS_POLL_INTERVAL_MS = 500L
private const val PLAYBACK_POSITION_PUSH_INTERVAL_MS = 1000L

class NotificationReaderService : NotificationListenerService() {

    private var activeController: MediaController? = null
    private var activeSessionListenerRegistered = false

    private var lastLyricsTrack: String? = null
    private var lastLyricsArtist: String? = null
    private var currentLyricLines: List<LyricsFetcher.LyricLine>? = null
    private var lastSentLyricLine: String? = null

    private val timeSyncHandler = Handler(Looper.getMainLooper())
    private val timeSyncRunnable = object : Runnable {
        override fun run() {
            if (BleGattClient.isConnected()) {
                BleGattClient.sendTimeSync()
            }
            timeSyncHandler.postDelayed(this, TIME_SYNC_INTERVAL_MS)
        }
    }

    private val lyricsPollHandler = Handler(Looper.getMainLooper())
    private val lyricsPollRunnable = object : Runnable {
        override fun run() {
            updateCurrentLyricLine()
            lyricsPollHandler.postDelayed(this, LYRICS_POLL_INTERVAL_MS)
        }
    }

    private val playbackPositionHandler = Handler(Looper.getMainLooper())
    private val playbackPositionRunnable = object : Runnable {
        override fun run() {
            val state = activeController?.playbackState
            if (state?.state == PlaybackState.STATE_PLAYING && BleGattClient.isConnected()) {
                pushCurrentPlaybackPosition(state)
            }
            playbackPositionHandler.postDelayed(this, PLAYBACK_POSITION_PUSH_INTERVAL_MS)
        }
    }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) {
            metadata ?: return
            val track = metadata.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
            val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
            val durationMs = metadata.getLong(MediaMetadata.METADATA_KEY_DURATION)
            val state = activeController?.playbackState
            val positionMs = currentPositionMs(state)
            val isPlaying = state?.state == PlaybackState.STATE_PLAYING
            reportSourceStatus(SOURCE_SPOTIFY, if (track.isEmpty()) {
                "Spotify session found; no track metadata"
            } else {
                "Spotify: $track${if (artist.isNotEmpty()) " — $artist" else ""}"
            })
            BleGattClient.sendTrackInfo(track, artist, isPlaying, durationMs, positionMs)
            BleGattClient.sendAlbumColor(extractAlbumColor(metadata))

            if (track != lastLyricsTrack || artist != lastLyricsArtist) {
                lastLyricsTrack = track
                lastLyricsArtist = artist
                currentLyricLines = null
                lastSentLyricLine = null
                if (track.isNotEmpty()) {
                    lastSentLyricLine = "Searching for lyrics..."
                    BleGattClient.sendLyricsLine(lastSentLyricLine!!)
                    fetchLyricsInBackground(track, artist, (durationMs / 1000).toInt())
                } else {
                    BleGattClient.sendLyricsLine("")
                }
            }
        }

        override fun onPlaybackStateChanged(state: PlaybackState?) {
            state ?: return
            if (activeController?.metadata == null) {
                reportSourceStatus(
                    SOURCE_SPOTIFY,
                    if (state.state == PlaybackState.STATE_PLAYING) "Spotify is playing; waiting for track" else "Spotify is paused"
                )
                BleGattClient.sendPlaybackState(state.state == PlaybackState.STATE_PLAYING)
                return
            }

            val metadata = activeController?.metadata ?: return
            val track = metadata.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
            val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
            val durationMs = metadata.getLong(MediaMetadata.METADATA_KEY_DURATION)
            BleGattClient.sendTrackInfo(
                track,
                artist,
                state.state == PlaybackState.STATE_PLAYING,
                durationMs,
                currentPositionMs(state)
            )
        }
    }

    /** lrclib.net is a network call -- do it off the main thread. */
    private fun fetchLyricsInBackground(track: String, artist: String, durationSec: Int) {
        Thread {
            val lines = LyricsFetcher.fetchSyncedLyrics(track, artist, durationSec)
            timeSyncHandler.post {
                // Only apply the result if the track hasn't changed again while this was in flight.
                if (track == lastLyricsTrack && artist == lastLyricsArtist) {
                    currentLyricLines = lines
                    if (lines == null) {
                        lastSentLyricLine = "No lyrics found"
                        BleGattClient.sendLyricsLine(lastSentLyricLine!!)
                    } else {
                        lastSentLyricLine = "[music]"
                        BleGattClient.sendLyricsLine(lastSentLyricLine!!)
                    }
                }
            }
        }.start()
    }

    /** Matches the controller's current playback position against the loaded lyrics. */
    private fun updateCurrentLyricLine() {
        val lines = currentLyricLines ?: return
        val state = activeController?.playbackState ?: return

        val positionMs = currentPositionMs(state)

        var current: String? = null
        for (line in lines) {
            if (line.timeMs <= positionMs) current = line.text else break
        }
        val displayLine = current?.let(LyricsFetcher::toDisplayText) ?: "[music]"
        if (displayLine != lastSentLyricLine) {
            lastSentLyricLine = displayLine
            BleGattClient.sendLyricsLine(displayLine)
        }
    }

    private val sessionListener = MediaSessionManager.OnActiveSessionsChangedListener { controllers ->
        Log.i(TAG, "Active media sessions changed: ${controllers?.joinToString { it.packageName } ?: "none"}")
        val spotifyController = controllers?.firstOrNull { it.packageName == SPOTIFY_PACKAGE }
        if (spotifyController != null && spotifyController != activeController) {
            activeController?.unregisterCallback(controllerCallback)
            activeController = spotifyController
            activeController?.registerCallback(controllerCallback)
            activeController?.metadata?.let { controllerCallback.onMetadataChanged(it) }
            activeController?.playbackState?.let { controllerCallback.onPlaybackStateChanged(it) }
        } else if (spotifyController == null) {
            reportSourceStatus(SOURCE_SPOTIFY, "No Spotify media session detected")
        }
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        isListenerConnected = true
        Log.i(TAG, "Notification listener connected")
        reportSourceStatus(SOURCE_SPOTIFY, "Waiting for Spotify media session")
        reportSourceStatus(SOURCE_MAPS, "Waiting for Maps navigation")

        val msm = getSystemService(MEDIA_SESSION_SERVICE) as MediaSessionManager
        val component = ComponentName(this, NotificationReaderService::class.java)
        if (!activeSessionListenerRegistered) {
            msm.addOnActiveSessionsChangedListener(sessionListener, component)
            activeSessionListenerRegistered = true
        }
        sessionListener.onActiveSessionsChanged(msm.getActiveSessions(component))

        // Playback commands come back from the watch's BOOT button over BLE.
        BleGattClient.onPlaybackCommand = { cmd ->
            val transport = activeController?.transportControls
            when (cmd) {
                PlaybackCommand.TOGGLE -> {
                    if (activeController?.playbackState?.state == PlaybackState.STATE_PLAYING) {
                        transport?.pause()
                    } else {
                        transport?.play()
                    }
                }
                PlaybackCommand.NEXT -> transport?.skipToNext()
                PlaybackCommand.PREVIOUS -> transport?.skipToPrevious()
            }
        }
        BleGattClient.onServicesReady = {
            timeSyncHandler.post { syncCurrentStateToWatch() }
        }

        BleGattClient.startScanAndConnect(applicationContext)
        timeSyncHandler.postDelayed(timeSyncRunnable, TIME_SYNC_INTERVAL_MS)
        lyricsPollHandler.postDelayed(lyricsPollRunnable, LYRICS_POLL_INTERVAL_MS)
        playbackPositionHandler.postDelayed(playbackPositionRunnable, PLAYBACK_POSITION_PUSH_INTERVAL_MS)
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        isListenerConnected = false
        val mediaSessionManager = getSystemService(MEDIA_SESSION_SERVICE) as MediaSessionManager
        if (activeSessionListenerRegistered) {
            mediaSessionManager.removeOnActiveSessionsChangedListener(sessionListener)
            activeSessionListenerRegistered = false
        }
        activeController?.unregisterCallback(controllerCallback)
        activeController = null
        reportSourceStatus(SOURCE_SPOTIFY, "Notification listener disconnected; reconnecting")
        reportSourceStatus(SOURCE_MAPS, "Notification listener disconnected; reconnecting")
        Log.w(TAG, "Notification listener disconnected; requesting system rebind")
        requestRebindIfNeeded(applicationContext)
    }

    override fun onDestroy() {
        super.onDestroy()
        isListenerConnected = false
        timeSyncHandler.removeCallbacks(timeSyncRunnable)
        lyricsPollHandler.removeCallbacks(lyricsPollRunnable)
        playbackPositionHandler.removeCallbacks(playbackPositionRunnable)
        BleGattClient.onServicesReady = null
        BleGattClient.onPlaybackCommand = null
        activeController?.unregisterCallback(controllerCallback)
        activeController = null
        if (activeSessionListenerRegistered) {
            val mediaSessionManager = getSystemService(MEDIA_SESSION_SERVICE) as MediaSessionManager
            mediaSessionManager.removeOnActiveSessionsChangedListener(sessionListener)
            activeSessionListenerRegistered = false
        }
    }

    private fun syncCurrentStateToWatch() {
        refreshActiveMediaSessions()
        val metadata = activeController?.metadata
        val track = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val durationMs = metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        val state = activeController?.playbackState
        val positionMs = currentPositionMs(state)
        val isPlaying = state?.state == PlaybackState.STATE_PLAYING
        Log.i(TAG, "Syncing playback state: track='$track', playing=$isPlaying")
        BleGattClient.sendTrackInfo(track, artist, isPlaying, durationMs, positionMs)
        BleGattClient.sendAlbumColor(metadata?.let(::extractAlbumColor))
        BleGattClient.sendPlaybackState(isPlaying)

        val activeMapsNotification = activeNotifications
            ?.filter { it.packageName == MAPS_PACKAGE }
            ?.maxByOrNull { it.postTime }
        if (activeMapsNotification != null) {
            Log.i(TAG, "Syncing active Google Maps notification")
            sendNavigationNotification(activeMapsNotification)
        } else {
            Log.i(TAG, "No active Google Maps notification")
            reportSourceStatus(SOURCE_MAPS, "No active Maps navigation notification")
            BleGattClient.sendNavActive(false)
        }
    }

    private fun refreshActiveMediaSessions() {
        val mediaSessionManager = getSystemService(MEDIA_SESSION_SERVICE) as MediaSessionManager
        val listenerComponent = ComponentName(this, NotificationReaderService::class.java)
        sessionListener.onActiveSessionsChanged(mediaSessionManager.getActiveSessions(listenerComponent))
    }

    private fun currentPositionMs(state: PlaybackState?): Long {
        if (state == null) return 0L
        if (state.state != PlaybackState.STATE_PLAYING) return state.position.coerceAtLeast(0L)
        val elapsed = (SystemClock.elapsedRealtime() - state.lastPositionUpdateTime) * state.playbackSpeed
        return (state.position + elapsed.toLong()).coerceAtLeast(0L)
    }

    private fun pushCurrentPlaybackPosition(state: PlaybackState) {
        val metadata = activeController?.metadata ?: return
        val track = metadata.getString(MediaMetadata.METADATA_KEY_TITLE) ?: return
        val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val durationMs = metadata.getLong(MediaMetadata.METADATA_KEY_DURATION)
        BleGattClient.sendTrackInfo(track, artist, true, durationMs, currentPositionMs(state))
    }

    private fun extractAlbumColor(metadata: MediaMetadata): Int? {
        val artwork = metadata.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: metadata.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: metadata.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
            ?: return null
        if (artwork.width <= 0 || artwork.height <= 0) return null

        val binCount = 4096
        val allCounts = IntArray(binCount)
        val colorCounts = IntArray(binCount)
        val redSums = IntArray(binCount)
        val greenSums = IntArray(binCount)
        val blueSums = IntArray(binCount)
        var saturatedPixelCount = 0
        val sampleSize = 24

        for (y in 0 until sampleSize) {
            val sourceY = (y * artwork.height / sampleSize).coerceAtMost(artwork.height - 1)
            for (x in 0 until sampleSize) {
                val sourceX = (x * artwork.width / sampleSize).coerceAtMost(artwork.width - 1)
                val pixel = artwork.getPixel(sourceX, sourceY)
                if (Color.alpha(pixel) < 128) continue

                val red = Color.red(pixel)
                val green = Color.green(pixel)
                val blue = Color.blue(pixel)
                val bin = ((red shr 4) shl 8) or ((green shr 4) shl 4) or (blue shr 4)
                allCounts[bin]++
                redSums[bin] += red
                greenSums[bin] += green
                blueSums[bin] += blue

                val hsv = FloatArray(3)
                Color.colorToHSV(pixel, hsv)
                if (hsv[1] >= 0.2f && hsv[2] >= 0.12f && hsv[2] <= 0.95f) {
                    colorCounts[bin]++
                    saturatedPixelCount++
                }
            }
        }

        val counts = if (saturatedPixelCount >= 3) colorCounts else allCounts
        val dominantBin = counts.indices.maxByOrNull { counts[it] } ?: return null
        val count = allCounts[dominantBin]
        if (count == 0) return null
        return Color.rgb(
            redSums[dominantBin] / count,
            greenSums[dominantBin] / count,
            blueSums[dominantBin] / count
        )
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PACKAGE) return
        Log.i(TAG, "Google Maps notification posted")
        sendNavigationNotification(sbn)
    }

    private fun sendNavigationNotification(sbn: StatusBarNotification) {
        val extras = sbn.notification.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        val text = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""
        if (title.isEmpty() && text.isEmpty()) return
        Log.i(TAG, "Sending Maps notification: title='$title', text='$text'")
        reportSourceStatus(SOURCE_MAPS, "Maps: ${text.ifEmpty { title }}")
        // Google Maps' persistent nav notification typically shows the distance to
        // the next turn as the prominent title (e.g. "800 m") and the turn
        // instruction/road name as the body text (e.g. "Turn left onto Main St") --
        // best-effort parsing, unverified against a real device. If the watch shows
        // these swapped once you're actually testing, swap title/text below.
        BleGattClient.sendNavInstruction(
            LyricsFetcher.toDisplayText(text),
            LyricsFetcher.toDisplayText(title),
            guessManeuverCode(text)
        )
        BleGattClient.sendNavActive(true)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName == MAPS_PACKAGE) {
            BleGattClient.sendNavActive(false)
        }
    }

    private fun guessManeuverCode(instruction: String): Int {
        val lower = LyricsFetcher.toDisplayText(instruction).lowercase()
        return when {
            Regex("""\bu[\s-]?turn\b""").containsMatchIn(lower) ||
                lower.contains("turn around") ||
                lower.contains("anastrof") ||
                lower.contains("epistrof") -> 3
            lower.contains("left") || lower.contains("arister") -> 1
            lower.contains("right") || lower.contains("deksia") || lower.contains("dexia") -> 2
            else -> 0
        }
    }

    companion object {
        private const val TAG = "NotificationReader"
        const val ACTION_SOURCE_STATUS = "com.example.smartwatchcompanion.SOURCE_STATUS"
        const val EXTRA_SOURCE = "source"
        const val EXTRA_STATUS = "status"
        const val SOURCE_SPOTIFY = "spotify"
        const val SOURCE_MAPS = "maps"

        @Volatile
        var isListenerConnected = false
            private set

        @Volatile
        private var lastRebindRequestAtMs = 0L

        fun requestRebindIfNeeded(
            context: android.content.Context,
            force: Boolean = false
        ) {
            val component = ComponentName(context, NotificationReaderService::class.java)
            if (!hasNotificationAccess(context)) return
            if (isListenerConnected && !force) return

            val now = SystemClock.elapsedRealtime()
            if (!force && lastRebindRequestAtMs != 0L &&
                now - lastRebindRequestAtMs < REBIND_REQUEST_INTERVAL_MS
            ) return
            lastRebindRequestAtMs = now
            if (force) {
                isListenerConnected = false
            }
            try {
                NotificationListenerService.requestRebind(component)
                Log.i(TAG, "Requested notification-listener rebind (force=$force)")
            } catch (e: SecurityException) {
                Log.e(TAG, "Android rejected the notification-listener rebind request", e)
            }
        }

        fun hasNotificationAccess(context: android.content.Context): Boolean {
            val component = ComponentName(context, NotificationReaderService::class.java)
            val enabledListeners = Settings.Secure.getString(
                context.contentResolver,
                "enabled_notification_listeners"
            ).orEmpty()
            return enabledListeners.split(':').any {
                ComponentName.unflattenFromString(it) == component
            }
        }

        private const val REBIND_REQUEST_INTERVAL_MS = 10_000L

        @Volatile
        var spotifyStatus = "Spotify: waiting for notification access"
            private set

        @Volatile
        var mapsStatus = "Maps: waiting for notification access"
            private set
    }

    private fun reportSourceStatus(source: String, status: String) {
        when (source) {
            SOURCE_SPOTIFY -> spotifyStatus = status
            SOURCE_MAPS -> mapsStatus = status
        }
        sendBroadcast(
            Intent(ACTION_SOURCE_STATUS)
                .setPackage(packageName)
                .putExtra(EXTRA_SOURCE, source)
                .putExtra(EXTRA_STATUS, status)
        )
    }
}
