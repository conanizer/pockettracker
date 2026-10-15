package com.conanizer.pockettracker

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import java.security.MessageDigest

/**
 * The Storage Access Framework primitives, and nothing else: `ContentResolver` and
 * `DocumentsContract` are Java-only, so the C++ `SafFileSystem` calls through this. It knows nothing
 * of `pt://` paths, the app folders or the home choice's meaning — that logic lives in C++.
 *
 * ⚠️ Every public method is resolved by name over JNI (through `MainActivity`'s wrappers): `@Keep`
 * there and a rule in `proguard-rules.pro`, or only a release build breaks.
 */
class SafStorage(private val context: Context) {

    /**
     * A granted tree: its derived id, display name, root document URI, the persisted tree URI the id
     * comes from (what [homeRootId] stores), and whether its folder is still there.
     * ⚠️ A grant outlives its folder: delete the directory or unmount the card and the grant stays,
     * well-formed. Only [live] tells; without it a dead tree could be chosen as home.
     */
    data class Root(val id: String, val displayName: String, val docUri: String, val treeUri: String,
                    val live: Boolean)

    // The granted trees, ordered by id — `persistedUriPermissions` has no guaranteed order.
    fun roots(): List<Root> {
        val out = ArrayList<Root>()
        for (p in context.contentResolver.persistedUriPermissions) {
            if (!p.isReadPermission) continue
            val tree = p.uri
            val docId = runCatching { DocumentsContract.getTreeDocumentId(tree) }.getOrNull() ?: continue
            val docUri = DocumentsContract.buildDocumentUriUsingTree(tree, docId)
            out += Root(rootId(tree), displayNameOf(docUri, docId), docUri.toString(), tree.toString(),
                        isLive(docUri))
        }
        out.sortBy { it.id }

        // A collision (48 bits over at most 512 grants) is logged rather than resolved arbitrarily.
        for (i in 1 until out.size) {
            if (out[i].id == out[i - 1].id) {
                Log.e(TAG, "saf: ROOT ID COLLISION ${out[i].id} - " +
                        "'${out[i - 1].displayName}' and '${out[i].displayName}' are indistinguishable")
            }
        }
        return out
    }

    // 12 hex characters of SHA-256 over the tree URI: derived, never stored, so any boot re-derives it.
    private fun rootId(tree: Uri): String =
        MessageDigest.getInstance("SHA-256")
            .digest(tree.toString().toByteArray())
            .take(6)
            .joinToString("") { "%02x".format(it) }

    // ⚠️ Asked by `COLUMN_DOCUMENT_ID`, not the name: every document has an id, while a provider may
    // offer no name. A throw (a deleted document) reads as gone too.
    private fun isLive(docUri: Uri): Boolean =
        queryRow(docUri, DocumentsContract.Document.COLUMN_DOCUMENT_ID) != null

    /** What the roots directory shows for a tree: the provider's display name, else the tail of its id. */
    private fun displayNameOf(docUri: Uri, docId: String): String {
        queryRow(docUri, DocumentsContract.Document.COLUMN_DISPLAY_NAME)?.let {
            if (it.isNotEmpty()) return clean(it)
        }
        // `primary:Documents/PocketTracker` → `PocketTracker`; never dropped for want of a name.
        return clean(docId.substringAfterLast('/').substringAfterLast(':').ifEmpty { "ROOT" })
    }

    fun rootCount(): Int = roots().size

    /**
     * The tree the app's folders live in, or "" if none is live.
     * ⚠️ The one piece of SAF state that is stored: a rule over the grants would move the app's
     * folders the day the user grants an unrelated one. The tree URI is stored, not the id.
     * First grant wins until it is gone or its folder deleted; then the lowest live id, re-stamped.
     */
    fun homeRootId(): String {
        val live = roots().filter { it.live }
        if (live.isEmpty()) return ""

        val prefs  = prefs()
        val stored = prefs.getString(HOME_TREE_KEY, null)
        live.firstOrNull { it.treeUri == stored }?.let { return it.id }

        val chosen = live.first()
        prefs.edit().putString(HOME_TREE_KEY, chosen.treeUri).apply()
        Log.i(TAG, "saf: home root = pt://${chosen.id} '${chosen.displayName}' " +
                   if (stored == null) "(first grant)" else "(previous home $stored is gone or empty)")
        return chosen.id
    }

    /**
     * The user's own choice of home, stored exactly as [homeRootId] reads it. True iff stored; a dead
     * tree is refused.
     */
    fun setHomeRoot(id: String): Boolean {
        val root = roots().firstOrNull { it.id == id } ?: return false
        if (!root.live) {
            Log.w(TAG, "saf: refusing pt://$id '${root.displayName}' as home - its folder is gone")
            return false
        }
        prefs().edit().putString(HOME_TREE_KEY, root.treeUri).apply()
        Log.i(TAG, "saf: home root = pt://$id '${root.displayName}' (chosen by the user)")
        return true
    }

