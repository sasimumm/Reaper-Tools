// ReaTrace: a REAPER extension for transcribing audio by drawing notes on a spectrogram.
//
// "ReaTrace: Open spectrogram editor for selected item" works on an audio item, or on a MIDI
// item on a "ReaTrace notes" track. Notes are stored in a MIDI item on a "ReaTrace notes" track
// directly below the audio track, so they are saved with the project and can also be edited in
// REAPER's MIDI editor.

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_CreateNewMIDIItemInProj
#define REAPERAPI_WANT_CreateTakeAudioAccessor
#define REAPERAPI_WANT_DestroyAudioAccessor
#define REAPERAPI_WANT_format_timestr_pos
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetAudioAccessorSamples
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_GetMainHwnd
#define REAPERAPI_WANT_GetMediaItem_Track
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value
#define REAPERAPI_WANT_GetMediaItemTake_Track
#define REAPERAPI_WANT_GetMediaSourceNumChannels
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_GetPlayPosition2
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_GetProjectStateChangeCount
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetTakeName
#define REAPERAPI_WANT_GetThemeColor
#define REAPERAPI_WANT_GetToggleCommandState
#define REAPERAPI_WANT_CSurf_OnPlayRateChange
#define REAPERAPI_WANT_Master_GetPlayRate
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_MIDI_CountEvts
#define REAPERAPI_WANT_MIDI_DeleteNote
#define REAPERAPI_WANT_MIDI_GetHash
#define REAPERAPI_WANT_MIDI_GetNote
#define REAPERAPI_WANT_MIDI_GetPPQPosFromProjTime
#define REAPERAPI_WANT_MIDI_GetProjTimeFromPPQPos
#define REAPERAPI_WANT_MIDI_InsertNote
#define REAPERAPI_WANT_MIDI_SetNote
#define REAPERAPI_WANT_MIDI_Sort
#define REAPERAPI_WANT_MIDI_GetTextSysexEvt
#define REAPERAPI_WANT_MIDI_InsertTextSysexEvt
#define REAPERAPI_WANT_MIDI_DeleteTextSysexEvt
#define REAPERAPI_WANT_PCM_Source_CreateFromFile
#define REAPERAPI_WANT_PlayTrackPreview2Ex
#define REAPERAPI_WANT_StopTrackPreview2
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_ShowMessageBox
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_TakeIsMIDI
#define REAPERAPI_WANT_time_precise
#define REAPERAPI_WANT_TimeMap2_QNToTime
#define REAPERAPI_WANT_TimeMap2_timeToQN
#define REAPERAPI_WANT_TimeMap_QNToMeasures
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_GetSet_LoopTimeRange2
#define REAPERAPI_WANT_GetSet_ArrangeView2
#define REAPERAPI_WANT_GetSetRepeat
#define REAPERAPI_WANT_SetMediaTrackInfo_Value
#define REAPERAPI_WANT_TrackList_AdjustWindows
#define REAPERAPI_WANT_OnPlayButton
#define REAPERAPI_WANT_OnStopButton
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_GetProjExtState
#define REAPERAPI_WANT_SetProjExtState
#define REAPERAPI_WANT_GetSetMediaItemInfo
#define REAPERAPI_WANT_guidToString
#define REAPERAPI_WANT_DockWindowAddEx
#define REAPERAPI_WANT_DockWindowRemove
#define REAPERAPI_WANT_DockWindowActivate
#define REAPERAPI_WANT_ValidatePtr2
#define REAPERAPI_WANT_AddMediaItemToTrack
#define REAPERAPI_WANT_AddTakeToMediaItem
#define REAPERAPI_WANT_CountMediaItems
#define REAPERAPI_WANT_GetMediaItem
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String
#define REAPERAPI_WANT_LICE_CreateFont
#define REAPERAPI_WANT_LICE_FillRect
#define REAPERAPI_WANT_LICE_Line
#define REAPERAPI_WANT_LICE__DrawText
#define REAPERAPI_WANT_LICE__GetHeight
#define REAPERAPI_WANT_LICE__GetWidth
#define REAPERAPI_WANT_LICE__SetFromHFont
#define REAPERAPI_WANT_LICE__SetTextColor
#define REAPERAPI_WANT_SetMediaItemTake_Source
#define REAPERAPI_WANT_SnapToGrid
#define REAPERAPI_WANT_Undo_OnStateChange
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include "chords.h"
#include "guard.h"
#include "window.h"

// Voice v lives on the v-th track below the audio track: "ReaTrace notes" for the lead, then
// "ReaTrace voice 2", "ReaTrace voice 3", ... Tracks named before the rename ("Reascribe ...")
// are still recognised.
static void voiceTrackName(int voice, char *buf, int sz, const char *prefix = "ReaTrace") {
    if (voice == 0) {
        snprintf(buf, sz, "%s notes", prefix);
    } else {
        snprintf(buf, sz, "%s voice %d", prefix, voice + 1);
    }
}

// -1 if the track isn't a ReaTrace voice track
static int voiceOfTrack(MediaTrack *tr) {
    char name[256] = "", want[64];
    if (!tr || !GetSetMediaTrackInfo_String(tr, "P_NAME", name, false)) {
        return -1;
    }
    for (int v = 0; v < WINDOW_VOICES; v++) {
        voiceTrackName(v, want, sizeof(want));
        if (!strcmp(name, want)) {
            return v;
        }
        voiceTrackName(v, want, sizeof(want), "Reascribe");
        if (!strcmp(name, want)) {
            return v;
        }
    }
    return -1;
}

