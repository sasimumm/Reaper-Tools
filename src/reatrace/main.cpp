// ReaTrace: a REAPER extension for transcribing audio by drawing notes on a spectrogram.
//
// "ReaTrace: Open spectrogram editor for selected item" works on an audio item, or on a MIDI
// item on a "ReaTrace notes" track. Notes are stored in a MIDI item on a "ReaTrace notes" track
// directly below the audio track, so they are saved with the project and can also be edited in
// REAPER's MIDI editor.

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_AddExtensionsMainMenu
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetTrackGUID
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
#define REAPERAPI_WANT_GetMediaSourceFileName
#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value
#define REAPERAPI_WANT_GetMediaItemTake_Track
#define REAPERAPI_WANT_GetMediaSourceNumChannels
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_GetPlayPosition2
#define REAPERAPI_WANT_MIDI_SetItemExtents
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_GetProjectStateChangeCount
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_GetSetMediaTrackInfo
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetTakeName
#define REAPERAPI_WANT_GetThemeColor
#define REAPERAPI_WANT_GetToggleCommandState
#define REAPERAPI_WANT_CSurf_OnPlayRateChange
#define REAPERAPI_WANT_Master_GetPlayRate
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetTrackName
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
#define REAPERAPI_WANT_MIDI_GetScale
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
#define REAPERAPI_WANT_TrackFX_Delete
#define REAPERAPI_WANT_TrackFX_GetCount
#define REAPERAPI_WANT_TrackFX_GetFXName
#define REAPERAPI_WANT_TrackFX_GetFXGUID
#define REAPERAPI_WANT_TrackFX_SetParam
#define REAPERAPI_WANT_RecursiveCreateDirectory
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_GetSet_LoopTimeRange2
#define REAPERAPI_WANT_GetSet_ArrangeView2
#define REAPERAPI_WANT_GetSetRepeat
#define REAPERAPI_WANT_CountTempoTimeSigMarkers
#define REAPERAPI_WANT_GetTempoTimeSigMarker
#define REAPERAPI_WANT_GetMediaItemTake_Item
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
#define REAPERAPI_WANT_EnumProjects
#define REAPERAPI_WANT_DockWindowActivate
#define REAPERAPI_WANT_ValidatePtr2
#define REAPERAPI_WANT_plugin_getapi
#include "index.h"
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include "shared/guard.h"
#include "shared/menu.h"
#include "ui/window.h"

// Voice v lives on the v-th track below the audio track, created as "ReaTrace notes" for the lead,
// then "ReaTrace voice 2", "ReaTrace voice 3", ... Tracks named before the rename ("Reascribe ...")
// are still recognised.
static std::string voiceTrackName(int voice, const std::string &prefix = "ReaTrace") {
    return voice == 0 ? prefix + " notes" : prefix + " voice " + std::to_string(voice + 1);
}

// The track's voice is kept in its extension data, so the track can be renamed. Tracks from
// before that are known by name, and get the tag when first found.
static const char *VOICE_EXT = "P_EXT:reatrace_voice";

static void tagVoiceTrack(MediaTrack *tr, int voice) {
    std::string v = std::to_string(voice);
    GetSetMediaTrackInfo_String(tr, VOICE_EXT, v.data(), true);
}

// The voice the track holds, or -1 if it isn't a ReaTrace voice track.
static int voiceOfTrack(MediaTrack *tr) {
    if (!tr) {
        return -1;
    }
    // read in place: the _String getter copies project data into a buffer it isn't told the size of
    if (const auto *ext = static_cast<const char *>(GetSetMediaTrackInfo(tr, VOICE_EXT, NULL));
        ext && *ext) {
        for (int v = 0; v < WINDOW_VOICES; v++) {
            if (std::string_view(ext) == std::to_string(v)) {
                return v;
            }
        }
        return -1;
    }
    char name[256] = "";
    if (!GetTrackName(tr, name, sizeof name)) {
        return -1;
    }
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if (name == voiceTrackName(v) || name == voiceTrackName(v, "Reascribe")) {
            tagVoiceTrack(tr, v);
            return v;
        }
    }
    return -1;
}

// The item overlapping [pos, pos + len) the most: notes items grow past their audio item's edges
// (to fit the notes), so the first overlapping one can be a neighbour's.
static MediaItem *overlappingItem(MediaTrack *tr, double pos, double len, bool midi) {
    MediaItem *best = NULL;
    double bestOverlap = 0;
    for (int i = 0; tr && i < CountTrackMediaItems(tr); i++) {
        MediaItem *it = GetTrackMediaItem(tr, i);
        const double p = GetMediaItemInfo_Value(it, "D_POSITION");
        if (p >= pos + len) {
            break; // a track's items are in position order
        }
        const double end = p + GetMediaItemInfo_Value(it, "D_LENGTH");
        const double overlap = (std::min)(end, pos + len) - (std::max)(p, pos);
        MediaItem_Take *tk = GetActiveTake(it);
        if (overlap > bestOverlap && tk && TakeIsMIDI(tk) == midi) {
            best = it;
            bestOverlap = overlap;
        }
    }
    return best;
}

