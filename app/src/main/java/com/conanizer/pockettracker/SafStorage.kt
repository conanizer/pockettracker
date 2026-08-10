package com.conanizer.pockettracker

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import java.security.MessageDigest

/**
 * The Storage Access Framework primitives, and nothing else.
 *
 * `ContentResolver` and `DocumentsContract` are Java-only APIs, so the C++ `SafFileSystem` cannot
 * reach them; this class is the platform residue it calls through. It is deliberately DUMB — it
 * queries, reads and creates single documents and knows nothing about `pt://` paths, the seven app
 * folders, or which tree is the app's home. All of that is derivation, and derivation lives in C++
 * where it is one implementation for every future host rather than a second one written in Kotlin.
 *
 * ⚠️ **Every public method here is resolved BY NAME over JNI** (through the wrappers on
 * `MainActivity`, which is the object native holds). They carry `@Keep` there and an explicit rule in
 * `proguard-rules.pro`, per the standing rule — a release-only rename is invisible until a release
 * build.
 */
class SafStorage(private val context: Context) {

    /**
     * A granted tree: its derived id, what to call it on screen, the document URI of its root, and
     * the persisted TREE uri the id was derived from — which is the handle [homeRootId] stores,
     * because it is the authoritative one and the id is only a function of it.
     */
    data class Root(val id: String, val displayName: String, val docUri: String, val treeUri: String)

    /**
     * The granted trees, ordered by id.
     *
     * ⚠️ **The ORDER `getPersistedUriPermissions()` returns is not guaranteed**, which is exactly why
     * the id cannot be an index into it. Sorting by the derived id makes this list stable across
     * boots without storing anything.
     */
    fun roots(): List<Root> {
        val out = ArrayList<Root>()
        for (p in context.contentResolver.persistedUriPermissions) {
            if (!p.isReadPermission) continue
            val tree = p.uri
            val docId = runCatching { DocumentsContract.getTreeDocumentId(tree) }.getOrNull() ?: continue
            val docUri = DocumentsContract.buildDocumentUriUsingTree(tree, docId)
            out += Root(rootId(tree), displayNameOf(docUri, docId), docUri.toString(), tree.toString())
        }
        out.sortBy { it.id }

        // The impossible case, made observable rather than silently resolving to whichever tree came
        // first. 48 bits against a 512-grant ceiling is ~5e-10, and "never" is not a reason to
        // resolve a collision arbitrarily.
        for (i in 1 until out.size) {
            if (out[i].id == out[i - 1].id) {
                Log.e(TAG, "saf: ROOT ID COLLISION ${out[i].id} - " +
                        "'${out[i - 1].displayName}' and '${out[i].displayName}' are indistinguishable")
            }
        }
        return out
    }

    /**
     * The id of a granted tree: 12 hex characters of SHA-256 over the persisted tree URI.
     *
     * ⭐ Derived, never stored — so the id→tree mapping is re-derivable from the grant list on any
     * boot, in any order, and there is no table to migrate or to fall out of sync. See
     * `saf-migration-plan.md` §5b for the three candidates this beat.
     */
    private fun rootId(tree: Uri): String =
        MessageDigest.getInstance("SHA-256")
            .digest(tree.toString().toByteArray())
            .take(6)
            .joinToString("") { "%02x".format(it) }

    /** What the roots directory shows for a tree: the provider's display name, else the tail of its id. */
    private fun displayNameOf(docUri: Uri, docId: String): String {
        queryRow(docUri, DocumentsContract.Document.COLUMN_DISPLAY_NAME)?.let {
            if (it.isNotEmpty()) return clean(it)
        }
        // `primary:Documents/PocketTracker` → `PocketTracker`. A provider that answers neither is
        // still listed, under something, rather than dropped from the browser.
        return clean(docId.substringAfterLast('/').substringAfterLast(':').ifEmpty { "ROOT" })
    }

    fun rootCount(): Int = roots().size