    /**
     * Opens the system folder picker. True = it is on screen; the grant arrives in [takeGrant].
     * ⚠️ UI thread only — [MainActivity.safRequestRoot] posts it. Android forbids granting a volume
     * root or `Download` itself.
     */
    fun requestRoot(activity: Activity, requestCode: Int): Boolean {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        // Open at the app's folder rather than Recents. A hint only (API 29+); nothing relies on it.
        runCatching {
            DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE_AUTHORITY, "primary:Documents/PocketTracker")
        }.onSuccess { intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, it) }

        return runCatching {
            activity.startActivityForResult(intent, requestCode)
            true
        }.getOrElse {
            // No documents provider at all; the browser says so on its status line.
            Log.e(TAG, "saf: no activity can handle ACTION_OPEN_DOCUMENT_TREE: $it")
            false
        }
    }

    /**
     * Persists the grant the picker returned. False = cancelled, or it could not be taken.
     * ⚠️ Without `takePersistableUriPermission` the grant dies with the activity and the browser is
     * empty on the next launch.
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

    /**
     * Releases the grant on tree [id]. Deletes nothing; granting it again restores the same id.
     * Clears the stored home if it was this tree, so the next [homeRootId] is a clean first choice.
     */
    fun forgetRoot(id: String): Boolean {
        val root = roots().firstOrNull { it.id == id } ?: return false
        return runCatching {
            context.contentResolver.releasePersistableUriPermission(
                Uri.parse(root.treeUri),
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
            val prefs = prefs()
            if (prefs.getString(HOME_TREE_KEY, null) == root.treeUri) {
                prefs.edit().remove(HOME_TREE_KEY).apply()
            }
            Log.i(TAG, "saf: forgot pt://$id '${root.displayName}' (${rootCount()} left)")
            true
        }.getOrElse {
            Log.w(TAG, "saf: releasePersistableUriPermission failed on ${root.treeUri}: $it")
            false
        }
    }

    /** `<id>\t<displayName>\t<docUri>\t<live 0|1>`, or "" for an index that is no longer there. */
    fun rootInfo(index: Int): String {
        val r = roots().getOrNull(index) ?: return ""
        return "${r.id}\t${r.displayName}\t${r.docUri}\t${if (r.live) 1 else 0}"
    }

    // Every child in ONE query, one line each: `docUri \t name \t isDir(0|1) \t size \t lastModified`.
    // One JNI string, because a call per child makes a 200-entry folder slow.
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

    // ⚠️ A tab or newline in a name would shift the record's fields, so they become spaces — Android
    // forbids only `/`.
    private fun clean(s: String): String =
        if (s.indexOf('\t') < 0 && s.indexOf('\n') < 0) s else s.replace('\t', ' ').replace('\n', ' ')

    /** Whole document → bytes, or null. */
    fun readFile(docUri: String): ByteArray? = runCatching {
        context.contentResolver.openInputStream(Uri.parse(docUri))?.use { it.readBytes() }
    }.getOrElse {
        Log.w(TAG, "saf: readFile failed on $docUri: $it")
        null
    }

    /**
     * An OS descriptor for a document, or -1 — `pt_fopen`'s hook.
     * ⚠️ `detachFd()`, never `getFd()`: the caller owns and closes it, and a descriptor the
     * `ParcelFileDescriptor` still tracks would be closed twice — the second time someone else's.
     */
    fun openFd(docUri: String, mode: String): Int = runCatching {
        context.contentResolver.openFileDescriptor(Uri.parse(docUri), mode)?.detachFd() ?: -1
    }.getOrElse {
        Log.w(TAG, "saf: openFd($mode) failed on $docUri: $it")
        -1
    }

    /** Creates a sub-directory, returning its URI — or the existing one if the name is taken. */
    fun createDir(parentDocUri: String, name: String): String =
        findOrCreate(parentDocUri, name, DocumentsContract.Document.MIME_TYPE_DIR, "createDir")

    /**
     * Creates a file, or finds the existing one, as [createDir] does — the C++ side opens it `"wt"`,
     * so this plus the truncate is an overwrite. Always `application/octet-stream`: the app reads
     * only the extension.
     */
    fun createFile(parentDocUri: String, name: String): String =
        findOrCreate(parentDocUri, name, "application/octet-stream", "createFile")

    // ⚠️ Find first: `createDocument` on a taken name de-duplicates (`Projects (1)`) instead of failing.
    private fun findOrCreate(parentDocUri: String, name: String, mime: String, what: String): String {
        findChild(parentDocUri, name)?.let { return it }
        return runCatching {
            DocumentsContract.createDocument(context.contentResolver, Uri.parse(parentDocUri), mime, name)
                ?.toString() ?: ""
        }.getOrElse {
            Log.w(TAG, "saf: $what('$name') failed under $parentDocUri: $it")
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
     * Renames a document, returning its URI afterwards, or "" on failure.
     * ⚠️ The URI may change (a provider may encode the name in the id). A null return is a rename in
     * place, so it keeps the old URI.
     */
    fun renameDoc(docUri: String, newName: String): String = runCatching {
        DocumentsContract.renameDocument(context.contentResolver, Uri.parse(docUri), newName)
            ?.toString() ?: docUri
    }.getOrElse {
        Log.w(TAG, "saf: renameDoc('$newName') failed on $docUri: $it")
        ""
    }

    /**
     * Moves a document between two directories, returning its new URI. "" means the provider will not
     * (no `FLAG_SUPPORTS_MOVE`, or across providers): the C++ side then copies and deletes.
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

    private fun prefs() = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** One column of a single document, or null. */
    private fun queryRow(docUri: Uri, column: String): String? = runCatching {
        context.contentResolver.query(docUri, arrayOf(column), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) c.getString(0) else null
        }
    }.getOrNull()

    private companion object {
        // ⚠️ The native side logs under `PocketTracker`.
        const val TAG = "PocketTrackerSDL"

        /** `MainActivity`'s prefs file, shared. */
        const val PREFS = "pockettracker_ui"

        /** The home root's persisted tree URI. */
        const val HOME_TREE_KEY = "saf_home_tree_uri"

        const val EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents"
    }
}
