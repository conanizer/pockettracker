// native-audio.cpp — STUB (Phase 0 file split complete)
//
// This file has been split into focused modules:
//   filter.h          — Biquad coefficient math (kept as reference; SVF used by effects/)
//   audio-defs.h      — Log macros, constants (MAX_VOICES, DECLICK_SAMPLES, FX_*)
//   note-queue.h      — SoundfontEntry, NoteQueue/KillQueue, InstrumentParams, Table
//   mod-system.h      — ParamId, ParamBus, IAudioVoice
//   sampler-voice.h   — Voice struct (sampler, with VoiceModSlot)
//   soundfont-voice.h — SoundfontVoice struct (SF2 playback)
//   soundfont-voice.cpp — TSF_IMPLEMENTATION + SoundfontVoice method bodies
//   audio-engine.h    — AudioEngine class declaration
//   audio-engine.cpp  — AudioEngine method bodies + sfVoices[8] definition
//   jni-bridge.cpp    — static engine* + all JNIEXPORT functions
//
// DO NOT add code here. This file is intentionally empty.
