// The Reascribe editor window: spectrogram with a piano roll on top.
#pragma once

#include "reaper_plugin.h"

void Window_Open(MediaItem *audio, MediaItem *midi);
void Window_Close();
void Window_Timer();                // called from REAPER's timer, ~30 Hz
int Window_TranslateAccel(MSG *msg); // accelerator hook, see accelerator_register_t

// Implemented in main.cpp: the MIDI item that holds the notes for an audio item, created if missing.
MediaItem *EnsureMidiItem(MediaItem *audio);