// The voice's track among the ReaTrace tracks directly below the audio track, created (after the
// last existing voice track) if asked.
static MediaTrack *voiceTrack(MediaItem *audio, int voice, bool create) {
    // IP_TRACKNUMBER is 1-based, so it is the index of the track below
    MediaTrack *audioTrack = GetMediaItem_Track(audio);
    int idx = static_cast<int>(GetMediaTrackInfo_Value(audioTrack, "IP_TRACKNUMBER"));
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
    std::string name = voiceTrackName(voice);
    GetSetMediaTrackInfo_String(tr, "P_NAME", name.data(), true);
    tagVoiceTrack(tr, voice);
    TrackFX_AddByName(tr, "ReaSynth", false, -1); // to hear the transcription against the audio
    return tr;
}

// The voice's MIDI item over the audio item (see ui/window.h).
MediaItem *VoiceItem(MediaItem *audio, int voice, bool create) {
    const double pos = GetMediaItemInfo_Value(audio, "D_POSITION");
    const double len = GetMediaItemInfo_Value(audio, "D_LENGTH");
    MediaTrack *tr = voiceTrack(audio, voice, false);
    MediaItem *midi = tr ? overlappingItem(tr, pos, len, true) : NULL;
    if (midi || !create) {
        return midi;
    }

    Undo_BeginBlock2(NULL);
    if (!tr) {
        tr = voiceTrack(audio, voice, true);
    }
    const bool qn = false; // pos and len are in seconds
    midi = CreateNewMIDIItemInProj(tr, pos, pos + len, &qn);
    if (midi) {
        // timebase time: tempo edits don't move the item
        SetMediaItemInfo_Value(midi, "C_BEATATTACHMODE", 0);
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
    // IP_TRACKNUMBER is 1-based: - 2 is the index of the track above
    int idx = static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER")) - 2;
    while (idx >= 0 && voiceOfTrack(GetTrack(NULL, idx)) >= 0) {
        idx--;
    }

    const double pos = GetMediaItemInfo_Value(midi, "D_POSITION");
    const double len = GetMediaItemInfo_Value(midi, "D_LENGTH");
    return idx >= 0 ? overlappingItem(GetTrack(NULL, idx), pos, len, false) : NULL;
}

// "ReaTrace: Open spectrogram editor for selected item": on the selected audio item, or on the
// audio item above a selected MIDI item of a voice track (opening that voice).
static void openSelected() {
    MediaItem *item = GetSelectedMediaItem(NULL, 0);
    MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
    MediaItem *audio = NULL;
    if (take && !TakeIsMIDI(take)) {
        audio = item;
    } else if (take) {
        audio = audioForMidi(item);
    }
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

// See shared/guard.h.
void ReportFailure(const char *where, const char *what) noexcept {
    try {
        // a failure in the timer would otherwise repeat 30x a second
        static std::set<std::string> reported;
        // At most 511 characters, as the fixed buffer this was formatted into allowed.
        const std::string msg =
            (std::string("ReaTrace: ") + where + " failed: " + what + "\n").substr(0, 511);
        if (reported.insert(msg).second) {
            ShowConsoleMsg(msg.c_str());
        }
    } catch (...) {
        // out of memory while formatting: a fixed message, once
        static bool shown = false;
        if (!shown) {
            shown = true;
            ShowConsoleMsg("ReaTrace: a failure couldn't be reported (out of memory)\n");
        }
    }
}

static int g_cmdOpen; // command id of the open action

// REAPER's entry points (hookcommand2, accelerator, timer), each run under guarded().

static bool onAction(KbdSectionInfo *, int command, int, int, int, HWND) {
    return guarded("action", false, [&] {
        if (command != g_cmdOpen) {
            return false;
        }
        openSelected();
        return true;
    });
}

static int translateAccel(MSG *msg, accelerator_register_t *) {
    return guarded("key handling", 0, [&] { return Window_TranslateAccel(msg); });
}

static void timer() {
    guarded("timer", [] { Window_Timer(); });
}

// A ReaTrace submenu in the Extensions menu, and "Open in ReaTrace" in the item context menu (a
// right-click selects the item first, so the action opens that one).
static void menuHook(const char *menuidstr, void *menu, int flag) {
    guarded("menu", [&] {
        if (flag != 0 || !menuidstr) {
            return;
        }
        HMENU m = static_cast<HMENU>(menu);
        const std::string_view id = menuidstr;
        if (id == MENU_EXTENSIONS) {
            addMenuSubmenu(m, "ReaTrace", "Open spectrogram editor for selected item", g_cmdOpen);
        } else if (id == MENU_ITEM_CONTEXT) {
            addMenuSeparator(m);
            insertMenuCommand(m, -1, "Open in ReaTrace", g_cmdOpen);
        }
    });
}

static accelerator_register_t g_accel = {translateAccel, true, NULL};

// Called with rec on load and with NULL on unload; returns 1 if loaded.
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE,
                                                                 reaper_plugin_info_t *rec);
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
    rec->Register("hookcommand2", reinterpret_cast<void *>(onAction));
    rec->Register("accelerator", &g_accel);
    rec->Register("timer", reinterpret_cast<void *>(timer));
    rec->Register("hookcustommenu", reinterpret_cast<void *>(menuHook));
    AddExtensionsMainMenu();
    return 1;
}
