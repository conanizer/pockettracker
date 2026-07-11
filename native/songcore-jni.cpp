// ─── songcore ⟷ Android: the JNI shell (event-schema §7) ─────────────────────────────────────────
//
// The Android platform shell for songcore, and the ONLY file in songcore's world that knows JNI
// exists. It marshals arguments and forwards to songcore::SongcoreHost (songcore/host.h) — no
// sequencing decision, no musical logic, nothing that could make the C++ engine sound different from
// the Kotlin one. The Linux/SDL shell replaces exactly this file and nothing else.
//
// Kept as its own translation unit for a second reason: CMakeLists builds the library with
// -ffast-math on ARM, which event-schema §5 forbids for songcore (fma contraction of `a*b+c` changes
// binary32 rounding, and the trace's float fields are compared as raw bits). This TU — the only one
// that instantiates songcore's scheduling math — is exempted there with -fno-fast-math
// -ffp-contract=off. Adding songcore headers to any other TU would silently break that guarantee.

#include <jni.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "songcore/host.h"

// The engine instance lives in jni-bridge.cpp (created with the audio stream). songcore needs it for
// the transport clock, the live-edit queue clear, and the master-EQ restore.
extern AudioEngine* pt_engine();

namespace {
std::unique_ptr<songcore::SongcoreHost> g_host;
}  // namespace

#define SONGCORE_FN(name) \
    Java_com_conanizer_pockettracker_platform_android_AndroidSongcore_native_1##name

