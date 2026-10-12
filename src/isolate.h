// PLAY-9 filtered listening: a JSFX on the audio track passes only the pitch of the voice being
// edited and its overtones, following its notes (or its suggestions where none are drawn).
#pragma once

#include <string>

// The JSFX (isolatefx.cpp, REAPER-free; run in tests/isolate_test.cpp). Its parameters: width
// (cents), background (dB), the number of changes, then ISOLATE_CHANGES (time, pitch) pairs.
inline constexpr int ISOLATE_CHANGES = 24; // sent ahead: about a phrase, refreshed every tick
inline constexpr const char *ISOLATE_DESC = "ReaTrace isolate";
std::string isolateJSFX();

bool isolateOn(); // the filter is on the audio track
void setIsolate(bool on);
void updateIsolate(); // timer: sends the filter the coming pitch changes