static MediaItem *overlappingItem(MediaTrack *tr, double pos, double len, bool midi) {
    for (int i = 0; tr && i < CountTrackMediaItems(tr); i++) {
        MediaItem *it = GetTrackMediaItem(tr, i);
        const double p = GetMediaItemInfo_Value(it, "D_POSITION"),
                     l = GetMediaItemInfo_Value(it, "D_LENGTH");
        MediaItem_Take *tk = GetActiveTake(it);
        if (tk && TakeIsMIDI(tk) == midi && p < pos + len && p + l > pos) {
            return it;
        }
    }
    return NULL;
}

// The voice's track among the ReaTrace tracks directly below the audio track, created (after the
// last existing voice track) if asked.
static MediaTrack *voiceTrack(MediaItem *audio, int voice, bool create) {
    const int first =
        (int)GetMediaTrackInfo_Value(GetMediaItem_Track(audio), "IP_TRACKNUMBER"); // = index below
    int idx = first;
    for (MediaTrack *tr = NULL; (tr = GetTrack(NULL, idx)) && voiceOfTrack(tr) >= 0; idx++) {
        if (voiceOfTrack(tr) == voice) {
            return tr;
        }
    }
    if (!create) {
        return NULL;
    }
    InsertTrackAtIndex(idx, true);
    MediaTrack *tr = GetTrack(NULL, idx);
    char name[64];
    voiceTrackName(voice, name, sizeof(name));
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, true);
    TrackFX_AddByName(tr, "ReaSynth", false, -1); // to hear the transcription against the audio
    return tr;
}

MediaItem *VoiceItem(MediaItem *audio, int voice, bool create) {
    const double pos = GetMediaItemInfo_Value(audio, "D_POSITION"),
                 len = GetMediaItemInfo_Value(audio, "D_LENGTH");
    MediaTrack *tr = voiceTrack(audio, voice, false);
    MediaItem *midi = tr ? overlappingItem(tr, pos, len, true) : NULL;
    if (midi || !create) {
        return midi;
    }
    Undo_BeginBlock2(NULL);
    if (!tr) {
        tr = voiceTrack(audio, voice, true);
    }
    const bool qn = false;
    midi = CreateNewMIDIItemInProj(tr, pos, pos + len, &qn);
    if (midi) {
        SetMediaItemInfo_Value(midi, "C_BEATATTACHMODE",
                               0); // timebase time: tempo edits don't move the item
    }
    Undo_EndBlock2(NULL, "ReaTrace: create notes item", -1);
    UpdateArrange();
    return midi;
}

// The audio item above a ReaTrace MIDI item: on the first track above its voice tracks.
static MediaItem *audioForMidi(MediaItem *midi) {
    MediaTrack *tr = GetMediaItem_Track(midi);
    if (voiceOfTrack(tr) < 0) {
        return NULL;
    }
    int idx = (int)GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") - 2;
    while (idx >= 0 && voiceOfTrack(GetTrack(NULL, idx)) >= 0) {
        idx--;
    }
    const double pos = GetMediaItemInfo_Value(midi, "D_POSITION"),
                 len = GetMediaItemInfo_Value(midi, "D_LENGTH");
    return idx >= 0 ? overlappingItem(GetTrack(NULL, idx), pos, len, false) : NULL;
}

static void openSelected() {
    MediaItem *item = GetSelectedMediaItem(NULL, 0);
    MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
    MediaItem *audio = take && !TakeIsMIDI(take) ? item : take ? audioForMidi(item) : NULL;
    if (!audio) {
        ShowMessageBox("Select an audio item (or a MIDI item on a \"ReaTrace notes\" track) first.",
                       "ReaTrace", 0);
        return;
    }
    const int voice = take && TakeIsMIDI(take) ? voiceOfTrack(GetMediaItem_Track(item)) : 0;
    if (voice == 0) {
        VoiceItem(audio, 0, true); // the lead's track and item, so there is somewhere to listen
    }
    Window_Open(audio, voice);
}

void ReportFailure(const char *where, const char *what) {
    static std::set<std::string>
        reported; // a failure in the timer would otherwise repeat 30x a second
    char msg[512];
    snprintf(msg, sizeof(msg), "ReaTrace: %s failed: %s\n", where, what);
    if (reported.insert(msg).second) {
        ShowConsoleMsg(msg);
    }
}

static int g_cmdOpen;

static bool onAction(KbdSectionInfo *, int command, int, int, int, HWND) {
    return guarded("action", false, [&] {
        if (Chords_OnAction(command)) {
            return true;
        }
        if (command != g_cmdOpen) {
            return false;
        }
        openSelected();
        return true;
    });
}

static int translateAccel(MSG *msg, accelerator_register_t *) {
    return guarded("key handling", 0, [&] {
        if (int r = Chords_TranslateAccel(msg)) {
            return r;
        }
        return Window_TranslateAccel(msg);
    });
}

static void timer() {
    guarded("timer", [] {
        Chords_Timer();
        Window_Timer();
    });
}
static accelerator_register_t g_accel = {translateAccel, true, NULL};

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE,
                                                                 reaper_plugin_info_t *rec) {
    if (!rec) {
        guarded("unload", [] { Window_Close(); });
        return 0;
    }
    if (rec->caller_version != REAPER_PLUGIN_VERSION || REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        return 0;
    }

    custom_action_register_t a = {
        0, "REATRACE_OPEN", "ReaTrace: Open spectrogram editor for selected item", NULL
    };
    if (!(g_cmdOpen = rec->Register("custom_action", &a))) {
        return 0;
    }
    Chords_Register(rec);
    rec->Register("hookcommand2", (void *)onAction);
    rec->Register("accelerator", &g_accel);
    rec->Register("timer", (void *)timer);
    return 1;
}
