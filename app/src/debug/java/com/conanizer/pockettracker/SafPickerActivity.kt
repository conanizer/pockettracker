package com.conanizer.pockettracker

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.provider.DocumentsContract
import android.util.Log

/**
 * Takes a persistable folder grant, and does nothing else. **SAF migration P3a scaffolding.**
 *
 * ⚠️ **This file lives in `src/debug/`, so it CANNOT reach a release APK.** It is `exported` — which
 * is the only way `adb shell am start` can reach it — and an exported activity that opens a folder
 * picker is not something to ship, even one that can only ever grant to this app. P4 replaces it
 * with the in-app `ADD FOLDER…` entry in the roots directory (`saf-migration-plan.md` §7), which is
 * a D-pad flow inside the tracker rather than an activity anybody can launch.
 *
 * It is a separate activity rather than an intent extra on [MainActivity] because that one is an
 * `SDLActivity`: firing a picker from its `onCreate` races the SDL thread it starts there, and the
 * native side would have already read an empty grant list by the time a result arrived. Granting in
 * a run of its own removes the ordering question entirely — take the grant, then launch the tracker.
 *
 *     adb shell am start -n com.conanizer.pockettracker.debug/com.conanizer.pockettracker.SafPickerActivity
 *     adb shell am start -n …/.MainActivity --es storage saf
 */
class SafPickerActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        report("before")

        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        // Open the picker AT the folder we want rather than at Recents, which turns granting into two
        // taps instead of a navigation. `--es at primary:Documents/PocketTracker` overrides it.
        // ⚠️ A HINT ONLY — a provider is free to ignore it, and API 26 ignores it entirely (it is
        // API 29+). Nothing below depends on it having worked.
        val at = getIntent()?.getStringExtra("at") ?: "primary:Documents/PocketTracker"
        runCatching {
            DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", at)
        }.onSuccess { intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, it) }

        startActivityForResult(intent, REQ_TREE)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val tree = data?.data
        if (requestCode != REQ_TREE || resultCode != RESULT_OK || tree == null) {
            Log.i(TAG, "saf-pick: cancelled (result=$resultCode)")
            finish()
            return
        }
        // ⚠️ WITHOUT this the grant dies with the activity, and the next launch finds nothing — which
        // presents as "the browser is empty again" rather than as a permission error.
        contentResolver.takePersistableUriPermission(
            tree, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        )
        Log.i(TAG, "saf-pick: took $tree")
        report("after")
        finish()
    }

    /** The grant list as the tracker will derive it, so the ids in the log match the ones in a path. */
    private fun report(when_: String) {
        val roots = SafStorage(this).roots()
        Log.i(TAG, "saf-pick: $when_ - ${roots.size} granted")
        roots.forEach { Log.i(TAG, "saf-pick:   pt://${it.id}  '${it.displayName}'  ${it.docUri}") }
    }

    private companion object {
        const val TAG = "PocketTrackerSDL"
        const val REQ_TREE = 1
    }
}
