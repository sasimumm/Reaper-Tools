// The ReaTrace editor window: spectrogram with a piano roll on top.
#pragma once

#include "reaper_plugin.h"

void Window_Open(MediaItem *audio, int voice); // voice 0 = lead
void Window_Close();
void Window_Timer();                 // called from REAPER's timer, ~30 Hz
int Window_TranslateAccel(MSG *msg); // accelerator hook, see accelerator_register_t

static const int WINDOW_VOICES = 4;

// Implemented in main.cpp: the MIDI item holding a voice's notes for an audio item (voice 0 is
// the lead), optionally created along with its track.
MediaItem *VoiceItem(MediaItem *audio, int voice, bool create);