    /**
     * The id of the tree the app's seven folders live in — the HOME root — or "" if nothing is granted.
     *
     * ⚠️⚠️ **This is the one piece of SAF state that is STORED rather than derived, and it has to be.**
     * Everything else about a root comes back out of `getPersistedUriPermissions()` on demand, which is
     * what makes ids re-derivable in any order on any boot. The home CHOICE cannot work that way: any
     * rule computed from the grant set — lowest id, first returned, newest — moves the app's Projects,
     * Samples and Renders folders the day the user grants a second, unrelated folder. The user would
     * see their songs vanish, and nothing would have gone wrong.
     *
     * ⭐ **The TREE URI is stored, not the id**, because the id is a function of the tree URI and not
     * the other way round: a stored id could not survive a change in how ids are derived, and could
     * not be matched back to a grant except by re-deriving every id anyway.
     *
     * **First grant wins, and it keeps winning until it is revoked.** A home that is no longer in the
     * list (the user withdrew the folder, or wiped the app's storage) falls back to the lowest id and
     * re-stamps, which is the only moment this value ever changes on its own.
     */
    fun homeRootId(): String {
        val list = roots()
        if (list.isEmpty()) return ""

        val prefs  = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val stored = prefs.getString(HOME_TREE_KEY, null)
        list.firstOrNull { it.treeUri == stored }?.let { return it.id }

        val chosen = list.first()
        prefs.edit().putString(HOME_TREE_KEY, chosen.treeUri).apply()
        Log.i(TAG, "saf: home root = pt://${chosen.id} '${chosen.displayName}' " +
                   if (stored == null) "(first grant)" else "(previous home $stored is gone)")
        return chosen.id
    }

