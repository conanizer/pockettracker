package com.example.pockettracker.crash

import android.content.Context
import com.example.pockettracker.BuildConfig
import org.acra.config.CoreConfiguration
import org.acra.sender.ReportSender
import org.acra.sender.ReportSenderFactory

class GitHubIssueSenderFactory : ReportSenderFactory {
    override fun create(context: Context, config: CoreConfiguration): ReportSender =
        GitHubIssueSender(
            token = BuildConfig.GITHUB_TOKEN,
            repoOwner = BuildConfig.GITHUB_REPO_OWNER,
            repoName = BuildConfig.GITHUB_REPO_NAME,
        )

    override fun enabled(config: CoreConfiguration): Boolean = true
}
