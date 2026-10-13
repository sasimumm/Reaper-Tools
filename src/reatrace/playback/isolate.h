// PLAY-9 filtered listening: a JSFX on the audio track passes only the pitch of the voice being
// edited (or of every shown voice) and its overtones, following its notes (or its suggestions where
// none are drawn). Or the reverse: it lowers the drawn notes of every shown voice, so what is left
// is what isn't notated yet.
#pragma once

#include <string>

// The JSFX (isolatefx.cpp, REAPER-free; run in tests/isolate_test.cpp). Its parameters: width
// (cents), level (dB: of the rest, or of the notes when removing), remove (0 keeps the pitches, 1
// lowers them), then per voice the number of changes and ISOLATE_CHANGES (time, pitch) pairs. Needs REAPER 6.74 or newer (more than 64 sliders).
inline constexpr int ISOLATE_VOICES = 4;   // WINDOW_VOICES
inline constexpr int ISOLATE_CHANGES = 16; // per voice, sent ahead and refreshed every tick
inline constexpr const char *ISOLATE_DESC = "ReaTrace isolate";
// parameter index (from 0) of a voice's change count; its pairs follow
inline constexpr int isolateParam(int voice) { return 3 + voice * (1 + 2 * ISOLATE_CHANGES); }
std::string isolateJSFX();

bool isolateOn(); // the filter is on the audio track, in either mode
void setIsolate(bool on);
// Turns the filter off when it is on in this mode (Settings::isolateRemove), else on in it.
void toggleIsolate(bool remove);
void updateIsolate(); // timer: sends the filter the coming pitch changes
