package com.example.pockettracker.crash

import android.content.Context
import org.acra.ReportField
import org.acra.data.CrashReportData
import org.acra.sender.ReportSender
import java.net.HttpURLConnection
import java.net.URL

class GitHubIssueSender(
    private val token: String,
    private val repoOwner: String,
    private val repoName: String,
) : ReportSender {

    override fun send(context: Context, report: CrashReportData) {
        if (token.isBlank()) return  // No token configured (e.g. dev build without local.properties)

        val stackTrace = report.getString(ReportField.STACK_TRACE) ?: "No stack trace"
        val appVersion = report.getString(ReportField.APP_VERSION_NAME) ?: "unknown"
        val androidVersion = report.getString(ReportField.ANDROID_VERSION) ?: "unknown"
        val phoneModel = report.getString(ReportField.PHONE_MODEL) ?: "unknown"
        val brand = report.getString(ReportField.BRAND) ?: "unknown"
        val totalMem = report.getString(ReportField.TOTAL_MEM_SIZE) ?: "unknown"
        val availMem = report.getString(ReportField.AVAILABLE_MEM_SIZE) ?: "unknown"
        val customData = report.getString(ReportField.CUSTOM_DATA) ?: ""

        val title = buildIssueTitle(stackTrace)
        val body = buildIssueBody(
            stackTrace, appVersion, androidVersion,
            phoneModel, brand, totalMem, availMem, customData
        )

        val url = URL("https://api.github.com/repos/$repoOwner/$repoName/issues")
        val conn = url.openConnection() as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.setRequestProperty("Authorization", "Bearer $token")
            conn.setRequestProperty("Accept", "application/vnd.github+json")
            conn.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            conn.setRequestProperty("Content-Type", "application/json")
            conn.doOutput = true
            conn.connectTimeout = 10_000
            conn.readTimeout = 10_000

            val payload = buildJsonPayload(title, body)
            conn.outputStream.use { it.write(payload.toByteArray(Charsets.UTF_8)) }

            conn.responseCode  // trigger the request
        } finally {
            conn.disconnect()
        }
    }

    private fun buildIssueTitle(stackTrace: String): String {
        // Use first non-empty line of the stack trace as a compact title
        val firstLine = stackTrace.lines().firstOrNull { it.isNotBlank() } ?: "Unknown crash"
        return "[Crash] ${firstLine.trim().take(100)}"
    }

    private fun buildIssueBody(
        stackTrace: String,
        appVersion: String,
        androidVersion: String,
        phoneModel: String,
        brand: String,
        totalMem: String,
        availMem: String,
        customData: String,
    ): String = buildString {
        appendLine("## Crash Report")
        appendLine()
        appendLine("| Field | Value |")
        appendLine("|-------|-------|")
        appendLine("| App version | `$appVersion` |")
        appendLine("| Android | `$androidVersion` |")
        appendLine("| Device | `$brand $phoneModel` |")
        appendLine("| RAM (total / avail) | `$totalMem / $availMem` |")
        if (customData.isNotBlank()) {
            appendLine("| Context | `$customData` |")
        }
        appendLine()
        appendLine("## Stack Trace")
        appendLine()
        appendLine("```")
        appendLine(stackTrace)
        appendLine("```")
    }

    private fun buildJsonPayload(title: String, body: String): String {
        // Manual JSON build to avoid any extra dependency
        fun escape(s: String) = s
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r")
            .replace("\t", "\\t")

        return """{"title":"${escape(title)}","body":"${escape(body)}","labels":["crash"]}"""
    }
}
