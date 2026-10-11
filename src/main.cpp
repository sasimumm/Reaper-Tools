// Reascribe: shows a spectrogram of an audio item inside REAPER's MIDI editor, so notes can be
// drawn with the regular piano roll on top of it.
//
// "Reascribe: Transcribe selected audio item" creates a "Reascribe notes" track below the audio
// track with an empty MIDI item over the audio, and opens it in the MIDI editor. Whenever the
// active MIDI editor shows a take on a "Reascribe notes" track, the audio item on the track above
// is drawn translucently over the piano roll.
//
// The piano roll's scroll and zoom are only available from the CFGEDITVIEW line of the item
// state chunk, and its window id and ruler height are undocumented (taken from juliansader's
// js_Mouse editing scripts), so a REAPER update may need RULER_H adjusted.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_CreateTakeAudioAccessor
#define REAPERAPI_WANT_DestroyAudioAccessor
#define REAPERAPI_WANT_GetAudioAccessorSamples
#define REAPERAPI_WANT_MIDI_GetProjTimeFromPPQPos
#define REAPERAPI_WANT_CreateNewMIDIItemInProj
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_GetMediaItem_Track
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_ValidatePtr2
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceNumChannels
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_TakeIsMIDI
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_ShowMessageBox
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_GetItemStateChunk
#define REAPERAPI_WANT_MIDIEditor_GetActive
#define REAPERAPI_WANT_MIDIEditor_GetTake
#define REAPERAPI_WANT_GetMediaItemTake_Item
#define REAPERAPI_WANT_GetMediaItemTake_Track
#define REAPERAPI_WANT_SelectAllMediaItems
#define REAPERAPI_WANT_SetMediaItemSelected
#define REAPERAPI_WANT_Main_OnCommand
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include "nsgt.h"

// Tuning knobs.
static const int RULER_H = 62;              // midiview ruler/marker area height (juliansader's value)
static const double OVERLAY_OPACITY = 0.8;  // alpha of the loudest cells; silence is transparent
static const int MIDIVIEW_ID = 1001;        // piano roll child window of the MIDI editor
static const char *NOTES_TRACK_NAME = "Reascribe notes";

// --- spectrogram of the paired audio item --------------------------------------

static MediaItem *g_audioItem;
static double g_audioPos;
static Spectrogram g_spec;
static RenderParams g_rp;
static std::vector<unsigned char> g_val; // cellValue() quantized to 0..255, frames*bins
static unsigned int g_lut[256];
static int g_valVersion;                 // bumped whenever g_val changes, invalidates the overlay

static void updateValues()
{
  g_val.resize(g_spec.mag.size());
  for (int f = 0; f < g_spec.frames; f++)
    for (int b = 0; b < g_spec.bins; b++)
      g_val[(size_t)f * g_spec.bins + b] = (unsigned char)lround(cellValue(g_spec, g_rp, f, b) * 255);
  makeColormap(g_rp.bias, g_lut);
  g_valVersion++;
}

static void analyze(MediaItem *item)
{
  MediaItem_Take *take = GetActiveTake(item);
  AudioAccessor *acc = CreateTakeAudioAccessor(take);
  const int nch = (std::max)(1, (std::min)(2, GetMediaSourceNumChannels(GetMediaItemTake_Source(take))));
  const NSGTParams p;
  const double len = GetMediaItemInfo_Value(item, "D_LENGTH");
  std::vector<double> buf;
  g_spec = computeNSGT(p, (long long)(len * p.sampleRate), [&](long long start, int n, float *out) {
    buf.assign((size_t)n * nch, 0); // the accessor leaves the buffer untouched where there's no audio
    // accessor time is relative to the item start
    GetAudioAccessorSamples(acc, (int)p.sampleRate, nch, start / p.sampleRate, n, buf.data());
    for (int i = 0; i < n; i++)
    {
      double s = 0;
      for (int c = 0; c < nch; c++) s += buf[(size_t)i * nch + c];
      out[i] = (float)(s / nch);
    }
  });
  DestroyAudioAccessor(acc);
  g_audioItem = item;
  g_audioPos = GetMediaItemInfo_Value(item, "D_POSITION");
  autoCalibrate(g_spec, g_rp);
  updateValues();
}

// --- project structure: audio track, notes track below it ----------------------

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
    double p = GetMediaItemInfo_Value(it, "D_POSITION"), l = GetMediaItemInfo_Value(it, "D_LENGTH");
    MediaItem_Take *tk = GetActiveTake(it);
    if (tk && TakeIsMIDI(tk) == midi && p < pos + len && p + l > pos) return it;
  }
  return NULL;
}

