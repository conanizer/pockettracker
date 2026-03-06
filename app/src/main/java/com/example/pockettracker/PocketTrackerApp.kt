package com.example.pockettracker

import android.app.Application
import org.acra.data.StringFormat
import org.acra.ktx.initAcra

class PocketTrackerApp : Application() {

    override fun onCreate() {
        super.onCreate()

        initAcra {
            reportFormat = StringFormat.JSON
            // Silent mode: no dialog, no notification.
            // GitHubIssueSenderFactory is auto-discovered via META-INF/services.
        }
    }
}
