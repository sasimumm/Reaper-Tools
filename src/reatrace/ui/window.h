// The ReaTrace editor window: spectrogram with a piano roll on top.
#pragma once

#include "reaper_plugin.h"

// Opens the editor on `audio`, editing `voice` (0 = lead); an editor already on that item only
// switches to the voice and comes to the front.
void Window_Open(MediaItem *audio, int voice);
void Window_Close();
void Window_Timer();                 // called from REAPER's timer, ~30 Hz
int Window_TranslateAccel(MSG *msg); // accelerator hook, see accelerator_register_t

static const int WINDOW_VOICES = 4; // lead plus three harmony/backing voices

// Implemented in main.cpp: the MIDI item holding a voice's notes for an audio item (voice 0 is
// the lead), optionally created along with its track.
MediaItem *VoiceItem(MediaItem *audio, int voice, bool create);
