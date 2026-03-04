package com.example.pockettracker

import android.app.Application
import org.acra.config.dialog
import org.acra.config.mailSender
import org.acra.data.StringFormat
import org.acra.ktx.initAcra

class PocketTrackerApp : Application() {

    override fun onCreate() {
        super.onCreate()

        initAcra {
            reportFormat = StringFormat.JSON

            dialog {
                text = "PocketTracker crashed. Send a report to help fix it?"
                title = "Crash Report"
                positiveButtonText = "Send"
                negativeButtonText = "Dismiss"
                resIcon = android.R.drawable.ic_dialog_alert
            }

            mailSender {
                mailTo = "your@email.com"   // ← replace with your email
                reportFileName = "pockettracker_crash.json"
                subject = "[PocketTracker] Crash Report"
                body = "Please describe what you were doing when the crash happened:\n\n"
            }
        }
    }
}