// The audio item a Reascribe MIDI take transcribes, or NULL if the take isn't one.
static MediaItem *audioItemFor(MediaItem_Take *midiTake)
{
  MediaTrack *tr = GetMediaItemTake_Track(midiTake);
  if (!isNotesTrack(tr)) return NULL;
  int idx = (int)GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") - 2; // 1-based, track above
  MediaItem *mi = GetMediaItemTake_Item(midiTake);
  double pos = GetMediaItemInfo_Value(mi, "D_POSITION"), len = GetMediaItemInfo_Value(mi, "D_LENGTH");
  return idx >= 0 ? overlappingItem(GetTrack(NULL, idx), pos, len, false) : NULL;
}

static void transcribeSelected()
{
  MediaItem *item = GetSelectedMediaItem(NULL, 0);
  MediaItem_Take *take = item ? GetActiveTake(item) : NULL;
  if (!take || TakeIsMIDI(take))
  {
    ShowMessageBox("Select an audio item first.", "Reascribe", 0);
    return;
  }
  const double pos = GetMediaItemInfo_Value(item, "D_POSITION"), len = GetMediaItemInfo_Value(item, "D_LENGTH");
  Undo_BeginBlock2(NULL);
  const int idx = (int)GetMediaTrackInfo_Value(GetMediaItem_Track(item), "IP_TRACKNUMBER"); // = index below
  MediaTrack *tr = GetTrack(NULL, idx);
  if (!isNotesTrack(tr))
  {
    InsertTrackAtIndex(idx, true);
    tr = GetTrack(NULL, idx);
    char name[256];
    snprintf(name, sizeof(name), "%s", NOTES_TRACK_NAME);
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, true);
    TrackFX_AddByName(tr, "ReaSynth", false, -1); // to hear the transcription against the audio
  }
  MediaItem *midi = overlappingItem(tr, pos, len, true);
  if (!midi)
  {
    bool qn = false;
    midi = CreateNewMIDIItemInProj(tr, pos, pos + len, &qn);
  }
  Undo_EndBlock2(NULL, "Reascribe: set up transcription", -1);

  analyze(item); // re-analyze even if paired before, in case the audio changed
  SelectAllMediaItems(NULL, false);
  SetMediaItemSelected(midi, true);
  UpdateArrange();
  Main_OnCommand(40153, 0); // Item: Open in built-in MIDI editor
}

// --- piano roll view -----------------------------------------------------------

struct View
{
  double leftTick, hzoom, topPitch, rowH;
  int beats, width, height, notesBottom, valVersion;
  bool operator==(const View &o) const
  {
    return leftTick == o.leftTick && hzoom == o.hzoom && topPitch == o.topPitch && rowH == o.rowH &&
           beats == o.beats && width == o.width && height == o.height && notesBottom == o.notesBottom &&
           valVersion == o.valVersion;
  }
};

static bool readView(MediaItem_Take *take, HWND hwnd, View &v)
{
  static std::vector<char> chunk(4 << 20); // ceiling: ~100k notes; grow if transcriptions get huge
  if (!GetItemStateChunk(GetMediaItemTake_Item(take), chunk.data(), (int)chunk.size(), false)) return false;
  // first take only: Reascribe items have one
  const char *p = strstr(chunk.data(), "\nCFGEDITVIEW ");
  double top;
  if (!p || sscanf(p, "\nCFGEDITVIEW %lf %lf %lf %lf", &v.leftTick, &v.hzoom, &top, &v.rowH) != 4) return false;
  v.topPitch = 127 - top;

  v.beats = 1;
  if ((p = strstr(chunk.data(), "\nCFGEDIT ")))
  {
    p += 9;
    for (int i = 1; i < 19 && p; i++) p = strchr(p + 1, ' ');
    if (p) v.beats = atoi(p) == 0 || atoi(p) == 4; // 19th field: timebase, 0/4 = beats
  }

  RECT r;
  GetClientRect(hwnd, &r);
  v.width = r.right, v.height = r.bottom, v.notesBottom = r.bottom;
  for (p = chunk.data(); (p = strstr(p, "\nVELLANE ")); p++)
  {
    int type, h;
    if (sscanf(p, "\nVELLANE %d %d", &type, &h) == 2) v.notesBottom -= h;
  }
  v.valVersion = g_valVersion;
  return v.hzoom > 0 && v.rowH > 0 && v.width > 0;
}

// --- overlay -------------------------------------------------------------------

static bool g_enabled = true;
static HWND g_target;                    // hooked midiview that should get the overlay
static MediaItem_Take *g_targetTake;
static std::map<HWND, WNDPROC> g_origProcs;

static HDC g_memdc;
static int g_memw, g_memh;
static View g_drawn;
static bool g_drawnValid;

