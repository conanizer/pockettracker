package com.conanizer.pockettracker

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/**
 * Keeps a song playing while the app is off the screen — Android does not freeze a process running
 * one. Started by [MainActivity.onPause] when a song plays; stopped on resume, or by native once the
 * song stops ([MainActivity.stopPlaybackService]).
 * ⚠️ STOP asks the native loop, which stops the song, hands the sound back and then ends this service.
 */
class PlaybackService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            nativeRequestStop()
            return START_NOT_STICKY
        }
        ServiceCompat.startForeground(
            this, NOTIFICATION_ID, buildNotification(),
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK else 0
        )
        // Not sticky: a song does not survive the process, so a restart would show a dead notification.
        return START_NOT_STICKY
    }

    private fun buildNotification(): android.app.Notification {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.playback_channel),
                                NotificationManager.IMPORTANCE_LOW)
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, PlaybackService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification_play)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.playback_playing))
            .setContentIntent(open)
            .addAction(0, getString(R.string.playback_stop), stop)
            .setOngoing(true)
            .setSilent(true)
            .build()
    }

    companion object {
        private const val CHANNEL_ID      = "playback"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP     = "com.conanizer.pockettracker.STOP"

        /** Native: the loop stops the transport on its next tick. */
        @JvmStatic external fun nativeRequestStop()
    }
}
