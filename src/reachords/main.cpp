// ReaChords: a REAPER extension for chord items, a track of chord names drawn and edited in the
// arrange view. "ReaChords: Insert chord item" adds one on the selected track.
//
// Other extensions read the project's chords through REAPER's API table:
//   int ReaChords_GetChord(int index, double *start, double *end, char *name, int nameSize)
// gives the index-th chord by start (project seconds, clipped to its item); 0 past the last one.

#include <cstdio>
#include <set>
#include <string>

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_AddMediaItemToTrack
#define REAPERAPI_WANT_AddTakeToMediaItem
#define REAPERAPI_WANT_CountMediaItems
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_GetMainHwnd
#define REAPERAPI_WANT_GetMediaItem
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value
#define REAPERAPI_WANT_GetProjectStateChangeCount
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_GetSet_LoopTimeRange2
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String
#define REAPERAPI_WANT_LICE_CreateFont
#define REAPERAPI_WANT_LICE_FillRect
#define REAPERAPI_WANT_LICE_Line
#define REAPERAPI_WANT_LICE__DrawText
#define REAPERAPI_WANT_LICE__SetFromHFont
#define REAPERAPI_WANT_LICE__SetTextColor
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_SetMediaItemTake_Source
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_ShowMessageBox
#define REAPERAPI_WANT_SnapToGrid
#define REAPERAPI_WANT_TimeMap2_QNToTime
#define REAPERAPI_WANT_TimeMap2_timeToQN
#define REAPERAPI_WANT_TimeMap_QNToMeasures
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_Undo_OnStateChange
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_ValidatePtr2
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include "chords.h"
#include "shared/guard.h"

void ReportFailure(const char *where, const char *what) {
    static std::set<std::string> reported; // a failure in the timer would otherwise repeat
    char msg[512];
    snprintf(msg, sizeof(msg), "ReaChords: %s failed: %s\n", where, what);
    if (reported.insert(msg).second) {
        ShowConsoleMsg(msg);
    }
}

static int getChord(int index, double *start, double *end, char *name, int nameSize) {
    return guarded("reading chords", 0, [&] {
        const std::vector<ChordSpan> &spans = Chords_Spans();
        if (index < 0 || index >= static_cast<int>(spans.size())) {
            return 0;
        }
        const ChordSpan &c = spans[static_cast<size_t>(index)];
        if (start) {
            *start = c.start;
        }
        if (end) {
            *end = c.end;
        }
        if (name && nameSize > 0) {
            snprintf(name, static_cast<size_t>(nameSize), "%s", c.name.c_str());
        }
        return 1;
    });
}

static bool onAction(KbdSectionInfo *, int command, int, int, int, HWND) {
    return guarded("action", false, [&] { return Chords_OnAction(command); });
}

static int translateAccel(MSG *msg, accelerator_register_t *) {
    return guarded("key handling", 0, [&] { return Chords_TranslateAccel(msg); });
}

static void timer() {
    guarded("timer", [] { Chords_Timer(); });
}
static accelerator_register_t g_accel = {translateAccel, true, NULL};

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE,
                                                                 reaper_plugin_info_t *rec);
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE,
                                                                 reaper_plugin_info_t *rec) {
    if (!rec) {
        return 0;
    }
    if (rec->caller_version != REAPER_PLUGIN_VERSION || REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        return 0;
    }
    Chords_Register(rec);
    rec->Register("API_ReaChords_GetChord", reinterpret_cast<void *>(getChord));
    rec->Register("hookcommand2", reinterpret_cast<void *>(onAction));
    rec->Register("accelerator", &g_accel);
    rec->Register("timer", reinterpret_cast<void *>(timer));
    return 1;
}