extern "C" {

JNIEXPORT void JNICALL SONGCORE_FN(create)(JNIEnv*, jobject) {
    AudioEngine* engine = pt_engine();
    const int sampleRate = engine ? engine->getSampleRate() : 44100;
    g_host = std::make_unique<songcore::SongcoreHost>(engine, sampleRate);
}

JNIEXPORT void JNICALL SONGCORE_FN(destroy)(JNIEnv*, jobject) {
    g_host.reset();
}

// ── ↓ data ───────────────────────────────────────────────────────────────────────────────────────
// The blob is FileController.serializeProject() as raw UTF-8 BYTES, not a jstring: JNI strings are
// *modified* UTF-8, which differs from real UTF-8 for supplementary characters (an emoji in a project
// or sample name), and the header's project= sha must equal EventTrace.projectSha1, which hashes real
// UTF-8. Bytes also skip a 440 KB transcoding pass. (This is the loadTable ByteArray precedent.)
JNIEXPORT jboolean JNICALL SONGCORE_FN(pushProject)(JNIEnv* env, jobject, jbyteArray blob) {
    if (!g_host || !blob) return JNI_FALSE;
    const jsize len = env->GetArrayLength(blob);
    std::string bytes(static_cast<size_t>(len), '\0');
    env->GetByteArrayRegion(blob, 0, len, reinterpret_cast<jbyte*>(&bytes[0]));
    return g_host->push_project(bytes) ? JNI_TRUE : JNI_FALSE;
}

// The two per-instrument facts songcore cannot derive because it never opens a file: the sample-rate
// ratio (deviceRate / fileRate — the Kotlin loaders compute it) and the SF2 slot the instrument's
// soundfontPath resolved to. Pushed with the project, from the same call site, so they cannot drift.
JNIEXPORT void JNICALL SONGCORE_FN(pushRouting)(JNIEnv* env, jobject,
                                                jfloatArray sampleRateRatios, jintArray sfSlots) {
    if (!g_host || !sampleRateRatios || !sfSlots) return;
    const jsize n = std::min(env->GetArrayLength(sampleRateRatios), env->GetArrayLength(sfSlots));
    if (n <= 0) return;

    std::vector<jfloat> ratios(static_cast<size_t>(n));
    std::vector<jint>   slots(static_cast<size_t>(n));
    env->GetFloatArrayRegion(sampleRateRatios, 0, n, ratios.data());
    env->GetIntArrayRegion(sfSlots, 0, n, slots.data());
    g_host->push_routing(ratios.data(), slots.data(), static_cast<int>(n));
}

// ── ↓ transport ──────────────────────────────────────────────────────────────────────────────────
// Each play verb returns the frame the transport latched, so the caller can stamp its own trace
// session with the SAME base instead of re-reading a clock that has already moved on.
JNIEXPORT jlong JNICALL SONGCORE_FN(playSong)(JNIEnv*, jobject, jint startRow) {
    return g_host ? static_cast<jlong>(g_host->play_song(startRow)) : 0;
}

JNIEXPORT jlong JNICALL SONGCORE_FN(playChain)(JNIEnv*, jobject, jint chainId) {
    return g_host ? static_cast<jlong>(g_host->play_chain(chainId)) : 0;
}

JNIEXPORT jlong JNICALL SONGCORE_FN(playPhrase)(JNIEnv*, jobject, jint phraseId) {
    return g_host ? static_cast<jlong>(g_host->play_phrase(phraseId)) : 0;
}

JNIEXPORT void JNICALL SONGCORE_FN(stop)(JNIEnv*, jobject) {
    if (g_host) g_host->stop();
}

JNIEXPORT void JNICALL SONGCORE_FN(poll)(JNIEnv*, jobject) {
    if (g_host) g_host->poll();
}

JNIEXPORT jboolean JNICALL SONGCORE_FN(isPlaying)(JNIEnv*, jobject) {
    return (g_host && g_host->is_playing()) ? JNI_TRUE : JNI_FALSE;
}

// AudioEngine.phraseTrackMask's twin — which tracks have had a note scheduled (OCTA scope lanes).
JNIEXPORT jint JNICALL SONGCORE_FN(getTrackMask)(JNIEnv*, jobject) {
    return g_host ? static_cast<jint>(g_host->track_mask()) : 0;
}

// ── ↓ the render path ────────────────────────────────────────────────────────────────────────────
// trackFilter == null renders every track (RenderController.scheduleSongForRender); a non-null array
// is the stem/selection filter (scheduleSelectionForRender). Returns the total frame span scheduled.
JNIEXPORT jlong JNICALL SONGCORE_FN(scheduleSongRange)(JNIEnv* env, jobject, jint startRow, jint endRow,
                                                       jintArray trackFilter) {
    if (!g_host) return 0;
    if (!trackFilter) return static_cast<jlong>(g_host->schedule_song_range(startRow, endRow, nullptr));

    const jsize n = env->GetArrayLength(trackFilter);
    std::vector<jint> ids(static_cast<size_t>(n));
    if (n > 0) env->GetIntArrayRegion(trackFilter, 0, n, ids.data());
    std::set<int> filter(ids.begin(), ids.end());
    return static_cast<jlong>(g_host->schedule_song_range(startRow, endRow, &filter));
}

// ── ↑ live-edit reaction ─────────────────────────────────────────────────────────────────────────
JNIEXPORT void JNICALL SONGCORE_FN(notifyDataChanged)(JNIEnv*, jobject) {
    if (g_host) g_host->notify_data_changed();
}

// ── ↑ feedback: the playheads (event-schema §7; never goldened — SC-4) ───────────────────────────
// int[4] = {row, chainRow, phraseStep, songRow} — PlaybackController.PlaybackPosition's four fields,
// which is exactly what the UI cursor reads.
JNIEXPORT jintArray JNICALL SONGCORE_FN(getPlayheads)(JNIEnv* env, jobject) {
    jint out[4] = {0, 0, 0, 0};
    if (g_host) {
        const songcore::PlaybackPosition p = g_host->playheads();
        out[0] = p.row;
        out[1] = p.chainRow;
        out[2] = p.phraseStep;
        out[3] = p.songRow;
    }
    jintArray arr = env->NewIntArray(4);
    if (arr) env->SetIntArrayRegion(arr, 0, 4, out);
    return arr;
}

// ── ↑ debug: the conformance trace ───────────────────────────────────────────────────────────────
// Writes the same schema-v1 text the Kotlin EventTrace tap writes, to the same path — so the S1
// device cross-check compares a C++-engine trace against the goldens with no change of procedure.
// The project must already be pushed: the header's project= is the sha of the pushed blob.
JNIEXPORT void JNICALL SONGCORE_FN(setTrace)(JNIEnv* env, jobject, jboolean enabled, jstring path) {
    if (!g_host) return;
    std::string p;
    if (path) {
        const char* c = env->GetStringUTFChars(path, nullptr);
        if (c) {
            p = c;
            env->ReleaseStringUTFChars(path, c);
        }
    }
    g_host->set_trace(enabled == JNI_TRUE, p);
}

}  // extern "C"