static void renderOverlay(MediaItem_Take *take, const View &v)
{
  const int w = (v.width + 3) & ~3; // SWELL backing stores are a multiple of 4 wide
  if (!g_memdc || w != g_memw || v.height != g_memh)
  {
    if (g_memdc) SWELL_DeleteGfxContext(g_memdc);
    g_memdc = SWELL_CreateMemContext(NULL, w, v.height);
    g_memw = w, g_memh = v.height;
  }
  unsigned int *fb = g_memdc ? (unsigned int *)SWELL_GetCtxFrameBuffer(g_memdc) : NULL;
  if (!fb) return;
  memset(fb, 0, (size_t)w * v.height * 4);
  const Spectrogram &s = g_spec;
  if (!s.frames) return;

  // pixel row y -> range of bins [rowLo, rowHi]
  const int y0 = RULER_H, y1 = (std::max)(y0, v.notesBottom);
  std::vector<int> rowLo(y1 - y0), rowHi(y1 - y0);
  for (int y = y0; y < y1; y++)
  {
    // pitch at a pixel's top edge; a note row is centred on its pitch
    double hi = s.binOfPitch(v.topPitch + 0.5 - (y - RULER_H) / v.rowH);
    double lo = s.binOfPitch(v.topPitch + 0.5 - (y + 1 - RULER_H) / v.rowH);
    int a = (int)ceil(lo), b = (int)ceil(hi) - 1;
    if (a > b) a = b = (int)lround(0.5 * (lo + hi));
    rowLo[y - y0] = a, rowHi[y - y0] = b;
  }

  const double leftTime = MIDI_GetProjTimeFromPPQPos(take, v.leftTick);
  auto timeAt = [&](double x) {
    return v.beats ? MIDI_GetProjTimeFromPPQPos(take, v.leftTick + x / v.hzoom) : leftTime + x / v.hzoom;
  };
  std::vector<unsigned char> col(s.bins);
  double ta = timeAt(0);
  for (int x = 0; x < v.width; x++)
  {
    const double tb = timeAt(x + 1);
    const double fa = (ta - g_audioPos) * s.frameRate(), fb2 = (tb - g_audioPos) * s.frameRate();
    ta = tb;
    int f0 = (int)ceil(fa), f1 = (int)ceil(fb2) - 1;
    if (f0 > f1) f0 = f1 = (int)lround(0.5 * (fa + fb2));
    f0 = (std::max)(f0, 0), f1 = (std::min)(f1, s.frames - 1);
    if (f0 > f1) continue;

    // max over the column's frames, then over each row's bins
    memcpy(col.data(), &g_val[(size_t)f0 * s.bins], s.bins);
    for (int f = f0 + 1; f <= f1; f++)
    {
      const unsigned char *v2 = &g_val[(size_t)f * s.bins];
      for (int b = 0; b < s.bins; b++) col[b] = (std::max)(col[b], v2[b]);
    }
    for (int y = y0; y < y1; y++)
    {
      const int a = (std::max)(0, rowLo[y - y0]), b = (std::min)(s.bins - 1, rowHi[y - y0]);
      if (a > b) continue;
      unsigned char m = 0;
      for (int k = a; k <= b; k++) m = (std::max)(m, col[k]);
      fb[(size_t)y * w + x] = ((unsigned int)(m * OVERLAY_OPACITY) << 24) | g_lut[m];
    }
  }
}

static void paintOverlay(HWND hwnd)
{
  View v;
  if (!g_targetTake || !readView(g_targetTake, hwnd, v)) return;
  if (!g_drawnValid || !(v == g_drawn))
  {
    renderOverlay(g_targetTake, v);
    g_drawn = v, g_drawnValid = true;
  }
  if (!g_memdc) return;

  PAINTSTRUCT ps; // under SWELL this only reports the update rect, it doesn't start a new paint
  BeginPaint(hwnd, &ps);
  RECT r = ps.rcPaint;
  r.top = (std::max)((int)r.top, RULER_H), r.bottom = (std::min)((int)r.bottom, v.notesBottom);
  r.right = (std::min)((int)r.right, v.width);
  if (r.right <= r.left || r.bottom <= r.top) return;
  HDC dc = GetDC(hwnd);
  BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, g_memdc, r.left, r.top, (int)SRCCOPY_USEALPHACHAN);
  ReleaseDC(hwnd, dc);
}