    /**
     * Fire the system folder picker. True = it is on screen; the GRANT arrives later, in
     * [takeGrant].
     *
     * ⚠️ **Must be called on the UI thread** — `startActivityForResult` is an Activity call and this
     * one is reached from the SDL thread, so [MainActivity.safRequestRoot] posts it and waits only for
     * the launch. See `ui::FileSystem::activate` for why it must not wait for the answer.
     *
     * ⚠️ Android forbids granting a volume root or `Download` itself, and the picker is a TOUCH flow in
     * an app that is otherwise 100 % D-pad. Both are owned regressions of the SAF migration, not
     * surprises — `saf-migration-plan.md` §7.
     */
    fun requestRoot(activity: Activity, requestCode: Int): Boolean {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        // Open AT the app's own folder rather than at Recents, so the common answer is one tap.
        // ⚠️ A HINT ONLY — a provider may ignore it, and it is API 29+. Nothing depends on it landing.
        runCatching {
            DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE_AUTHORITY, "primary:Documents/PocketTracker")
        }.onSuccess { intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, it) }

        return runCatching {
            activity.startActivityForResult(intent, requestCode)
            true
        }.getOrElse {
            // A device with no documents provider at all. The browser says so on its status line
            // rather than looking like a button that does nothing.
            Log.e(TAG, "saf: no activity can handle ACTION_OPEN_DOCUMENT_TREE: $it")
            false
        }
    }

    /**
     * Persist the grant the picker returned. False = the user cancelled, or it could not be taken.
     *
     * ⚠️ **Without `takePersistableUriPermission` the grant dies with the activity**, and the next
     * launch finds nothing — which presents as "the browser is empty again", not as a permission
     * error, so it is the one line here that must never be lost.
     */
    fun takeGrant(tree: Uri?): Boolean {
        if (tree == null) return false
        return runCatching {
            context.contentResolver.takePersistableUriPermission(
                tree, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
            Log.i(TAG, "saf: took grant $tree -> pt://${rootId(tree)} (${rootCount()} granted)")
            true
        }.getOrElse {
            Log.e(TAG, "saf: takePersistableUriPermission failed on $tree: $it")
            false
        }
    }

    /** `<id>\t<displayName>\t<docUri>`, or "" for an index that is no longer there. */
    fun rootInfo(index: Int): String {
        val r = roots().getOrNull(index) ?: return ""
        return "${r.id}\t${r.displayName}\t${r.docUri}"
    }

    /**
     * Every child of a directory document, in ONE query.
     *
     * One record per line: `docUri \t name \t isDir(0|1) \t size \t lastModified`. The whole listing
     * crosses JNI as a single string because the alternative — a call per child, or a call per field
     * — is what makes a 200-entry sample folder feel slow, and `filesystem.h` is explicit that the
     * sort keys are read once at build time rather than re-`stat`ed inside the comparator.
     */
    fun listChildren(dirDocUri: String): String {
        val dir = Uri.parse(dirDocUri)
        val childrenUri = runCatching {
            DocumentsContract.buildChildDocumentsUriUsingTree(dir, DocumentsContract.getDocumentId(dir))
        }.getOrNull() ?: return ""

        val sb = StringBuilder()
        runCatching {
            context.contentResolver.query(
                childrenUri,
                arrayOf(
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE,
                    DocumentsContract.Document.COLUMN_SIZE,
                    DocumentsContract.Document.COLUMN_LAST_MODIFIED
                ), null, null, null
            )?.use { c ->
                while (c.moveToNext()) {
                    val id = c.getString(0) ?: continue
                    val name = clean(c.getString(1) ?: continue)
                    val isDir = c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR
                    val size = if (c.isNull(3)) 0L else c.getLong(3)
                    val modified = if (c.isNull(4)) 0L else c.getLong(4)
                    val childUri = DocumentsContract.buildDocumentUriUsingTree(dir, id)
                    sb.append(childUri).append('\t').append(name).append('\t')
                        .append(if (isDir) '1' else '0').append('\t')
                        .append(size).append('\t').append(modified).append('\n')
                }
            }
        }.onFailure { Log.w(TAG, "saf: listChildren failed on $dirDocUri: $it") }
        return sb.toString()
    }

    /**
     * ⚠️ The record separators are structural, so a display name may not contain them.
     * Android forbids `/` in a display name but says nothing about a tab; a name carrying one would
     * shift every later field by a column. Replaced rather than rejected — a file with an odd name
     * should still be listed.
     */
    private fun clean(s: String): String =
        if (s.indexOf('\t') < 0 && s.indexOf('\n') < 0) s else s.replace('\t', ' ').replace('\n', ' ')

    /** Whole document → bytes, or null. The C++ side turns it into the `std::string` the seam wants. */
    fun readFile(docUri: String): ByteArray? = runCatching {
        context.contentResolver.openInputStream(Uri.parse(docUri))?.use { it.readBytes() }
    }.getOrElse {
        Log.w(TAG, "saf: readFile failed on $docUri: $it")
        null
    }

    /**
     * An OS descriptor for a document, or -1. This is what `pt_fopen`'s hook returns.
     *
     * ⚠️ **`detachFd()`, never `getFd()`** — `byte_source.h` states that ownership transfers to the
     * caller, which wraps it in a `FILE*` and `fclose`s it. Handing back a descriptor the
     * `ParcelFileDescriptor` still tracks means it is closed twice, and the second close lands on
     * whatever unrelated file has since been given that number.
     */
    fun openFd(docUri: String, mode: String): Int = runCatching {
        context.contentResolver.openFileDescriptor(Uri.parse(docUri), mode)?.detachFd() ?: -1
    }.getOrElse {
        Log.w(TAG, "saf: openFd($mode) failed on $docUri: $it")
        -1
    }

    /** Create a sub-directory document, returning its URI — or the EXISTING one if the name is taken. */
    fun createDir(parentDocUri: String, name: String): String {
        // ⚠️ `createDocument` with a name that exists does NOT fail — providers de-duplicate, so
        // asking twice yields `Projects` and `Projects (1)`. The seven app folders are created on
        // first use on every launch, so this path runs constantly and must find before it creates.
        findChild(parentDocUri, name)?.let { return it }
        return runCatching {
            DocumentsContract.createDocument(
                context.contentResolver, Uri.parse(parentDocUri),
                DocumentsContract.Document.MIME_TYPE_DIR, name
            )?.toString() ?: ""
        }.getOrElse {
            Log.w(TAG, "saf: createDir('$name') failed under $parentDocUri: $it")
            ""
        }
    }

    /**
     * Create a FILE document under `parentDocUri`, returning its URI — or the EXISTING one if the
     * name is taken, exactly as [createDir] does for folders.
     *
     * ⚠️ **Find before create, for the same reason folders do**: `createDocument` de-duplicates rather
     * than failing, so asking twice for `song.ptp` yields `song.ptp` and `song.ptp (1)` and the second
     * save would land in a file the browser shows beside the first. The C++ side then opens the
     * returned URI `"wt"`, which truncates — so create-or-find plus truncate is a whole overwrite.
     *
     * The MIME type is deliberately `application/octet-stream` for everything. A provider is free to
     * infer a better one from the extension, and the app never reads it back: `list_files` decides
     * "is this a directory?" from `MIME_TYPE_DIR` alone, and the browser's own filter is the
     * extension. Guessing a type per suffix would be a second, worse copy of `path_extension`.
     */
    fun createFile(parentDocUri: String, name: String): String {
        findChild(parentDocUri, name)?.let { return it }
        return runCatching {
            DocumentsContract.createDocument(
                context.contentResolver, Uri.parse(parentDocUri), "application/octet-stream", name
            )?.toString() ?: ""
        }.getOrElse {
            Log.w(TAG, "saf: createFile('$name') failed under $parentDocUri: $it")
            ""
        }
    }

    /** Delete a document. A directory goes with everything under it, as `deleteFileOrFolder` does. */
    fun deleteDoc(docUri: String): Boolean = runCatching {
        DocumentsContract.deleteDocument(context.contentResolver, Uri.parse(docUri))
    }.getOrElse {
        Log.w(TAG, "saf: deleteDoc failed on $docUri: $it")
        false
    }

    /**
     * Rename a document, returning its URI afterwards — or "" on failure.
     *
     * ⚠️ **The URI may CHANGE**, because a provider is free to encode the display name into the
     * document id (`ExternalStorageProvider` does exactly that: `primary:Documents/a.ptp`). The
     * returned value is the authority on where the document now is; the old string is dead. A provider
     * that renames in place returns null having succeeded, so null falls back to the original URI
     * rather than being read as a failure — the two are told apart by the exception, not by the return.
     */
    fun renameDoc(docUri: String, newName: String): String = runCatching {
        DocumentsContract.renameDocument(context.contentResolver, Uri.parse(docUri), newName)
            ?.toString() ?: docUri
    }.getOrElse {
        Log.w(TAG, "saf: renameDoc('$newName') failed on $docUri: $it")
        ""
    }

    /**
     * Move a document between two directory documents, returning its new URI — or "" if the provider
     * will not do it.
     *
     * ⚠️ "" is NOT necessarily an error: `moveDocument` requires the source to carry
     * `FLAG_SUPPORTS_MOVE`, which a provider may simply not offer, and it throws where a document
     * crosses providers. The C++ side treats "" as "fall back to copy + delete" rather than as a
     * failed move — which is the same shape `StdFileSystem::move_file` already has for a `rename(2)`
     * that fails across filesystems.
     */
    fun moveDoc(docUri: String, fromParentDocUri: String, toParentDocUri: String): String = runCatching {
        DocumentsContract.moveDocument(
            context.contentResolver, Uri.parse(docUri), Uri.parse(fromParentDocUri),
            Uri.parse(toParentDocUri)
        )?.toString() ?: ""
    }.getOrElse {
        Log.w(TAG, "saf: moveDoc failed on $docUri: $it")
        ""
    }

    /** The child of `parentDocUri` whose display name is `name`, or null. */
    private fun findChild(parentDocUri: String, name: String): String? {
        for (line in listChildren(parentDocUri).lineSequence()) {
            if (line.isEmpty()) continue
            val fields = line.split('\t')
            if (fields.size >= 2 && fields[1] == name) return fields[0]
        }
        return null
    }

    /** One column of a single document, or null. */
    private fun queryRow(docUri: Uri, column: String): String? = runCatching {
        context.contentResolver.query(docUri, arrayOf(column), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) c.getString(0) else null
        }
    }.getOrNull()

    private companion object {
        // The same tag the rest of the Kotlin half logs under; `MainActivity`'s own is private to it.
        // ⚠️ `PocketTrackerSDL` is the KOTLIN tag — the native side logs under `PocketTracker`, and a
        // logcat filter on the wrong one reports a working migration as never having run.
        const val TAG = "PocketTrackerSDL"

        /** `MainActivity`'s prefs file, shared so the migration counters and this sit in one place. */
        const val PREFS = "pockettracker_ui"

        /** The persisted TREE uri of the home root. See [homeRootId] for why it is the uri, not the id. */
        const val HOME_TREE_KEY = "saf_home_tree_uri"

        const val EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents"
    }
}
