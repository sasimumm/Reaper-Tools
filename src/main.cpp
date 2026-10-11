// Reascribe: a REAPER extension for transcribing audio by drawing notes on a spectrogram.
//
// "Reascribe: Open spectrogram editor for selected item" works on an audio item, or on a MIDI
// item on a "Reascribe notes" track. Notes are stored in a MIDI item on a "Reascribe notes" track
// directly below the audio track, so they are saved with the project and can also be edited in
// REAPER's MIDI editor.

#include <cstdio>
#include <cstring>

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
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetTakeName
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_MIDI_CountEvts
#define REAPERAPI_WANT_MIDI_DeleteNote
#define REAPERAPI_WANT_MIDI_GetHash
#define REAPERAPI_WANT_MIDI_GetNote
#define REAPERAPI_WANT_MIDI_GetPPQPosFromProjTime
#define REAPERAPI_WANT_MIDI_GetProjTimeFromPPQPos
#define REAPERAPI_WANT_MIDI_InsertNote
#define REAPERAPI_WANT_MIDI_SetNote
#define REAPERAPI_WANT_MIDI_Sort
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_ShowMessageBox
#define REAPERAPI_WANT_TakeIsMIDI
#define REAPERAPI_WANT_TimeMap2_QNToTime
#define REAPERAPI_WANT_TimeMap2_timeToQN
#define REAPERAPI_WANT_TimeMap_QNToMeasures
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_ValidatePtr2
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include "window.h"

static const char *NOTES_TRACK_NAME = "Reascribe notes";

static bool isNotesTrack(MediaTrack *tr)
{
  char name[256] = "";
  return tr && GetSetMediaTrackInfo_String(tr, "P_NAME", name, false) && !strcmp(name, NOTES_TRACK_NAME);
}

static MediaItem *overlappingItem(MediaTrack *tr, double pos, double len, bool midi)
{
  for (int i = 0; tr && i < CountTrackMediaItems(tr); i++)
  {
    MediaItem *it = GetTrackMediaItem(tr, i);
    const double p = GetMediaItemInfo_Value(it, "D_POSITION"), l = GetMediaItemInfo_Value(it, "D_LENGTH");
    MediaItem_Take *tk = GetActiveTake(it);
    if (tk && TakeIsMIDI(tk) == midi && p < pos + len && p + l > pos) return it;
  }
  return NULL;
}

static MediaItem *findMidiItem(MediaItem *audio, bool create)
{
  const double pos = GetMediaItemInfo_Value(audio, "D_POSITION"), len = GetMediaItemInfo_Value(audio, "D_LENGTH");
  const int idx = (int)GetMediaTrackInfo_Value(GetMediaItem_Track(audio), "IP_TRACKNUMBER"); // = index below
  MediaTrack *tr = GetTrack(NULL, idx);
  if (!isNotesTrack(tr))
  {
    if (!create) return NULL;
    InsertTrackAtIndex(idx, true);
    tr = GetTrack(NULL, idx);
    char name[256];
    snprintf(name, sizeof(name), "%s", NOTES_TRACK_NAME);
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, true);
    TrackFX_AddByName(tr, "ReaSynth", false, -1); // to hear the transcription against the audio
  }
  MediaItem *midi = overlappingItem(tr, pos, len, true);
  if (!midi && create)
  {
    const bool qn = false;
    midi = CreateNewMIDIItemInProj(tr, pos, pos + len, &qn);
  }
  return midi;
}

MediaItem *EnsureMidiItem(MediaItem *audio)
{
  Undo_BeginBlock2(NULL);
  MediaItem *midi = findMidiItem(audio, true);
  Undo_EndBlock2(NULL, "Reascribe: create notes item", -1);
  UpdateArrange();
  return midi;
}

// The audio item on the track above a Reascribe MIDI item.
static MediaItem *audioForMidi(MediaItem *midi)
{
  MediaTrack *tr = GetMediaItem_Track(midi);
  if (!isNotesTrack(tr)) return NULL;
  const int above = (int)GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") - 2;
  const double pos = GetMediaItemInfo_Value(midi, "D_POSITION"), len = GetMediaItemInfo_Value(midi, "D_LENGTH");
  return above >= 0 ? overlappingItem(GetTrack(NULL, above), pos, len, false) : NULL;
}

static void openSelected()
{
  MediaItem *item = GetSelectedMediaItem(NULL, 0);
  MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
  MediaItem *audio = take && !TakeIsMIDI(take) ? item : take ? audioForMidi(item) : NULL;
  if (!audio)
  {
    ShowMessageBox("Select an audio item (or a MIDI item on a \"Reascribe notes\" track) first.", "Reascribe", 0);
    return;
  }
  Window_Open(audio, EnsureMidiItem(audio));
}

static int g_cmdOpen;

static bool onAction(KbdSectionInfo *sec, int command, int val, int val2, int relmode, HWND hwnd)
{
  if (command != g_cmdOpen) return false;
  openSelected();
  return true;
}

static int translateAccel(MSG *msg, accelerator_register_t *ctx) { return Window_TranslateAccel(msg); }
static accelerator_register_t g_accel = {translateAccel, true, NULL};

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t *rec)
{
  if (!rec)
  {
    Window_Close();
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;

  custom_action_register_t a = {0, "REASCRIBE_OPEN", "Reascribe: Open spectrogram editor for selected item", NULL};
  if (!(g_cmdOpen = rec->Register("custom_action", &a))) return 0;
  rec->Register("hookcommand2", (void *)onAction);
  rec->Register("accelerator", &g_accel);
  rec->Register("timer", (void *)Window_Timer);
  return 1;
}