static LRESULT WINAPI midiviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  WNDPROC orig = g_origProcs[hwnd];
  if (msg == WM_DESTROY)
  {
    SetWindowLong(hwnd, GWL_WNDPROC, (LONG_PTR)orig);
    g_origProcs.erase(hwnd);
    if (hwnd == g_target) g_target = NULL, g_targetTake = NULL;
    return CallWindowProc(orig, hwnd, msg, wParam, lParam);
  }
  LRESULT res = CallWindowProc(orig, hwnd, msg, wParam, lParam);
  if (msg == WM_PAINT && hwnd == g_target && g_enabled) paintOverlay(hwnd); // after REAPER has drawn
  return res;
}

static void unhookAll()
{
  for (auto &it : g_origProcs)
  {
    // only restore if nobody subclassed after us; otherwise our proc stays as a passthrough
    if ((WNDPROC)GetWindowLong(it.first, GWL_WNDPROC) == midiviewProc)
      SetWindowLong(it.first, GWL_WNDPROC, (LONG_PTR)it.second);
  }
  g_origProcs.clear();
  g_target = NULL, g_targetTake = NULL;
}

static void onTimer()
{
  HWND editor = MIDIEditor_GetActive();
  MediaItem_Take *take = editor ? MIDIEditor_GetTake(editor) : NULL;
  if (take && !ValidatePtr2(NULL, take, "MediaItem_Take*")) take = NULL;
  MediaItem *audio = take ? audioItemFor(take) : NULL;
  HWND view = audio ? GetDlgItem(editor, MIDIVIEW_ID) : NULL;

  if (audio && audio != g_audioItem) analyze(audio);
  if (g_audioItem && !ValidatePtr2(NULL, g_audioItem, "MediaItem*")) g_audioItem = NULL, g_spec = Spectrogram();
  if (view && !g_origProcs.count(view))
    g_origProcs[view] = (WNDPROC)SetWindowLong(view, GWL_WNDPROC, (LONG_PTR)midiviewProc);
  if (view != g_target || take != g_targetTake)
  {
    if (g_target) InvalidateRect(g_target, NULL, FALSE);
    g_target = view, g_targetTake = view ? take : NULL, g_drawnValid = false;
    if (g_target) InvalidateRect(g_target, NULL, FALSE);
  }
}

// --- actions -------------------------------------------------------------------

enum { ACT_TRANSCRIBE, ACT_TOGGLE_OVERLAY, ACT_TOGGLE_PEAKS, ACT_TOGGLE_DENOISE, ACT_COUNT };
static int g_cmds[ACT_COUNT];

static void refresh()
{
  if (g_target) InvalidateRect(g_target, NULL, FALSE);
}

static bool onAction(KbdSectionInfo *sec, int command, int val, int val2, int relmode, HWND hwnd)
{
  if (command == g_cmds[ACT_TRANSCRIBE]) transcribeSelected();
  else if (command == g_cmds[ACT_TOGGLE_OVERLAY]) g_enabled = !g_enabled, refresh();
  else if (command == g_cmds[ACT_TOGGLE_PEAKS]) g_rp.peaksOnly = !g_rp.peaksOnly, updateValues(), refresh();
  else if (command == g_cmds[ACT_TOGGLE_DENOISE]) g_rp.subtractNoise = !g_rp.subtractNoise, updateValues(), refresh();
  else return false;
  return true;
}

static int toggleState(int command)
{
  if (command == g_cmds[ACT_TOGGLE_OVERLAY]) return g_enabled;
  if (command == g_cmds[ACT_TOGGLE_PEAKS]) return g_rp.peaksOnly;
  if (command == g_cmds[ACT_TOGGLE_DENOISE]) return g_rp.subtractNoise;
  return -1;
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t *rec)
{
  if (!rec)
  {
    unhookAll();
    if (g_memdc) SWELL_DeleteGfxContext(g_memdc);
    g_memdc = NULL;
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;

  static const char *ids[ACT_COUNT][2] = {
      {"REASCRIBE_TRANSCRIBE", "Reascribe: Transcribe selected audio item (spectrogram in MIDI editor)"},
      {"REASCRIBE_TOGGLE_OVERLAY", "Reascribe: Toggle spectrogram overlay"},
      {"REASCRIBE_TOGGLE_PEAKS", "Reascribe: Toggle spectrogram peaks only"},
      {"REASCRIBE_TOGGLE_DENOISE", "Reascribe: Toggle spectrogram noise subtraction"},
  };
  for (int i = 0; i < ACT_COUNT; i++)
  {
    custom_action_register_t a = {0, ids[i][0], ids[i][1], NULL};
    if (!(g_cmds[i] = rec->Register("custom_action", &a))) return 0;
  }
  rec->Register("hookcommand2", (void *)onAction);
  rec->Register("toggleaction", (void *)toggleState);
  rec->Register("timer", (void *)onTimer);
  return 1;
}
