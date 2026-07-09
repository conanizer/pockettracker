package com.conanizer.pockettracker.platform.android

import android.content.Context
import com.conanizer.pockettracker.core.resources.IResourceLoader
import com.conanizer.pockettracker.core.resources.SampleData

/**
 * Android implementation of IResourceLoader.
 *
 * Inert stub: the app ships no bundled default samples (users load their own via the
 * file browser), so there is nothing to map resource names to. The interface stays —
 * it is part of the core/platform port seam. If bundled samples ever return, route the
 * decoding through the native decoders (loadSampleFromWav / loadSampleFromCompressed)
 * rather than reintroducing a Kotlin WAV parser: the naive fixed-44-byte-header parser
 * that used to live here misparsed WAVs with extended fmt or LIST chunks.
 *
 * @param context Android context for accessing resources (unused while the stub is inert)
 */
class AndroidResourceLoader(
    @Suppress("unused") private val context: Context
) : IResourceLoader {

    override fun loadWav(name: String): SampleData {
        throw IllegalArgumentException("No bundled samples: unknown resource '$name'")
    }
}
