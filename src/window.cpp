#include "window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "reaper_plugin_functions.h"
#undef min
#undef max

#include "contour.h"
#include "editor.h"
#include "nsgt.h"

// Layout, in pixels.
static const int KB_W = 56, RULER_H = 30, STATUS_H = 20, PANEL_W = 250;
// Zoom per pinch event: SWELL clamps each event's factor to 0.5..2, so only its direction is used.
static const double PINCH_STEP = 1.03;
static const double WHEEL_ZOOM = 1.05; // zoom per wheel notch (cmd/alt+scroll)
static const int GUIDE_THICKNESS = 3;  // overtone hover marks, pixels

// Colours, 0xRRGGBB.
static const unsigned int C_BG = 0x1b1b1b, C_RULER = 0x262626, C_TEXT = 0xd0d0d0, C_DIM = 0x8a8a8a;
static const unsigned int C_NOTE = 0x2ec4b6, C_NOTE_EDGE = 0x08302d, C_SEL = 0xeafffd, C_SEL_EDGE = 0x2ec4b6;
static const unsigned int C_EDIT_CURSOR = 0xff5050, C_PLAY_CURSOR = 0xffffff, C_ACCENT = 0x7fd6ff;

// --- state ---------------------------------------------------------------------

static HWND g_hwnd;
static MediaItem *g_audio, *g_midi;
static double g_audioPos, g_audioLen, g_audioOffs, g_audioRate;

static NSGTParams g_np;
static RenderParams g_rp;
static Spectrogram g_spec;
static std::vector<unsigned char> g_val; // renderValues() output, one byte per cell
static unsigned int g_lut[256];
static int g_valVersion;

static ViewMap g_view;
static NoteEditor g_ed;
static std::vector<Note> g_loaded; // notes as read from the take, by take index
static std::string g_hash;
// Snapping: onsets (and note changes in the pitch contour) by default, since recordings are
// usually free tempo and the grid means nothing until a tempo map is fitted.
enum { SNAP_OFF, SNAP_ONSETS, SNAP_GRID };
static int g_snapMode = SNAP_ONSETS;
static const double ONSET_SNAP_RANGE = 0.03; // seconds

static Contour g_contour;
static std::vector<Segment> g_segments;   // stable-pitch stretches, shown as suggested notes
static std::vector<int> g_onsets;         // frames
static std::vector<double> g_snapTimes;   // onsets and segment edges, project time, sorted
static bool g_showContour = true, g_suggest = true, g_pitchFromContour = true;

static const char *const CHANNELS[] = {"Mix", "Left", "Right", "Side", "Centre"}; // centre: mid minus side
enum { CH_MIX, CH_LEFT, CH_RIGHT, CH_SIDE, CH_CENTRE };
static int g_channel = CH_MIX;

static const char *const SPEEDS[] = {"100%", "75%", "50%"};
static const double SPEED_RATES[] = {1, 0.75, 0.5};
static int g_speed;
static double g_quantStrength = 1;
static bool g_panel = true;
static bool g_ghosts = true;   // faint outlines at each note's overtones
static bool g_audition = true; // play a note through the notes track when clicked or drawn
static double g_harmonics = 8, g_guideOpacity = 0.4;
// Explain-away: dim spectrogram energy on drawn notes and their overtones, so what is still
// bright is what still needs transcribing.
static bool g_explain;
static const double EXPLAIN_DIM = 0.25;
static int g_notesVersion; // bumped whenever notes are loaded

static HWND g_edit;            // lyric edit field while typing; identifies its note by start and pitch
static double g_editStart;
static int g_editPitch;

enum Drag { DRAG_NONE, DRAG_NOTES, DRAG_RULER, DRAG_RULER_SELECT, DRAG_PANEL };
static double g_selectAnchor;
static Drag g_drag;
static int g_panelWidget = -1;
static bool g_analysisDirty;
static int g_mouseX = -1, g_mouseY = -1;

static HDC g_memdc;
static int g_memw, g_memh;
static HFONT g_font;
static std::vector<unsigned int> g_layer; // rendered spectrogram for the note area
struct LayerKey
{
  double t0, pps, pitchTop, rowH;
  int w, h, version, explain; // explain: 0 off, else notes version + harmonics
  bool operator==(const LayerKey &o) const
  {
    return t0 == o.t0 && pps == o.pps && pitchTop == o.pitchTop && rowH == o.rowH && w == o.w && h == o.h &&
           version == o.version && explain == o.explain;
  }
};
static LayerKey g_layerKey;
static bool g_layerValid;

// change detection for the timer
static int g_lastPlayState, g_lastProjChange;
static double g_lastPlayPos, g_lastCursor;

struct Rect
{
  int l, t, r, b;
  bool in(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
};

static Rect clientRect()
{
  RECT r = {0, 0, 0, 0};
  if (g_hwnd) GetClientRect(g_hwnd, &r);
  return {0, 0, (int)r.right, (int)r.bottom};
}
// Below this size only the background is drawn, so the areas below always fit in the client rect.
static const int MIN_W = KB_W + 40, MIN_H = RULER_H + STATUS_H + 20;

static bool panelShown() { return g_panel && clientRect().r >= KB_W + PANEL_W + 100; }
static Rect noteArea()
{
  Rect c = clientRect();
  return {KB_W, RULER_H, std::max(KB_W + 1, c.r - (panelShown() ? PANEL_W : 0)), std::max(RULER_H + 1, c.b - STATUS_H)};
}
static Rect rulerArea() { Rect n = noteArea(); return {n.l, 0, n.r, RULER_H}; }
static Rect panelArea() { Rect c = clientRect(), n = noteArea(); return {n.r, 0, c.r, c.b}; }

static void syncViewOrigin()
{
  Rect n = noteArea();
  g_view.x0 = n.l, g_view.y0 = n.t;
}

static void redraw()
{
  if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

// --- spectrogram ---------------------------------------------------------------

static void updateValues()
{
  renderValues(g_spec, g_rp, g_val);
  makeColormap(g_rp.bias, g_lut);
  g_valVersion++;
  redraw();
}

static double frameTime(double f) { return g_audioPos + f / g_spec.frameRate(); }

// What an analysis depends on: if any of it changes, that channel is recomputed.
struct AnalysisKey
{
  NSGTParams p;
  double pos, len, offs, rate;
  bool operator==(const AnalysisKey &o) const
  {
    return p.sampleRate == o.p.sampleRate && p.fmin == o.p.fmin && p.fmax == o.p.fmax && p.binsPerOctave == o.p.binsPerOctave &&
           p.hop == o.p.hop && p.gamma == o.p.gamma && pos == o.pos && len == o.len && offs == o.offs && rate == o.rate;
  }
};

struct Vocal
{
  Contour contour;
  std::vector<Segment> segments;
  std::vector<int> onsets;
};

static Vocal analyzeVocal(const Spectrogram &s)
{
  Vocal v;
  v.contour = trackPitch(s);
  v.onsets = detectOnsets(s);
  v.segments = segmentContour(v.contour, s.frameRate(), &v.onsets);
  return v;
}

// One entry per channel. Memory ceiling: ~30 MB per channel per 3 minutes of audio.
struct ChannelCache
{
  bool valid = false;
  AnalysisKey key;
  Spectrogram spec;
  bool vocalValid = false;
  Vocal vocal;
};
static ChannelCache g_cache[5];

static void clearCache()
{
  for (ChannelCache &c : g_cache) c = ChannelCache();
}

static bool cached(int ch, const AnalysisKey &key) { return g_cache[ch].valid && g_cache[ch].key == key; }

// The audio, read on the main thread (accessors must be), resampled to the analysis rate.
static void readAudio(MediaItem_Take *take, double len, double sr, std::vector<float> &left, std::vector<float> &right)
{
  AudioAccessor *acc = CreateTakeAudioAccessor(take);
  const int nch = std::max(1, std::min(2, GetMediaSourceNumChannels(GetMediaItemTake_Source(take))));
  const long long n = (long long)(len * sr);
  left.assign((size_t)std::max(0LL, n), 0), right.assign(left.size(), 0);
  std::vector<double> buf;
  const int chunk = 65536;
  for (long long pos = 0; pos < n; pos += chunk)
  {
    const int m = (int)std::min<long long>(chunk, n - pos);
    buf.assign((size_t)m * nch, 0); // the accessor leaves the buffer untouched where there's no audio
    GetAudioAccessorSamples(acc, (int)sr, nch, pos / sr, m, buf.data()); // time relative to item start
    for (int i = 0; i < m; i++)
      left[pos + i] = (float)buf[(size_t)i * nch], right[pos + i] = (float)buf[(size_t)i * nch + (nch - 1)];
  }
  DestroyAudioAccessor(acc);
}

// A background analysis of one channel. The worker only touches the job; the main thread polls
// `done` from the timer and installs the results.
struct Job
{
  std::thread thread;
  std::atomic<bool> cancel{false}, done{false};
  std::atomic<int> permille{0};
  AnalysisKey key;
  int channel;
  std::vector<float> left, right;
  bool need[4] = {};   // raw channels (CH_MIX..CH_SIDE) to compute
  Spectrogram raw[4];  // computed, or copied from the cache when only needed for the centre
  Spectrogram spec;    // the requested channel
  Vocal vocal;
  bool vocalNeeded = true;

  void run()
  {
    int stages = 1, stage = 0;
    for (bool b : need) stages += b;
    for (int ch = 0; ch < 4; ch++)
    {
      if (!need[ch]) continue;
      const std::vector<float> &l = left, &r = right;
      raw[ch] = computeNSGT(key.p, (long long)l.size(), [&](long long start, int n, float *out) {
        for (int i = 0; i < n; i++)
        {
          const long long j = start + i;
          const float a = j >= 0 && j < (long long)l.size() ? l[j] : 0, b = j >= 0 && j < (long long)r.size() ? r[j] : 0;
          out[i] = ch == CH_LEFT ? a : ch == CH_RIGHT ? b : ch == CH_SIDE ? 0.5f * (a - b) : 0.5f * (a + b);
        }
      }, [&](double f) {
        permille = (int)(1000 * (stage + f) / stages);
        return !cancel;
      });
      stage++;
      if (cancel) return;
    }
    if (channel == CH_CENTRE)
    {
      // centre-panned content (usually the lead vocal): mid magnitudes minus side magnitudes
      spec = raw[CH_MIX];
      if (raw[CH_SIDE].mag.size() == spec.mag.size())
        for (size_t i = 0; i < spec.mag.size(); i++) spec.mag[i] = std::max(0.0f, spec.mag[i] - raw[CH_SIDE].mag[i]);
    }
    else spec = raw[channel];
    if (vocalNeeded) vocal = analyzeVocal(spec);
    permille = 1000;
    done = true;
  }
};
static std::unique_ptr<Job> g_job;
static bool g_autoOnInstall; // auto-calibrate the display once the analysis arrives

static void cancelJob()
{
  if (!g_job) return;
  g_job->cancel = true;
  if (g_job->thread.joinable()) g_job->thread.join();
  g_job.reset();
}

static void install(const ChannelCache &c)
{
  g_spec = c.spec;
  g_contour = c.vocal.contour, g_segments = c.vocal.segments, g_onsets = c.vocal.onsets;
  g_snapTimes.clear();
  for (int f : g_onsets) g_snapTimes.push_back(frameTime(f));
  for (const Segment &sg : g_segments) g_snapTimes.push_back(frameTime(sg.f0)), g_snapTimes.push_back(frameTime(sg.f1 + 1));
  std::sort(g_snapTimes.begin(), g_snapTimes.end());
  estimateNoise(g_spec, g_rp.noiseFraction);
  if (g_autoOnInstall) autoCalibrate(g_spec, g_rp), g_autoOnInstall = false;
  g_analysisDirty = false;
  updateValues();
}

// Starts (or reuses from the cache) the analysis for the current channel and settings.
static void requestAnalysis(bool autoCalibrateAfter)
{
  MediaItem_Take *take = GetActiveTake(g_audio);
  if (!take) return;
  g_autoOnInstall |= autoCalibrateAfter;
  cancelJob();
  const AnalysisKey key = {g_np, g_audioPos, g_audioLen, g_audioOffs, g_audioRate};
  const int ch = g_channel;
  if (cached(ch, key) && g_cache[ch].vocalValid) { install(g_cache[ch]); return; }

  std::unique_ptr<Job> job(new Job);
  job->key = key, job->channel = ch;
  bool needAudio = false;
  const int rawCh[2] = {ch == CH_CENTRE ? CH_MIX : ch, ch == CH_CENTRE ? CH_SIDE : -1};
  for (int r : rawCh)
  {
    if (r < 0) continue;
    if (cached(r, key)) job->raw[r] = g_cache[r].spec;
    else job->need[r] = needAudio = true;
  }
  if (ch == CH_CENTRE && cached(CH_CENTRE, key)) // only the vocal analysis is missing
  {
    job->need[CH_MIX] = job->need[CH_SIDE] = needAudio = false;
    job->raw[CH_MIX] = g_cache[CH_CENTRE].spec, job->raw[CH_SIDE] = Spectrogram();
  }
  if (needAudio) readAudio(take, g_audioLen, g_np.sampleRate, job->left, job->right);
  Job *j = job.get();
  job->thread = std::thread([j] { j->run(); });
  g_job = std::move(job);
  g_analysisDirty = false;
  redraw();
}

// Called from the timer: installs a finished analysis into the cache and the view.
static void pollJob()
{
  if (!g_job) return;
  if (!g_job->done) { redraw(); return; } // keep the progress display moving
  g_job->thread.join();
  Job &j = *g_job;
  for (int r = 0; r < 4; r++)
    if (j.need[r] && r != j.channel)
    {
      ChannelCache &c = g_cache[r];
      c = ChannelCache();
      c.valid = true, c.key = j.key, c.spec = std::move(j.raw[r]);
    }
  ChannelCache &c = g_cache[j.channel];
  c.valid = true, c.key = j.key, c.spec = std::move(j.spec), c.vocal = std::move(j.vocal), c.vocalValid = true;
  g_job.reset();
  install(c);
}

static void renderLayer(const Rect &n)
{
  const int w = n.r - n.l, h = n.b - n.t;
  g_layer.assign((size_t)w * h, 0xff000000 | C_BG);
  const Spectrogram &s = g_spec;
  if (!s.frames || g_val.size() != s.mag.size()) return;

  std::vector<int> rowLo(h), rowHi(h); // bins covered by each pixel row
  for (int y = 0; y < h; y++)
  {
    const double hi = s.binOfPitch(g_view.p(n.t + y)), lo = s.binOfPitch(g_view.p(n.t + y + 1));
    int a = (int)ceil(lo), b = (int)ceil(hi) - 1;
    if (a > b) a = b = (int)lround(0.5 * (lo + hi));
    rowLo[y] = std::max(0, a), rowHi[y] = std::min(s.bins - 1, b);
  }
  std::vector<double> rowPitch(h);
  for (int y = 0; y < h; y++) rowPitch[y] = g_view.p(n.t + y + 0.5);
  std::vector<double> explained; // pitches of notes and overtones sounding in this column
  const int nh = (int)lround(g_harmonics);
  std::vector<unsigned char> col(s.bins);
  for (int x = 0; x < w; x++)
  {
    explained.clear();
    if (g_explain)
    {
      const double ta = g_view.t(n.l + x), tb = g_view.t(n.l + x + 1);
      for (const Note &nt : g_ed.notes)
        if (nt.start < tb && nt.end > ta)
          for (int hh = 1; hh <= nh; hh++) explained.push_back(nt.pitch + 12 * log2((double)hh));
    }
    const double fa = (g_view.t(n.l + x) - g_audioPos) * s.frameRate();
    const double fb = (g_view.t(n.l + x + 1) - g_audioPos) * s.frameRate();
    int f0 = (int)ceil(fa), f1 = (int)ceil(fb) - 1;
    if (f0 > f1) f0 = f1 = (int)lround(0.5 * (fa + fb));
    f0 = std::max(f0, 0), f1 = std::min(f1, s.frames - 1);
    if (f0 > f1) continue;
    memcpy(col.data(), &g_val[(size_t)f0 * s.bins], s.bins);
    for (int f = f0 + 1; f <= f1; f++)
    {
      const unsigned char *v = &g_val[(size_t)f * s.bins];
      for (int b = 0; b < s.bins; b++) col[b] = std::max(col[b], v[b]);
    }
    for (int y = 0; y < h; y++)
    {
      if (rowLo[y] > rowHi[y]) continue;
      unsigned char m = 0;
      for (int k = rowLo[y]; k <= rowHi[y]; k++) m = std::max(m, col[k]);
      for (double ep : explained)
        if (fabs(rowPitch[y] - ep) < 0.5) { m = (unsigned char)(m * EXPLAIN_DIM); break; }
      g_layer[(size_t)y * w + x] = 0xff000000 | g_lut[m];
    }
  }
}

// --- notes <-> MIDI take ---------------------------------------------------------

static MediaItem_Take *midiTake()
{
  if (g_midi && !ValidatePtr2(NULL, g_midi, "MediaItem*")) g_midi = NULL;
  return g_midi ? GetActiveTake(g_midi) : NULL;
}

static std::string takeHash(MediaItem_Take *take)
{
  char h[128] = "";
  if (take) MIDI_GetHash(take, false, h, sizeof(h)); // includes text events, so lyric edits count
  return h;
}

static const int LYRIC_EVENT = 5; // MIDI meta event type used by REAPER's notation lyrics

// Lyrics are MIDI lyric events at a note's start; each attaches to the first note starting there.
static void loadNotes()
{
  g_ed.cancel();
  g_loaded.clear();
  MediaItem_Take *take = midiTake();
  int cnt = 0, textCnt = 0;
  if (take) MIDI_CountEvts(take, &cnt, NULL, &textCnt);
  std::vector<double> startPPQ;
  for (int i = 0; i < cnt; i++)
  {
    bool sel = false;
    double s = 0, e = 0;
    int pitch = 0;
    MIDI_GetNote(take, i, &sel, NULL, &s, &e, NULL, &pitch, NULL);
    g_loaded.push_back({MIDI_GetProjTimeFromPPQPos(take, s), MIDI_GetProjTimeFromPPQPos(take, e), pitch, sel, i});
    startPPQ.push_back(s);
  }
  for (int i = 0; i < textCnt; i++)
  {
    char msg[1024];
    int sz = sizeof(msg) - 1, type = 0;
    double ppq = 0;
    if (!MIDI_GetTextSysexEvt(take, i, NULL, NULL, &ppq, &type, msg, &sz) || type != LYRIC_EVENT) continue;
    msg[std::max(0, std::min(sz, (int)sizeof(msg) - 1))] = 0;
    for (size_t n = 0; n < g_loaded.size(); n++)
      if (g_loaded[n].lyricIdx < 0 && fabs(startPPQ[n] - ppq) < 0.5)
      {
        g_loaded[n].lyric = msg, g_loaded[n].lyricIdx = i;
        break;
      }
  }
  g_ed.notes = g_loaded;
  g_ed.deleted.clear();
  g_hash = takeHash(take);
  g_notesVersion++;
  redraw();
}

// Writes the editor's notes back to the take as one undo point (none for selection-only changes).
static void commitNotes()
{
  bool dataChanged = !g_ed.deleted.empty(), lyricsChanged = false;
  for (int i : g_ed.deleted) lyricsChanged |= g_loaded[i].lyricIdx >= 0;
  for (const Note &n : g_ed.notes)
  {
    if (n.idx < 0 || n.idx >= (int)g_loaded.size())
    {
      dataChanged = true, lyricsChanged |= !n.lyric.empty();
      continue;
    }
    const Note &o = g_loaded[n.idx];
    dataChanged |= n.start != o.start || n.end != o.end || n.pitch != o.pitch || n.lyric != o.lyric;
    lyricsChanged |= n.lyric != o.lyric || (n.start != o.start && !n.lyric.empty());
  }
  if (dataChanged) Undo_BeginBlock2(NULL);
  MediaItem_Take *take = midiTake();
  if (!take && dataChanged && (g_midi = EnsureMidiItem(g_audio))) take = GetActiveTake(g_midi);
  if (take)
  {
    const bool noSort = true;
    for (const Note &n : g_ed.notes)
    {
      if (n.idx < 0 || n.idx >= (int)g_loaded.size()) continue;
      const Note &o = g_loaded[n.idx];
      if (n.start == o.start && n.end == o.end && n.pitch == o.pitch && n.sel == o.sel) continue;
      const double s = MIDI_GetPPQPosFromProjTime(take, n.start), e = MIDI_GetPPQPosFromProjTime(take, n.end);
      MIDI_SetNote(take, n.idx, &n.sel, NULL, &s, &e, NULL, &n.pitch, NULL, &noSort);
    }
    std::vector<int> del = g_ed.deleted;
    std::sort(del.rbegin(), del.rend()); // highest first so earlier indices stay valid
    for (int i : del) MIDI_DeleteNote(take, i);
    for (const Note &n : g_ed.notes)
      if (n.idx < 0)
        MIDI_InsertNote(take, n.sel, false, MIDI_GetPPQPosFromProjTime(take, n.start),
                        MIDI_GetPPQPosFromProjTime(take, n.end), 0, n.pitch, g_ed.velocity, &noSort);
    if (lyricsChanged)
    {
      // rewrite the lyric events attached to notes; unattached text events are left alone
      std::vector<int> old;
      for (const Note &o : g_loaded)
        if (o.lyricIdx >= 0) old.push_back(o.lyricIdx);
      std::sort(old.rbegin(), old.rend());
      for (int i : old) MIDI_DeleteTextSysexEvt(take, i);
      for (const Note &n : g_ed.notes)
        if (!n.lyric.empty())
          MIDI_InsertTextSysexEvt(take, false, false, MIDI_GetPPQPosFromProjTime(take, n.start), LYRIC_EVENT,
                                  n.lyric.c_str(), (int)n.lyric.size());
    }
    MIDI_Sort(take);
  }
  if (dataChanged)
  {
    Undo_EndBlock2(NULL, "Reascribe: edit notes", UNDO_STATE_ITEMS);
    UpdateArrange();
  }
  loadNotes();
}

// --- grid ------------------------------------------------------------------------

// Finest straight division (in quarter notes) at least 10 px apart, or 0 for "measures".
static double gridStepQN()
{
  const double tMid = g_view.t(g_view.x0 + 200), qn = TimeMap2_timeToQN(NULL, tMid);
  for (double step : {0.125, 0.25, 0.5, 1.0})
    if ((TimeMap2_QNToTime(NULL, qn + step) - TimeMap2_QNToTime(NULL, qn)) * g_view.pps >= 10) return step;
  return 0;
}

static double snapToGrid(double t)
{
  const double qn = TimeMap2_timeToQN(NULL, t), step = gridStepQN();
  if (step > 0) return TimeMap2_QNToTime(NULL, floor(qn / step + 0.5) * step);
  double ms = 0, me = 0;
  TimeMap_QNToMeasures(NULL, qn, &ms, &me);
  return TimeMap2_QNToTime(NULL, qn - ms < me - qn ? ms : me);
}

struct GridLine { int x, level; int measure; }; // level: 0 = measure, 1 = beat, 2 = subdivision

static std::vector<GridLine> gridLines(const Rect &n)
{
  std::vector<GridLine> out;
  const double qa = TimeMap2_timeToQN(NULL, g_view.t(n.l)), qb = TimeMap2_timeToQN(NULL, g_view.t(n.r));
  const double step = gridStepQN();
  if (step > 0)
  {
    for (double q = ceil(qa / step) * step; q <= qb && out.size() < 4000; q += step)
    {
      double ms = 0;
      const int m = TimeMap_QNToMeasures(NULL, q + 1e-9, &ms, NULL);
      const int level = fabs(q - ms) < 1e-6 ? 0 : fabs(q - floor(q + 0.5)) < 1e-6 ? 1 : 2;
      out.push_back({(int)lround(g_view.x(TimeMap2_QNToTime(NULL, q))), level, m});
    }
  }
  else
  {
    double ms = 0, me = 0;
    TimeMap_QNToMeasures(NULL, qa, &ms, &me);
    for (double q = ms; q <= qb && out.size() < 4000;)
    {
      double s2 = 0, e2 = 0;
      const int m = TimeMap_QNToMeasures(NULL, q + 1e-9, &s2, &e2);
      out.push_back({(int)lround(g_view.x(TimeMap2_QNToTime(NULL, s2))), 0, m});
      q = std::max(e2, q + 1e-3);
    }
  }
  return out;
}

// --- tempo map and quantise -----------------------------------------------------------

// Runs a tempo map edit as one undo point, keeping the audio item, the notes item and all notes
// and lyrics at their current times: the recording doesn't move, only the grid does.
static void editTempoMap(const char *undoName, void (*edit)(double a, double b), double a, double b)
{
  g_ed.cancel();
  Undo_BeginBlock2(NULL);
  MediaItem_Take *atake = GetActiveTake(g_audio), *take = midiTake();
  const double apos = GetMediaItemInfo_Value(g_audio, "D_POSITION"), alen = GetMediaItemInfo_Value(g_audio, "D_LENGTH");
  const double arate = atake ? GetMediaItemTakeInfo_Value(atake, "D_PLAYRATE") : 1;
  const double mpos = g_midi ? GetMediaItemInfo_Value(g_midi, "D_POSITION") : 0, mlen = g_midi ? GetMediaItemInfo_Value(g_midi, "D_LENGTH") : 0;
  const std::vector<Note> notes = g_loaded;

  edit(a, b);

  SetMediaItemInfo_Value(g_audio, "D_POSITION", apos), SetMediaItemInfo_Value(g_audio, "D_LENGTH", alen);
  if (atake) SetMediaItemTakeInfo_Value(atake, "D_PLAYRATE", arate);
  if (g_midi) SetMediaItemInfo_Value(g_midi, "D_POSITION", mpos), SetMediaItemInfo_Value(g_midi, "D_LENGTH", mlen);
  if (take)
  {
    const bool noSort = true;
    for (const Note &n : notes)
    {
      const double s = MIDI_GetPPQPosFromProjTime(take, n.start), e = MIDI_GetPPQPosFromProjTime(take, n.end);
      MIDI_SetNote(take, n.idx, NULL, NULL, &s, &e, NULL, NULL, NULL, &noSort);
      if (n.lyricIdx >= 0) MIDI_SetTextSysexEvt(take, n.lyricIdx, NULL, NULL, &s, NULL, NULL, 0, &noSort);
    }
    MIDI_Sort(take);
  }
  UpdateTimeline();
  UpdateArrange();
  Undo_EndBlock2(NULL, undoName, -1);
  loadNotes();
}

static void removeTempoMarkersIn(double a, double b)
{
  for (int i = CountTempoTimeSigMarkers(NULL) - 1; i >= 0; i--)
  {
    double t = 0;
    if (GetTempoTimeSigMarker(NULL, i, &t, NULL, NULL, NULL, NULL, NULL, NULL) && t > a - 0.001 && t < b - 0.001)
      DeleteTempoTimeSigMarker(NULL, i);
  }
}

static bool timeSelection(double *a, double *b)
{
  GetSet_LoopTimeRange2(NULL, false, false, a, b, false);
  return *b - *a >= 0.05;
}

// The time selection becomes one bar (a measure starts at its start) or one beat; the selection
// then moves on to the next bar/beat, ready for the next one.
static void selectionIs(bool bar)
{
  double a, b;
  if (!timeSelection(&a, &b))
  {
    ShowMessageBox("Select a bar first: shift+drag in the ruler.", "Reascribe", 0);
    return;
  }
  static bool s_bar;
  s_bar = bar;
  editTempoMap(bar ? "Reascribe: selection is one bar" : "Reascribe: selection is one beat",
               [](double a, double b) {
                 int num = 4, den = 4;
                 double tempo = 120;
                 TimeMap_GetTimeSigAtTime(NULL, a, &num, &den, &tempo);
                 const double qn = (s_bar ? num : 1) * 4.0 / den; // tempo is in quarter notes per minute
                 removeTempoMarkersIn(a, b);
                 SetTempoTimeSigMarker(NULL, -1, a, -1, -1, 60 * qn / (b - a), s_bar ? num : 0, s_bar ? den : 0, false);
               },
               a, b);
  double na = b, nb = b + (b - a);
  GetSet_LoopTimeRange2(NULL, true, false, &na, &nb, false);
}

static void tempoMarkerAtCursor()
{
  editTempoMap("Reascribe: insert tempo marker", [](double t, double) {
    int num = 4, den = 4;
    double tempo = 120;
    TimeMap_GetTimeSigAtTime(NULL, t, &num, &den, &tempo);
    SetTempoTimeSigMarker(NULL, -1, t, -1, -1, tempo, 0, 0, false);
  }, GetCursorPosition(), 0);
}

// Moves selected notes' starts and ends towards the grid by the quantise strength.
static void quantiseSelected()
{
  bool any = false;
  for (Note &n : g_ed.notes)
  {
    if (!n.sel) continue;
    const double s = n.start + (snapToGrid(n.start) - n.start) * g_quantStrength;
    double e = n.end + (snapToGrid(n.end) - n.end) * g_quantStrength;
    if (e < s + g_ed.minLen) e = std::max(n.end - n.start + s, s + g_ed.minLen);
    any |= s != n.start || e != n.end;
    n.start = s, n.end = e;
  }
  if (any) commitNotes();
}

// --- drawing helpers -------------------------------------------------------------

struct Canvas
{
  unsigned int *px;
  int w, h;
  Rect clip;

  void fill(int x0, int y0, int x1, int y1, unsigned int col, int a = 256)
  {
    x0 = std::max(x0, clip.l), y0 = std::max(y0, clip.t), x1 = std::min(x1, clip.r), y1 = std::min(y1, clip.b);
    for (int y = y0; y < y1; y++)
    {
      unsigned int *p = px + (size_t)y * w;
      for (int x = x0; x < x1; x++)
      {
        if (a >= 256) { p[x] = 0xff000000 | col; continue; }
        const unsigned int d = p[x];
        const unsigned int r = (((d >> 16) & 255) * (256 - a) + ((col >> 16) & 255) * a) >> 8;
        const unsigned int g = (((d >> 8) & 255) * (256 - a) + ((col >> 8) & 255) * a) >> 8;
        const unsigned int b = ((d & 255) * (256 - a) + (col & 255) * a) >> 8;
        p[x] = 0xff000000 | (r << 16) | (g << 8) | b;
      }
    }
  }
  void frame(int x0, int y0, int x1, int y1, unsigned int col, int t = 1)
  {
    fill(x0, y0, x1, y0 + t, col), fill(x0, y1 - t, x1, y1, col);
    fill(x0, y0, x0 + t, y1, col), fill(x1 - t, y0, x1, y1, col);
  }
};

static void text(HDC dc, const char *s, int l, int t, int r, int b, unsigned int col, int flags)
{
  SetTextColor(dc, RGB((col >> 16) & 255, (col >> 8) & 255, col & 255));
  RECT rc = {l, t, r, b};
  DrawText(dc, s, -1, &rc, flags | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
}

static void pitchName(int p, char *buf, int sz)
{
  static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  snprintf(buf, sz, "%s%d", names[((p % 12) + 12) % 12], p / 12 - 1);
}
static bool isBlack(int p) { return (0x54A >> (((p % 12) + 12) % 12)) & 1; }

// --- settings panel --------------------------------------------------------------

enum WidgetType { W_HEADER, W_SLIDER, W_TOGGLE, W_BUTTON, W_CHOICE, W_BUTTONS };
enum { B_AUTO_ALL = 1, B_AUTO_VIEW, B_RESET, B_BAR, B_BEAT, B_TEMPO_MARKER, B_QUANTISE, B_SAVE_DEFAULT, B_LOAD_DEFAULT,
       B_PRESET = 100 };
static const char *const PRESETS[] = {"Vocal in mix", "Solo vocal", "Fast runs"};
static const char *const DEFAULTS[] = {"Save as default", "Load default"};
struct Widget
{
  WidgetType type;
  const char *label;
  double *value;   // sliders
  bool *flag;      // toggles
  double lo, hi;
  bool logScale, analysis;
  double display;  // shown value = value * display
  const char *fmt;
  int button;
  int *choice;               // choices: a row of buttons, one per option
  const char *const *options;
  int optionCount;
};

static std::vector<Widget> widgets()
{
  return {
      {W_BUTTONS, "Presets", NULL, NULL, 0, 0, false, false, 0, NULL, B_PRESET, NULL, PRESETS, 3},
      {W_BUTTONS, "Settings for new items", NULL, NULL, 0, 0, false, false, 0, NULL, B_SAVE_DEFAULT, NULL, DEFAULTS, 2},
      {W_HEADER, "Display"},
      {W_SLIDER, "Floor", &g_rp.dbFloor, NULL, -120, -20, false, false, 1, "%.0f dB"},
      {W_SLIDER, "Contrast", &g_rp.contrast, NULL, 0.1, 3, true, false, 1, "%.2f"},
      {W_SLIDER, "Brightness", &g_rp.brightness, NULL, -1, 1, false, false, 1, "%+.2f"},
      {W_SLIDER, "Colour curve", &g_rp.bias, NULL, -2, 2, false, false, 1, "%+.2f"},
      {W_TOGGLE, "Peaks only", NULL, &g_rp.peaksOnly},
      {W_SLIDER, "Peak dimming", &g_rp.peakDim, NULL, 0, 1, false, false, 1, "%.2f"},
      {W_TOGGLE, "Noise subtraction", NULL, &g_rp.subtractNoise},
      {W_SLIDER, "Noise strength", &g_rp.noiseAlpha, NULL, 0, 3, false, false, 1, "%.2f"},
      {W_SLIDER, "Noise from quietest", &g_rp.noiseFraction, NULL, 0.01, 0.5, false, false, 100, "%.0f%%"},
      {W_BUTTON, "Auto: whole item", NULL, NULL, 0, 0, false, false, 0, NULL, B_AUTO_ALL},
      {W_BUTTON, "Auto: visible", NULL, NULL, 0, 0, false, false, 0, NULL, B_AUTO_VIEW},
      {W_HEADER, "Analysis"},
      {W_SLIDER, "Resolution (gamma)", &g_np.gamma, NULL, 3, 40, true, true, 1, "%.1f Hz"},
      {W_SLIDER, "Lowest frequency", &g_np.fmin, NULL, 20, 500, true, true, 1, "%.0f Hz"},
      {W_SLIDER, "Highest frequency", &g_np.fmax, NULL, 1000, 11000, true, true, 1, "%.0f Hz"},
      {W_CHOICE, "Channel", NULL, NULL, 0, 0, false, true, 0, NULL, 0, &g_channel, CHANNELS, 5},
      {W_BUTTON, "Reset to defaults", NULL, NULL, 0, 0, false, false, 0, NULL, B_RESET},
      {W_HEADER, "Guides"},
      {W_TOGGLE, "Overtone ghosts (H)", NULL, &g_ghosts},
      {W_TOGGLE, "Explain away drawn notes (X)", NULL, &g_explain},
      {W_SLIDER, "Overtones", &g_harmonics, NULL, 2, 16, false, false, 1, "%.0f"},
      {W_SLIDER, "Guide opacity", &g_guideOpacity, NULL, 0, 1, false, false, 1, "%.2f"},
      {W_TOGGLE, "Pitch contour (P)", NULL, &g_showContour},
      {W_TOGGLE, "Suggested notes (N, A: accept all)", NULL, &g_suggest},
      {W_TOGGLE, "Note pitch from contour", NULL, &g_pitchFromContour},
      {W_HEADER, "Tempo"},
      {W_BUTTON, "Selection = 1 bar (B)", NULL, NULL, 0, 0, false, false, 0, NULL, B_BAR},
      {W_BUTTON, "Selection = 1 beat", NULL, NULL, 0, 0, false, false, 0, NULL, B_BEAT},
      {W_BUTTON, "Tempo marker at edit cursor", NULL, NULL, 0, 0, false, false, 0, NULL, B_TEMPO_MARKER},
      {W_SLIDER, "Quantise strength", &g_quantStrength, NULL, 0, 1, false, false, 100, "%.0f%%"},
      {W_BUTTON, "Quantise selected notes (Q)", NULL, NULL, 0, 0, false, false, 0, NULL, B_QUANTISE},
      {W_HEADER, "Playback"},
      {W_TOGGLE, "Play notes when clicked", NULL, &g_audition},
      {W_CHOICE, "Speed", NULL, NULL, 0, 0, false, false, 0, NULL, 0, &g_speed, SPEEDS, 3},
  };
}

static int widgetHeight(WidgetType t) { return t == W_HEADER ? 22 : t == W_SLIDER ? 32 : t == W_TOGGLE ? 20 : t == W_CHOICE || t == W_BUTTONS ? 42 : 24; }

// Spectrogram settings need the display values recomputed; guide settings only a redraw.
static bool isRenderParam(const void *p) { return p >= (const void *)&g_rp && p < (const void *)(&g_rp + 1); }

static double sliderFrac(const Widget &w, double v)
{
  return w.logScale ? log(v / w.lo) / log(w.hi / w.lo) : (v - w.lo) / (w.hi - w.lo);
}
static double sliderValue(const Widget &w, double f)
{
  f = std::max(0.0, std::min(1.0, f));
  return w.logScale ? w.lo * pow(w.hi / w.lo, f) : w.lo + (w.hi - w.lo) * f;
}

static int g_panelScroll; // pixels the panel is scrolled down

static int panelContentHeight()
{
  int h = 16;
  for (const Widget &w : widgets()) h += widgetHeight(w.type);
  return h + 70; // room for the key help
}

static void scrollPanel(int dy)
{
  const Rect p = panelArea();
  g_panelScroll = std::max(0, std::min(g_panelScroll + dy, panelContentHeight() - (p.b - p.t)));
  redraw();
}

static int widgetAt(int y)
{
  std::vector<Widget> ws = widgets();
  int top = 8 - g_panelScroll;
  for (size_t i = 0; i < ws.size(); i++)
  {
    const int h = widgetHeight(ws[i].type);
    if (y >= top && y < top + h) return (int)i;
    top += h;
  }
  return -1;
}

static void autoCalibrateView()
{
  const Rect n = noteArea();
  const int f0 = (int)((g_view.t(n.l) - g_audioPos) * g_spec.frameRate());
  const int f1 = (int)((g_view.t(n.r) - g_audioPos) * g_spec.frameRate()) + 1;
  autoCalibrate(g_spec, g_rp, f0, f1);
}

static void changedSetting(const Widget &w)
{
  if (w.analysis) { g_analysisDirty = true; redraw(); return; } // recomputed on mouse up
  if (w.value == &g_rp.noiseFraction) estimateNoise(g_spec, g_rp.noiseFraction);
  if (isRenderParam(w.value)) updateValues();
  else redraw();
}

// Settings are remembered per audio item (item extended state) and as defaults for new items
// (REAPER extended state), as "name=value;" pairs.
struct Setting
{
  const char *name;
  double *d;
  bool *b;
  int *i;
  double lo, hi; // loaded values are clamped to this range
};
static const Setting SETTINGS[] = {
    {"gamma", &g_np.gamma, NULL, NULL, 3, 40}, {"fmin", &g_np.fmin, NULL, NULL, 20, 500},
    {"fmax", &g_np.fmax, NULL, NULL, 1000, 11000}, {"channel", NULL, NULL, &g_channel, 0, CH_CENTRE},
    {"floor", &g_rp.dbFloor, NULL, NULL, -120, -20}, {"contrast", &g_rp.contrast, NULL, NULL, 0.1, 3},
    {"brightness", &g_rp.brightness, NULL, NULL, -1, 1}, {"bias", &g_rp.bias, NULL, NULL, -2, 2},
    {"peaks", NULL, &g_rp.peaksOnly}, {"peakdim", &g_rp.peakDim, NULL, NULL, 0, 1},
    {"denoise", NULL, &g_rp.subtractNoise}, {"noisealpha", &g_rp.noiseAlpha, NULL, NULL, 0, 3},
    {"noisefrac", &g_rp.noiseFraction, NULL, NULL, 0.01, 0.5}, {"ghosts", NULL, &g_ghosts},
    {"harmonics", &g_harmonics, NULL, NULL, 2, 16}, {"guideopacity", &g_guideOpacity, NULL, NULL, 0, 1},
    {"explain", NULL, &g_explain}, {"contour", NULL, &g_showContour}, {"suggest", NULL, &g_suggest},
    {"pitchfromcontour", NULL, &g_pitchFromContour}, {"audition", NULL, &g_audition},
    {"snap", NULL, NULL, &g_snapMode, 0, SNAP_GRID}, {"quantise", &g_quantStrength, NULL, NULL, 0, 1},
};
static const char *SETTINGS_KEY = "P_EXT:reascribe_settings", *EXT_SECTION = "reascribe", *EXT_DEFAULTS = "defaults";

static std::string saveSettings()
{
  std::string out;
  char buf[64];
  for (const Setting &st : SETTINGS)
  {
    if (st.d) snprintf(buf, sizeof(buf), "%s=%.6g;", st.name, *st.d);
    else if (st.b) snprintf(buf, sizeof(buf), "%s=%d;", st.name, *st.b ? 1 : 0);
    else snprintf(buf, sizeof(buf), "%s=%d;", st.name, *st.i);
    out += buf;
  }
  return out;
}

// Unknown names are ignored; values are clamped (they come from the project file).
static void loadSettings(const char *str)
{
  for (const char *p = str; p && *p;)
  {
    const char *eq = strchr(p, '='), *end = strchr(p, ';');
    if (!eq || !end || eq > end) break;
    const std::string name(p, eq - p);
    const double v = atof(eq + 1);
    for (const Setting &st : SETTINGS)
      if (name == st.name)
      {
        const double c = v >= st.lo ? std::min(v, st.hi) : st.lo; // NaN -> lo
        if (st.d) *st.d = c;
        else if (st.b) *st.b = v != 0;
        else *st.i = (int)c;
      }
    p = end + 1;
  }
}

static void resetSettings()
{
  g_np = NSGTParams(), g_rp = RenderParams(), g_channel = CH_MIX;
  g_ghosts = true, g_harmonics = 8, g_guideOpacity = 0.4, g_explain = false;
  g_showContour = g_suggest = g_pitchFromContour = g_audition = true;
  g_snapMode = SNAP_ONSETS, g_quantStrength = 1;
}

static void setupEditorSnap();
static std::string g_savedSettings; // last written to the item, to only write on change

// Returns true if the item had saved settings.
static bool loadItemSettings()
{
  resetSettings();
  loadSettings(GetExtState(EXT_SECTION, EXT_DEFAULTS));
  static char buf[65536]; // "NeedBig" API: no size is passed, so leave plenty of room
  buf[0] = 0;
  const bool has = GetSetMediaItemInfo_String(g_audio, SETTINGS_KEY, buf, false) && *buf;
  buf[sizeof(buf) - 1] = 0;
  if (has) loadSettings(buf);
  g_savedSettings = saveSettings();
  return has;
}

static void saveItemSettings()
{
  if (!g_audio || !ValidatePtr2(NULL, g_audio, "MediaItem*")) return;
  const std::string cur = saveSettings();
  if (cur == g_savedSettings) return;
  static char buf[65536];
  snprintf(buf, sizeof(buf), "%s", cur.c_str());
  GetSetMediaItemInfo_String(g_audio, SETTINGS_KEY, buf, true);
  g_savedSettings = cur;
}

static void applyPreset(int i)
{
  if (i == 0) g_channel = CH_CENTRE, g_rp.subtractNoise = true, g_np.fmin = 60, g_np.fmax = 5000, g_np.gamma = 11;
  else if (i == 1) g_channel = CH_MIX, g_rp.subtractNoise = false, g_np.fmin = 60, g_np.fmax = 6000, g_np.gamma = 11;
  else if (i == 2) g_np.gamma = 25; // wider bands, shorter windows: sharper timing for fast runs
  requestAnalysis(true);
}

static void pressButton(int b)
{
  if (b >= B_PRESET) { applyPreset(b - B_PRESET); return; }
  if (b == B_SAVE_DEFAULT) { SetExtState(EXT_SECTION, EXT_DEFAULTS, saveSettings().c_str(), true); return; }
  if (b == B_LOAD_DEFAULT)
  {
    resetSettings();
    loadSettings(GetExtState(EXT_SECTION, EXT_DEFAULTS));
    setupEditorSnap();
    requestAnalysis(true);
    return;
  }
  if (b == B_BAR || b == B_BEAT) { selectionIs(b == B_BAR); return; }
  if (b == B_TEMPO_MARKER) { tempoMarkerAtCursor(); return; }
  if (b == B_QUANTISE) { quantiseSelected(); return; }
  if (b == B_AUTO_ALL) autoCalibrate(g_spec, g_rp);
  else if (b == B_AUTO_VIEW) autoCalibrateView();
  else if (b == B_RESET)
  {
    resetSettings();
    setupEditorSnap();
    requestAnalysis(true);
  }
  updateValues();
}

// Master playrate; slowed down with pitch preserved (REAPER's "preserve pitch in audio items when
// changing master playrate" toggle is switched on if it is off).
static const int CMD_TOGGLE_PRESERVE_PITCH = 40671;

static void applySpeed()
{
  const double rate = SPEED_RATES[g_speed];
  if (rate != 1 && GetToggleCommandState(CMD_TOGGLE_PRESERVE_PITCH) == 0) Main_OnCommand(CMD_TOGGLE_PRESERVE_PITCH, 0);
  CSurf_OnPlayRateChange(rate);
}

static void panelMouse(int x, int y, bool down)
{
  const Rect p = panelArea();
  std::vector<Widget> ws = widgets();
  if (down) g_panelWidget = widgetAt(y);
  if (g_panelWidget < 0) return;
  const Widget &w = ws[g_panelWidget];
  if (w.type == W_SLIDER)
  {
    const double v = sliderValue(w, (double)(x - p.l - 12) / (p.r - p.l - 24));
    if (v != *w.value) *w.value = v, changedSetting(w);
  }
  else if (down && w.type == W_TOGGLE)
  {
    *w.flag = !*w.flag;
    if (isRenderParam(w.flag)) updateValues();
    else redraw();
  }
  else if (down && w.type == W_BUTTON) pressButton(w.button);
  else if (down && w.type == W_BUTTONS)
    pressButton(w.button + std::max(0, std::min(w.optionCount - 1, (x - p.l - 12) * w.optionCount / std::max(1, p.r - p.l - 24))));
  else if (down && w.type == W_CHOICE)
  {
    const int i = std::max(0, std::min(w.optionCount - 1, (x - p.l - 12) * w.optionCount / std::max(1, p.r - p.l - 24)));
    if (i == *w.choice) return;
    *w.choice = i;
    if (w.choice == &g_speed) applySpeed();
    else if (w.analysis) requestAnalysis(false);
  }
}

static void drawPanel(Canvas &cv, HDC dc)
{
  const Rect p = panelArea();
  if (p.r <= p.l) return;
  cv.fill(p.l, p.t, p.r, p.b, 0x222222);
  cv.fill(p.l, p.t, p.l + 1, p.b, 0x000000);
  // show the actual playrate, which can also be changed elsewhere
  const double rate = Master_GetPlayRate(NULL);
  for (int i = 0; i < 3; i++)
    if (fabs(SPEED_RATES[i] - rate) < fabs(SPEED_RATES[g_speed] - rate)) g_speed = i;
  std::vector<Widget> ws = widgets();
  int top = 8 - g_panelScroll;
  char buf[64];
  const int l = p.l + 12, r = p.r - 12;
  for (const Widget &w : ws)
  {
    const int h = widgetHeight(w.type);
    if (w.type == W_HEADER) text(dc, w.label, l, top + 4, r, top + h, C_ACCENT, DT_LEFT);
    else if (w.type == W_SLIDER)
    {
      text(dc, w.label, l, top, r, top + 16, C_TEXT, DT_LEFT);
      snprintf(buf, sizeof(buf), w.fmt, *w.value * w.display);
      text(dc, buf, l, top, r, top + 16, C_DIM, DT_RIGHT);
      const int by = top + 20, bx = l + (int)lround((r - l) * std::max(0.0, std::min(1.0, sliderFrac(w, *w.value))));
      cv.fill(l, by + 3, r, by + 7, 0x444444);
      cv.fill(l, by + 3, bx, by + 7, w.analysis && g_analysisDirty ? 0xc0a040 : 0x3a8a84);
      cv.fill(bx - 3, by, bx + 3, by + 10, C_TEXT);
    }
    else if (w.type == W_TOGGLE)
    {
      cv.frame(l, top + 5, l + 12, top + 17, C_TEXT);
      if (*w.flag) cv.fill(l + 3, top + 8, l + 9, top + 14, C_NOTE);
      text(dc, w.label, l + 18, top, r, top + 22, C_TEXT, DT_LEFT);
    }
    else if (w.type == W_CHOICE || w.type == W_BUTTONS)
    {
      text(dc, w.label, l, top, r, top + 16, C_TEXT, DT_LEFT);
      for (int i = 0; i < w.optionCount; i++)
      {
        const int bl = l + (r - l) * i / w.optionCount, br = l + (r - l) * (i + 1) / w.optionCount;
        const bool on = w.choice && i == *w.choice;
        cv.fill(bl, top + 18, br - 2, top + 40, on ? 0x3a8a84 : 0x333333);
        text(dc, w.options[i], bl, top + 18, br - 2, top + 40, on ? 0xffffff : C_TEXT, DT_CENTER);
      }
    }
    else
    {
      cv.fill(l, top + 2, r, top + h - 2, 0x333333);
      text(dc, w.label, l, top + 2, r, top + h - 2, C_TEXT, DT_CENTER);
    }
    top += h;
  }
  const char *help[] = {"Keys: S panel, G snap, F fit, Z zoom to", "selection, H ghosts, X explain, Enter lyric,",
                        "[ ] brightness, - = contrast, , . resolution", "Scroll here to see more settings."};
  for (const char *line : help) text(dc, line, l, top + 6, r, top + 22, C_DIM, DT_LEFT), top += 16;
}

// --- overtone guides ------------------------------------------------------------------

// Strongest level (dB) within half a semitone of `pitch` between times a and b.
static double levelDb(double pitch, double a, double b)
{
  const Spectrogram &s = g_spec;
  if (!s.frames) return -200;
  const int f0 = std::max(0, (int)floor((a - g_audioPos) * s.frameRate()));
  const int f1 = std::min(s.frames - 1, (int)ceil((b - g_audioPos) * s.frameRate()));
  const int b0 = std::max(0, (int)ceil(s.binOfPitch(pitch - 0.5))), b1 = std::min(s.bins - 1, (int)floor(s.binOfPitch(pitch + 0.5)));
  float m = 0;
  for (int f = f0; f <= f1; f++)
    for (int k = b0; k <= b1; k++) m = std::max(m, s.at(f, k));
  return 20 * log10(std::max(m, 1e-10f));
}

static double overtonePitch(double p, int h) { return p + 12 * log2((double)h); }

// Note ghosts: faint outlines where each note's overtones should be.
static void drawGhosts(Canvas &cv)
{
  if (!g_ghosts) return;
  const int a = (int)lround(g_guideOpacity * 160), n = (int)lround(g_harmonics);
  for (const Note &nt : g_ed.notes)
  {
    const int x0 = (int)lround(g_view.x(nt.start)), x1 = std::max(x0 + 2, (int)lround(g_view.x(nt.end)));
    for (int h = 2; h <= n; h++)
    {
      const double y = g_view.y(overtonePitch(nt.pitch, h)), hh = std::max(1.0, g_view.rowH * 0.3);
      const int y0 = (int)lround(y - hh), y1 = (int)lround(y + hh) + 2;
      cv.fill(x0, y0, x1, y0 + 2, C_NOTE, a), cv.fill(x0, y1 - 2, x1, y1, C_NOTE, a);
      cv.fill(x0, y0, x0 + 2, y1, C_NOTE, a), cv.fill(x1 - 2, y0, x1, y1, C_NOTE, a);
    }
  }
}

// Hover guides: overtone marks for the pitch under the mouse (or the note being dragged), plus the
// octave check: the pitches this one would be the 2nd, 3rd or 4th overtone of, with their level
// relative to it. A strong one means the real note is probably lower.
static void drawHoverGuides(Canvas &cv, HDC dc, const Rect &n)
{
  const int dn = g_ed.dragNote();
  if (dn < 0 && !n.in(g_mouseX, g_mouseY)) return;
  int p0;
  double tA, tB;
  int xA, xB;
  if (dn >= 0)
  {
    const Note &nt = g_ed.notes[dn];
    p0 = nt.pitch, tA = nt.start, tB = nt.end;
    xA = (int)lround(g_view.x(tA)), xB = std::max(xA + 40, (int)lround(g_view.x(tB)));
  }
  else
  {
    p0 = g_view.pitchAt(g_mouseY), tA = g_view.t(g_mouseX) - 0.03, tB = g_view.t(g_mouseX) + 0.03;
    xA = g_mouseX - 70, xB = g_mouseX + 70;
  }
  const int a = (int)lround(g_guideOpacity * 256), nh = (int)lround(g_harmonics);
  char buf[32];
  for (int h = 1; h <= nh; h++)
  {
    const int y = (int)lround(g_view.y(overtonePitch(p0, h)));
    if (y < n.t || y >= n.b) continue;
    const int t = GUIDE_THICKNESS + (h == 1 ? 1 : 0);
    cv.fill(xA, y - t / 2, xB, y - t / 2 + t, 0xffffff, h == 1 ? 256 : a);
    snprintf(buf, sizeof(buf), "%d", h);
    text(dc, buf, xB + 3, y - 8, xB + 30, y + 8, 0xffffff, DT_LEFT);
  }
  const double own = levelDb(p0, tA, tB);
  static const struct { int h; const char *label; } below[] = {{2, "8vb"}, {3, "12th"}, {4, "2 oct"}};
  for (const auto &c : below)
  {
    const double pc = overtonePitch(p0, 1) - 12 * log2((double)c.h);
    const int y = (int)lround(g_view.y(pc));
    if (y < n.t || y >= n.b || g_spec.binOfPitch(pc) < 0) continue;
    const double rel = levelDb(pc, tA, tB) - own;
    const bool strong = rel > -12;
    for (int x = xA; x < xB; x += 8)
      cv.fill(x, y - GUIDE_THICKNESS / 2, x + 4, y - GUIDE_THICKNESS / 2 + GUIDE_THICKNESS, strong ? 0xffa040 : 0xa0a0a0, strong ? 256 : a);
    snprintf(buf, sizeof(buf), "%s %+.0f dB", c.label, rel);
    text(dc, buf, xB + 3, y - 8, xB + 120, y + 8, strong ? 0xffa040 : 0xa0a0a0, DT_LEFT);
  }
}

// --- contour and suggestions -------------------------------------------------------------

// Suggested notes are segments not already covered by a drawn note (vocals are one line at a time).
static bool covered(const Segment &sg)
{
  const double a = frameTime(sg.f0), b = frameTime(sg.f1 + 1);
  for (const Note &n : g_ed.notes)
    if (std::min(b, n.end) - std::max(a, n.start) > 0.3 * (b - a)) return true;
  return false;
}

static void segmentRect(const Segment &sg, int *x0, int *y0, int *x1, int *y1)
{
  const int p = (int)lround(sg.pitch);
  *x0 = (int)lround(g_view.x(frameTime(sg.f0))), *x1 = std::max(*x0 + 2, (int)lround(g_view.x(frameTime(sg.f1 + 1))));
  *y0 = (int)lround(g_view.y(p + 0.5)), *y1 = std::max(*y0 + 3, (int)lround(g_view.y(p - 0.5)));
}

static int suggestionAt(int x, int y)
{
  if (!g_suggest) return -1;
  for (size_t i = 0; i < g_segments.size(); i++)
  {
    int x0, y0, x1, y1;
    segmentRect(g_segments[i], &x0, &y0, &x1, &y1);
    if (x >= x0 - 2 && x < x1 + 2 && y >= y0 - 2 && y < y1 + 2 && !covered(g_segments[i])) return (int)i;
  }
  return -1;
}

static void drawSuggestions(Canvas &cv, const Rect &n)
{
  if (!g_suggest) return;
  for (const Segment &sg : g_segments)
  {
    int x0, y0, x1, y1;
    segmentRect(sg, &x0, &y0, &x1, &y1);
    if (x1 < n.l || x0 > n.r || covered(sg)) continue;
    cv.fill(x0, y0, x1, y1, C_NOTE, 50);
    for (int x = x0; x < x1; x += 6) cv.fill(x, y0, std::min(x + 3, x1), y0 + 1, C_NOTE), cv.fill(x, y1 - 1, std::min(x + 3, x1), y1, C_NOTE);
    for (int y = y0; y < y1; y += 4) cv.fill(x0, y, x0 + 1, std::min(y + 2, y1), C_NOTE), cv.fill(x1 - 1, y, x1, std::min(y + 2, y1), C_NOTE);
  }
}

// The sung pitch as a white line with a dark edge, one pixel column at a time.
static void drawContour(Canvas &cv, const Rect &n)
{
  if (!g_showContour || g_contour.pitch.empty()) return;
  const double fr = g_spec.frameRate();
  auto pitchAt = [&](double f) -> double { // linear between voiced frames, -1 if unvoiced
    const int a = (int)floor(f), b = a + 1;
    if (a < 0 || b >= (int)g_contour.pitch.size()) return -1;
    const double pa = g_contour.pitch[a], pb = g_contour.pitch[b];
    if (pa < 0 || pb < 0) return pa >= 0 && f - a < 0.5 ? pa : pb >= 0 && f - a >= 0.5 ? pb : -1;
    return pa + (pb - pa) * (f - a);
  };
  double prev = -1;
  for (int x = n.l; x < n.r; x++)
  {
    const double fa = (g_view.t(x) - g_audioPos) * fr, fb = (g_view.t(x + 1) - g_audioPos) * fr;
    double lo = 1e9, hi = -1e9;
    auto add = [&](double p) { if (p >= 0) lo = std::min(lo, p), hi = std::max(hi, p); };
    add(pitchAt(fa)), add(pitchAt(fb));
    for (int f = (int)ceil(fa); f < fb; f++) add(f >= 0 && f < (int)g_contour.pitch.size() ? g_contour.pitch[f] : -1);
    if (hi < 0) { prev = -1; continue; }
    if (prev >= 0) add(prev); // connect to the previous column
    prev = pitchAt(fb) >= 0 ? pitchAt(fb) : hi;
    const int y0 = (int)lround(g_view.y(hi)), y1 = (int)lround(g_view.y(lo));
    cv.fill(x, y0 - 2, x + 1, y1 + 3, 0x000000, 180);
    cv.fill(x, y0 - 1, x + 1, y1 + 2, 0xffffff);
  }
}

// --- paint -------------------------------------------------------------------------

static void placeEdit();

static void paint(HDC hdc)
{
  const Rect c = clientRect();
  if (c.r < 2 || c.b < 2) return;
  const int w = (c.r + 3) & ~3; // SWELL backing stores are a multiple of 4 wide
  if (!g_memdc || w != g_memw || c.b != g_memh)
  {
    if (g_memdc) SWELL_DeleteGfxContext(g_memdc);
    g_memdc = SWELL_CreateMemContext(hdc, w, c.b);
    g_memw = w, g_memh = c.b;
    if (!g_font)
      g_font = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Helvetica");
    if (g_memdc && g_font) SelectObject(g_memdc, g_font);
  }
  unsigned int *fb = g_memdc ? (unsigned int *)SWELL_GetCtxFrameBuffer(g_memdc) : NULL;
  if (!fb) return;
  if (c.r < MIN_W || c.b < MIN_H)
  {
    for (size_t i = 0; i < (size_t)w * c.b; i++) fb[i] = 0xff000000 | C_BG;
    BitBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, SRCCOPY);
    return;
  }
  syncViewOrigin();
  const Rect n = noteArea(), ru = rulerArea();
  Canvas cv = {fb, w, c.b, {0, 0, w, c.b}};
  cv.fill(0, 0, w, c.b, C_BG);
  HDC dc = g_memdc;
  SetBkMode(dc, TRANSPARENT);
  char buf[160];

  // spectrogram
  LayerKey key = {g_view.t0, g_view.pps, g_view.pitchTop, g_view.rowH, n.r - n.l, n.b - n.t, g_valVersion,
                  g_explain ? g_notesVersion * 32 + (int)lround(g_harmonics) : 0};
  if (!g_layerValid || !(key == g_layerKey))
  {
    renderLayer(n);
    g_layerKey = key, g_layerValid = true;
  }
  for (int y = n.t; y < n.b; y++) memcpy(fb + (size_t)y * w + n.l, &g_layer[(size_t)(y - n.t) * (n.r - n.l)], (n.r - n.l) * 4);

  // pitch rows, then the time grid
  cv.clip = n;
  const int pLo = (int)floor(g_view.p(n.b)), pHi = (int)ceil(g_view.p(n.t));
  for (int p = pLo; p <= pHi; p++)
  {
    const int y = (int)lround(g_view.y(p - 0.5)); // boundary below pitch p
    cv.fill(n.l, y, n.r, y + 1, 0xffffff, p % 12 == 0 ? 60 : 16);
  }
  std::vector<GridLine> grid = gridLines(n);
  for (const GridLine &g : grid) cv.fill(g.x, n.t, g.x + 1, n.b, 0xffffff, g.level == 0 ? 70 : g.level == 1 ? 32 : 14);

  drawGhosts(cv);
  drawContour(cv, n);
  drawSuggestions(cv, n);
  drawHoverGuides(cv, dc, n);

  // notes
  const int drawing = g_ed.drawing();
  for (size_t i = 0; i < g_ed.notes.size(); i++)
  {
    const Note &nt = g_ed.notes[i];
    const int x0 = (int)lround(g_view.x(nt.start)), x1 = std::max(x0 + 2, (int)lround(g_view.x(nt.end)));
    const int y0 = (int)lround(g_view.y(nt.pitch + 0.5)), y1 = std::max(y0 + 3, (int)lround(g_view.y(nt.pitch - 0.5)));
    const bool sel = nt.sel || (int)i == drawing;
    cv.fill(x0, y0, x1, y1, sel ? C_SEL : C_NOTE, 235);
    cv.frame(x0, y0, x1, y1, sel ? C_SEL_EDGE : C_NOTE_EDGE, sel && y1 - y0 > 6 ? 2 : 1);
  }
  // lyrics: inside the note when it is tall enough, else just below it
  for (const Note &nt : g_ed.notes)
  {
    if (nt.lyric.empty()) continue;
    const int x0 = (int)lround(g_view.x(nt.start)), x1 = std::max(x0 + 2, (int)lround(g_view.x(nt.end)));
    const int y0 = (int)lround(g_view.y(nt.pitch + 0.5)), y1 = (int)lround(g_view.y(nt.pitch - 0.5));
    const int l = std::max(x0 + 2, n.l), r = std::min(std::max(x1, x0 + 60), n.r);
    if (r <= l) continue;
    if (y1 - y0 >= 12) text(dc, nt.lyric.c_str(), l, std::max(y0, n.t), std::min(x1 - 1, n.r), std::min(y1, n.b), nt.sel ? 0x0b3d3a : 0x04201e, DT_LEFT);
    else if (y1 + 14 <= n.b) text(dc, nt.lyric.c_str(), l, y1, r, y1 + 14, 0xffffff, DT_LEFT);
  }

  double mr[4];
  if (g_ed.marquee(mr))
  {
    cv.fill((int)mr[0], (int)mr[1], (int)mr[2], (int)mr[3], 0xffffff, 30);
    cv.frame((int)mr[0], (int)mr[1], (int)mr[2], (int)mr[3], 0xffffff);
  }

  // keyboard
  cv.clip = {0, n.t, n.l, n.b};
  const int hoverPitch = n.in(g_mouseX, g_mouseY) || (g_mouseX < n.l && g_mouseY >= n.t) ? g_view.pitchAt(g_mouseY) : -1;
  for (int p = pLo; p <= pHi; p++)
  {
    const int y0 = (int)lround(g_view.y(p + 0.5)), y1 = (int)lround(g_view.y(p - 0.5));
    cv.fill(0, y0, n.l, y1, p == hoverPitch ? C_ACCENT : isBlack(p) ? 0x303030 : 0xd8d8d8);
    cv.fill(0, y1 - 1, n.l, y1, 0x888888);
    if (p % 12 == 0 && y1 - y0 >= 8)
    {
      pitchName(p, buf, sizeof(buf));
      text(dc, buf, 2, y0, n.l - 4, y1, 0x202020, DT_RIGHT);
    }
  }
  cv.fill(n.l - 1, n.t, n.l, n.b, 0x000000);

  // ruler: bars on top, seconds below
  cv.clip = {0, 0, ru.r, RULER_H};
  cv.fill(0, 0, ru.r, RULER_H, C_RULER);
  cv.fill(0, RULER_H - 1, ru.r, RULER_H, 0x000000);
  int lastLabel = -1000;
  for (const GridLine &g : grid)
  {
    if (g.level > 1) continue;
    cv.fill(g.x, g.level == 0 ? 0 : 8, g.x + 1, 14, 0xa0a0a0);
    if (g.level == 0 && g.x - lastLabel >= 36)
    {
      snprintf(buf, sizeof(buf), "%d", g.measure + 1);
      text(dc, buf, g.x + 3, 0, g.x + 60, 14, C_TEXT, DT_LEFT);
      lastLabel = g.x;
    }
  }
  double secStep = 0.01;
  for (double s : {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0})
    if ((secStep = s) * g_view.pps >= 70) break;
  for (double t = ceil(g_view.t(n.l) / secStep) * secStep; t <= g_view.t(n.r); t += secStep)
  {
    const int x = (int)lround(g_view.x(t));
    cv.fill(x, 22, x + 1, RULER_H - 1, 0x707070);
    format_timestr_pos(t, buf, sizeof(buf), 0);
    text(dc, buf, x + 3, 14, x + 90, RULER_H - 1, C_DIM, DT_LEFT);
  }
  for (int f : g_onsets)
  {
    const int ox = (int)lround(g_view.x(frameTime(f)));
    cv.fill(ox - 1, RULER_H - 6, ox + 2, RULER_H - 1, 0xffa040);
  }
  cv.clip = {0, 0, KB_W, RULER_H};
  cv.fill(0, 0, KB_W, RULER_H, 0x303030);
  text(dc, g_panel ? "Hide (S)" : "Settings", 0, 0, KB_W, RULER_H, C_TEXT, DT_CENTER);

  // time selection
  cv.clip = {n.l, 0, n.r, n.b};
  double selA, selB;
  GetSet_LoopTimeRange2(NULL, false, false, &selA, &selB, false);
  if (selB > selA)
  {
    const int xa = (int)lround(g_view.x(selA)), xb = (int)lround(g_view.x(selB));
    cv.fill(xa, 0, xb, n.b, 0xffffff, 22);
    cv.fill(xa, 0, xa + 1, n.b, 0xffffff, 90), cv.fill(xb, 0, xb + 1, n.b, 0xffffff, 90);
  }

  // cursors
  int x = (int)lround(g_view.x(GetCursorPosition()));
  cv.fill(x, 0, x + 1, n.b, C_EDIT_CURSOR);
  if (GetPlayState() & 1)
  {
    x = (int)lround(g_view.x(GetPlayPosition2()));
    cv.fill(x, 0, x + 1, n.b, C_PLAY_CURSOR);
  }

  // status line: pitch, frequency, cents of the strongest nearby peak, time, bar.beat, level
  cv.clip = {0, n.b, c.r, c.b};
  cv.fill(0, n.b, c.r, c.b, C_RULER);
  std::string status;
  if (n.in(g_mouseX, g_mouseY))
  {
    const double t = g_view.t(g_mouseX), p = g_view.p(g_mouseY);
    const int ip = (int)floor(p + 0.5);
    char name[16], tm[32], bb[32];
    pitchName(ip, name, sizeof(name));
    format_timestr_pos(t, tm, sizeof(tm), 0);
    format_timestr_pos(t, bb, sizeof(bb), 2);
    snprintf(buf, sizeof(buf), "%-4s %7.1f Hz   %s   %s", name, 440 * pow(2.0, (p - 69) / 12), tm, bb);
    status = buf;
    const int f = (int)lround((t - g_audioPos) * g_spec.frameRate());
    if (f >= 0 && f < g_spec.frames)
    {
      const int b0 = std::max(0, (int)floor(g_spec.binOfPitch(ip - 1))), b1 = std::min(g_spec.bins - 1, (int)ceil(g_spec.binOfPitch(ip + 1)));
      int best = -1;
      for (int b = b0; b <= b1; b++)
        if (best < 0 || g_spec.at(f, b) > g_spec.at(f, best)) best = b;
      if (best >= 0)
      {
        const double peakPitch = 69 + 12 * log2(g_spec.p.fmin * pow(2.0, (double)best / g_spec.p.binsPerOctave) / 440);
        const int pp = (int)floor(peakPitch + 0.5);
        pitchName(pp, name, sizeof(name));
        snprintf(buf, sizeof(buf), "   peak %s %+.0f c   %.0f dB", name, (peakPitch - pp) * 100,
                 20 * log10(std::max(1e-10f, g_spec.at(f, std::max(0, std::min(g_spec.bins - 1, (int)lround(g_spec.binOfPitch(p))))))));
        status += buf;
      }
    }
  }
  int hovered = -1;
  if (n.in(g_mouseX, g_mouseY) && g_ed.hitTest(g_view, g_mouseX, g_mouseY, &hovered) != NoteEditor::NONE)
  {
    const Note &hn = g_ed.notes[hovered];
    double sung;
    const int f0 = (int)lround((hn.start - g_audioPos) * g_spec.frameRate()), f1 = (int)lround((hn.end - g_audioPos) * g_spec.frameRate());
    if (contourMedian(g_contour, f0, f1, (int)lround(0.05 * g_spec.frameRate()), &sung))
    {
      snprintf(buf, sizeof(buf), "   note sung %+.0f c", (sung - hn.pitch) * 100);
      status += buf;
    }
  }
  static const char *const SNAP_NAMES[] = {"off", "onsets", "grid"};
  if (g_job)
  {
    const int pm = g_job->permille;
    snprintf(buf, sizeof(buf), "analysing %d%%   snap: %s (G)", pm / 10, SNAP_NAMES[g_snapMode]);
    cv.clip = {0, 0, w, c.b};
    cv.fill(n.l, n.t, n.l + (n.r - n.l) * pm / 1000, n.t + 3, C_ACCENT);
    cv.clip = {0, n.b, c.r, c.b};
  }
  else snprintf(buf, sizeof(buf), "snap: %s (G)", SNAP_NAMES[g_snapMode]);
  text(dc, status.c_str(), 6, n.b, c.r - 120, c.b, C_TEXT, DT_LEFT);
  text(dc, buf, 6, n.b, c.r - 6, c.b, C_DIM, DT_RIGHT);

  cv.clip = {0, 0, w, c.b};
  if (panelShown()) drawPanel(cv, dc);

  BitBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, SRCCOPY);
  if (g_edit) placeEdit(); // follow scrolling and zooming while typing a lyric
}

// --- navigation --------------------------------------------------------------------

static void fitItem()
{
  syncViewOrigin();
  const Rect n = noteArea();
  g_view.pps = std::max(5.0, (n.r - n.l) / std::max(0.1, g_audioLen * 1.02));
  g_view.t0 = g_audioPos - 0.01 * g_audioLen;
  g_view.rowH = std::max(3.0, (double)(n.b - n.t) / (84 - 40 + 1)); // vocal range E2..C6
  g_view.pitchTop = 84.5;
  redraw();
}

static void zoomToSelection()
{
  double a = 1e30, b = -1e30;
  int lo = 128, hi = -1;
  for (const Note &nt : g_ed.notes)
    if (nt.sel) a = std::min(a, nt.start), b = std::max(b, nt.end), lo = std::min(lo, nt.pitch), hi = std::max(hi, nt.pitch);
  if (hi < 0) return;
  const Rect n = noteArea();
  const double len = std::max(0.2, b - a);
  g_view.pps = std::min(3000.0, (n.r - n.l) / (len * 1.2));
  g_view.t0 = a - len * 0.1;
  g_view.rowH = std::min(60.0, std::max(3.0, (double)(n.b - n.t) / (hi - lo + 7)));
  g_view.pitchTop = hi + 3.5;
  redraw();
}

static void zoomTime(double factor, int x)
{
  const double t = g_view.t(x);
  g_view.pps = std::min(3000.0, std::max(2.0, g_view.pps * factor));
  g_view.t0 = t - (x - g_view.x0) / g_view.pps;
  redraw();
}

static void zoomPitch(double factor, int y)
{
  const double p = g_view.p(y);
  g_view.rowH = std::min(60.0, std::max(3.0, g_view.rowH * factor));
  g_view.pitchTop = p + (y - g_view.y0) / g_view.rowH;
  redraw();
}

static void scrollPitch(double semitones)
{
  g_view.pitchTop = std::min(128.0, std::max(g_view.pitchTop + semitones, (noteArea().b - noteArea().t) / g_view.rowH - 0.5));
  redraw();
}

// --- note preview ----------------------------------------------------------------------

// Plays a short note through the notes track (so through its synth) as a track preview, from a
// tiny generated MIDI file per pitch; nothing needs to be armed. Previews always play to the end:
// stopping one mid-note leaves the synth without a note-off, so a pool lets them overlap.
// Seconds (at normal speed) before a finished preview is stopped. The file is ~1 s at 120 bpm;
// generous in case REAPER plays it at a slower project tempo, since stopping before the
// note-off hangs the note.
static const double PREVIEW_LIFE = 2.5;
static const int PREVIEW_SLOTS = 16;
struct Preview
{
  preview_register_t reg;
  bool init, playing;
  double started;
};
static Preview g_previews[PREVIEW_SLOTS];

static void stopPreview(Preview &p)
{
  if (p.playing) StopTrackPreview2(NULL, &p.reg);
  p.playing = false;
  delete p.reg.src;
  p.reg.src = NULL;
}

static void stopPreviews(bool onlyFinished)
{
  const double now = time_precise();
  // previews follow the master playrate, so at 50% speed they take twice as long
  const double life = PREVIEW_LIFE / std::max(0.1, Master_GetPlayRate(NULL));
  for (Preview &p : g_previews)
    if (p.playing && (!onlyFinished || now - p.started > life)) stopPreview(p);
}

static void previewPitch(int pitch)
{
  if (!g_audition || pitch < 0 || pitch > 127 || !g_midi || !ValidatePtr2(NULL, g_midi, "MediaItem*")) return;
  stopPreviews(true);
  Preview *slot = NULL;
  for (Preview &p : g_previews)
    if (!p.playing) { slot = &p; break; }
  if (!slot) return; // all busy: skip rather than cut a note off

  // Written fresh every time into REAPER's per-user resource folder, never following a symlink.
  // One file per pitch, in case REAPER caches MIDI file sources by path; files of playing
  // previews are only rewritten with identical bytes.
  char path[2048];
  snprintf(path, sizeof(path), "%s/Data/reascribe-preview-%d.mid", GetResourcePath(), pitch);
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) return;
  // format 0, 96 ticks per quarter: note on; note off after 77 ticks (~0.4 s at 120 bpm); all notes
  // off; end of track 96 ticks later, so the note-off isn't on the source's last tick (where it
  // was not played, leaving the note hanging)
  const unsigned char smf[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 16,
                               0, 0x90, (unsigned char)pitch, 96,
                               77, 0x80, (unsigned char)pitch, 0,
                               10, 0xb0, 0x7b, 0,
                               96, 0xff, 0x2f, 0};
  const bool ok = write(fd, smf, sizeof(smf)) == (ssize_t)sizeof(smf);
  close(fd);
  if (!ok) return;
  if (!slot->init) pthread_mutex_init(&slot->reg.mutex, NULL), slot->init = true;
  slot->reg.src = PCM_Source_CreateFromFile(path);
  if (!slot->reg.src) return;
  slot->reg.m_out_chan = -1; // -1: play through preview_track
  slot->reg.preview_track = GetMediaItem_Track(g_midi);
  slot->reg.curpos = 0, slot->reg.loop = false, slot->reg.volume = 1;
  slot->playing = PlayTrackPreview2Ex(NULL, &slot->reg, 0, 0) != 0;
  slot->started = time_precise();
  if (!slot->playing) stopPreview(*slot);
}

static int g_lastDragPitch = -1;

// While drawing or moving, play the dragged note again whenever its pitch changes.
static void previewDragged()
{
  const int dn = g_ed.dragNote();
  const int pitch = dn >= 0 ? g_ed.notes[dn].pitch : -1;
  if (pitch >= 0 && pitch != g_lastDragPitch) previewPitch(pitch);
  g_lastDragPitch = pitch;
}

// --- lyrics ------------------------------------------------------------------------------

static int findNote(double start, int pitch)
{
  for (size_t i = 0; i < g_ed.notes.size(); i++)
    if (fabs(g_ed.notes[i].start - start) < 1e-6 && g_ed.notes[i].pitch == pitch) return (int)i;
  return -1;
}

// The note after (dir > 0) or before (dir < 0) note i in time; same-start notes ordered by pitch.
static int neighbourNote(int i, int dir)
{
  const Note &c = g_ed.notes[i];
  int best = -1;
  for (size_t j = 0; j < g_ed.notes.size(); j++)
  {
    const Note &o = g_ed.notes[j];
    const bool after = o.start > c.start || (o.start == c.start && o.pitch > c.pitch);
    const bool before = o.start < c.start || (o.start == c.start && o.pitch < c.pitch);
    if ((int)j == i || (dir > 0 ? !after : !before)) continue;
    if (best < 0) { best = (int)j; continue; }
    const Note &b = g_ed.notes[best];
    const bool closer = dir > 0 ? (o.start < b.start || (o.start == b.start && o.pitch < b.pitch))
                                : (o.start > b.start || (o.start == b.start && o.pitch > b.pitch));
    if (closer) best = (int)j;
  }
  return best;
}

static void placeEdit()
{
  const int i = findNote(g_editStart, g_editPitch);
  if (!g_edit || i < 0) return;
  const Note &nt = g_ed.notes[i];
  const int x = (int)lround(g_view.x(nt.start)), y = (int)lround(g_view.y(nt.pitch - 0.5));
  const int w = std::max(90, (int)lround(g_view.x(nt.end)) - x);
  RECT r;
  GetWindowRect(g_edit, &r);
  POINT pt = {r.left, r.top};
  ScreenToClient(g_hwnd, &pt);
  if (pt.x != x || pt.y != y + 1 || r.right - r.left != w) // moving it repaints us, so only when needed
    SetWindowPos(g_edit, NULL, x, y + 1, w, 20, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void beginLyric(int i)
{
  if (i < 0 || i >= (int)g_ed.notes.size()) return;
  // scroll so the note is visible
  const Rect n = noteArea();
  const Note &nt = g_ed.notes[i];
  if (g_view.x(nt.start) < n.l + 20 || g_view.x(nt.start) > n.r - 100) g_view.t0 = nt.start - 60 / g_view.pps - (n.l - g_view.x0) / g_view.pps;
  if (g_view.y(nt.pitch) < n.t + 20 || g_view.y(nt.pitch) > n.b - 40) g_view.pitchTop = nt.pitch + 0.5 * (n.b - n.t) / g_view.rowH;
  for (Note &o : g_ed.notes) o.sel = false;
  g_ed.notes[i].sel = true;
  g_editStart = nt.start, g_editPitch = nt.pitch;
  if (!g_edit)
  {
    SWELL_MakeSetCurParms(1, 1, 0, 0, g_hwnd, false, false);
    g_edit = SWELL_MakeEditField(1001, 0, 0, 90, 20, 0);
    SWELL_MakeSetCurParms(1, 1, 0, 0, NULL, false, false);
  }
  if (!g_edit) return;
  SetWindowText(g_edit, nt.lyric.c_str());
  placeEdit();
  ShowWindow(g_edit, SW_SHOW);
  SetFocus(g_edit);
  SendMessage(g_edit, EM_SETSEL, 0, -1);
  redraw();
}

// commit: store the typed text; dir: then edit the next (1) / previous (-1) note, or stop (0)
static void endLyric(bool commit, int dir)
{
  if (!g_edit) return;
  char buf[1024] = "";
  GetWindowText(g_edit, buf, sizeof(buf));
  DestroyWindow(g_edit);
  g_edit = NULL;
  SetFocus(g_hwnd);
  int i = findNote(g_editStart, g_editPitch);
  if (i < 0) return;
  std::string text = buf;
  while (!text.empty() && isspace((unsigned char)text.back())) text.pop_back();
  while (!text.empty() && isspace((unsigned char)text[0])) text.erase(0, 1);
  const int next = dir ? neighbourNote(i, dir) : -1;
  double nextStart = 0;
  int nextPitch = -1;
  if (next >= 0) nextStart = g_ed.notes[next].start, nextPitch = g_ed.notes[next].pitch;
  if (commit && text != g_ed.notes[i].lyric)
  {
    g_ed.notes[i].lyric = text;
    commitNotes();
  }
  if (nextPitch >= 0) beginLyric(findNote(nextStart, nextPitch));
  redraw();
}

static int firstSelected()
{
  int best = -1;
  for (size_t i = 0; i < g_ed.notes.size(); i++)
    if (g_ed.notes[i].sel && (best < 0 || g_ed.notes[i].start < g_ed.notes[best].start)) best = (int)i;
  return best;
}

// --- input -------------------------------------------------------------------------

static int mods()
{
  return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MOD_CMD : 0) |
         ((GetAsyncKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0);
}

static double snapToOnsets(double t)
{
  auto it = std::lower_bound(g_snapTimes.begin(), g_snapTimes.end(), t);
  double best = t, dist = ONSET_SNAP_RANGE;
  if (it != g_snapTimes.end() && *it - t <= dist) best = *it, dist = *it - t;
  if (it != g_snapTimes.begin() && t - *(it - 1) < dist) best = *(it - 1);
  return best;
}

static void setupEditorSnap()
{
  if (g_snapMode == SNAP_GRID) g_ed.snap = snapToGrid;
  else if (g_snapMode == SNAP_ONSETS) g_ed.snap = snapToOnsets;
  else g_ed.snap = [](double t) { return t; };
}

static void adjustGamma(double delta)
{
  g_np.gamma = std::max(3.0, std::min(40.0, g_np.gamma + delta));
  requestAnalysis(false);
}

static void acceptSuggestion(int i)
{
  const Segment &sg = g_segments[i];
  for (Note &o : g_ed.notes) o.sel = false;
  g_ed.notes.push_back({frameTime(sg.f0), frameTime(sg.f1 + 1), (int)lround(sg.pitch), true, -1});
  previewPitch((int)lround(sg.pitch));
  commitNotes();
}

static void acceptVisibleSuggestions()
{
  const Rect n = noteArea();
  bool any = false;
  for (const Segment &sg : g_segments)
  {
    const double a = frameTime(sg.f0), b = frameTime(sg.f1 + 1);
    if (b < g_view.t(n.l) || a > g_view.t(n.r) || covered(sg)) continue;
    g_ed.notes.push_back({a, b, (int)lround(sg.pitch), false, -1});
    any = true;
  }
  if (any) commitNotes();
}

// A freshly drawn note takes the median sung pitch under it when that is within two semitones.
static void pitchFromContour(Note &nt)
{
  const double fr = g_spec.frameRate();
  double sung;
  if (contourMedian(g_contour, (int)lround((nt.start - g_audioPos) * fr), (int)lround((nt.end - g_audioPos) * fr),
                    (int)lround(0.05 * fr), &sung) &&
      fabs(sung - nt.pitch) <= 2)
    nt.pitch = (int)lround(sung);
}

// cmd+V: one syllable per note, to the selected notes (several selected) or from the selected
// note onwards (one selected, or none: from the first note).
static void pasteLyrics()
{
  std::string text;
  if (!OpenClipboard(g_hwnd)) return;
  if (HANDLE h = GetClipboardData(CF_TEXT))
  {
    if (const char *p = (const char *)GlobalLock(h)) text = p;
    GlobalUnlock(h);
  }
  CloseClipboard();
  const std::vector<std::string> syl = splitSyllables(text);
  if (syl.empty()) return;

  std::vector<int> order(g_ed.notes.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
  std::sort(order.begin(), order.end(), [](int a, int b) {
    const Note &x = g_ed.notes[a], &y = g_ed.notes[b];
    return x.start < y.start || (x.start == y.start && x.pitch < y.pitch);
  });
  int selected = 0;
  for (const Note &nt : g_ed.notes) selected += nt.sel;
  std::vector<int> targets;
  bool started = selected == 0;
  for (int i : order)
  {
    if (selected > 1 ? g_ed.notes[i].sel : (started |= g_ed.notes[i].sel)) targets.push_back(i);
  }
  for (size_t k = 0; k < targets.size() && k < syl.size(); k++) g_ed.notes[targets[k]].lyric = syl[k];
  commitNotes();
}

// Punctuation arrives as a character code without FVIRTKEY, and some collide with VK codes
// ('.' == VK_DELETE), so keys are encoded as key | CHARKEY for characters.
static const int CHARKEY = 0x10000;
static int keyCode(int key, int flags) { return (flags & FVIRTKEY) ? key : key | CHARKEY; }

// returns true if handled
static bool onKey(int key)
{
  const int m = mods();
  if (key == VK_DELETE || key == VK_BACK) { if (g_ed.deleteSelected()) commitNotes(); }
  else if (key == VK_ESCAPE) { if (g_ed.active()) g_ed.cancel(), g_drag = DRAG_NONE, ReleaseCapture(); else if (g_ed.selectAll(false)) commitNotes(); }
  else if (key == 'A' && (m & MOD_CMD)) { if (g_ed.selectAll(true)) commitNotes(); }
  else if (key == VK_RETURN)
  {
    int i = firstSelected();
    if (i < 0) g_ed.hitTest(g_view, g_mouseX, g_mouseY, &i); // nothing selected: the note under the mouse
    beginLyric(i);
  }
  else if (key == 'H' && !m) g_ghosts = !g_ghosts;
  else if (key == 'S' && !m) g_panel = !g_panel;
  else if (key == 'G' && !m) g_snapMode = (g_snapMode + 1) % 3, setupEditorSnap();
  else if (key == 'P' && !m) g_showContour = !g_showContour;
  else if (key == 'X' && !m) g_explain = !g_explain;
  else if (key == 'Q' && !m) quantiseSelected();
  else if (key == 'B' && !m) selectionIs(true);
  else if (key == 'N' && !m) g_suggest = !g_suggest;
  else if (key == 'A' && !m) acceptVisibleSuggestions();
  else if (key == 'V' && (m & MOD_CMD)) pasteLyrics();
  else if (key == 'F' && !m) fitItem();
  else if (key == 'Z' && !m) zoomToSelection();
  else if (key == ('[' | CHARKEY)) g_rp.brightness = std::max(-1.0, g_rp.brightness - 0.05), updateValues();
  else if (key == (']' | CHARKEY)) g_rp.brightness = std::min(1.0, g_rp.brightness + 0.05), updateValues();
  else if (key == ('-' | CHARKEY)) g_rp.contrast = std::max(0.1, g_rp.contrast / 1.1), updateValues();
  else if (key == ('=' | CHARKEY)) g_rp.contrast = std::min(3.0, g_rp.contrast * 1.1), updateValues();
  else if (key == (',' | CHARKEY)) adjustGamma(-1);
  else if (key == ('.' | CHARKEY)) adjustGamma(1);
  else return false;
  redraw();
  return true;
}

static bool isOurKey(int key, int flags)
{
  const bool cmd = (flags & FCONTROL) != 0;
  switch (key)
  {
  case VK_DELETE: case VK_BACK: case VK_ESCAPE: case VK_RETURN:
  case '[' | CHARKEY: case ']' | CHARKEY: case '-' | CHARKEY: case '=' | CHARKEY: case ',' | CHARKEY: case '.' | CHARKEY:
    return true;
  case 'A': return true;
  case 'V': return cmd;
  case 'S': case 'G': case 'F': case 'Z': case 'H': case 'P': case 'N': case 'X': case 'Q': case 'B': return !cmd && !(flags & (FALT | FSHIFT));
  }
  return false;
}

int Window_TranslateAccel(MSG *msg)
{
  if (!g_hwnd || (msg->hwnd != g_hwnd && !IsChild(g_hwnd, msg->hwnd))) return 0;
  if (g_edit && msg->hwnd != g_hwnd)
  {
    // typing a lyric: Tab/Space next, shift+Tab previous, Enter done, Esc cancel; the rest goes to the field
    if (msg->message == WM_KEYDOWN)
    {
      const int key = (int)msg->wParam, flags = (int)msg->lParam;
      if (key == VK_TAB) { endLyric(true, (flags & FSHIFT) ? -1 : 1); return 1; }
      if (key == VK_SPACE && (flags & FVIRTKEY)) { endLyric(true, 1); return 1; }
      if (key == '-' && !(flags & FVIRTKEY)) // hyphen: this syllable joins the next one
      {
        char buf[1024] = "";
        GetWindowText(g_edit, buf, sizeof(buf) - 1);
        strcat(buf, "-");
        SetWindowText(g_edit, buf);
        endLyric(true, 1);
        return 1;
      }
      if (key == VK_RETURN) { endLyric(true, 0); return 1; }
      if (key == VK_ESCAPE) { endLyric(false, 0); return 1; }
    }
    return -1;
  }
  // Our keys are handled here: delivered to the window, SWELL would turn Enter/Esc into dialog commands.
  if (msg->message == WM_KEYDOWN && isOurKey(keyCode((int)msg->wParam, (int)msg->lParam), (int)msg->lParam))
  {
    onKey(keyCode((int)msg->wParam, (int)msg->lParam));
    return 1;
  }
  if (msg->message == WM_KEYUP && isOurKey(keyCode((int)msg->wParam, (int)msg->lParam), (int)msg->lParam)) return 1;
  return -666; // everything else (space = play/stop, ...) uses REAPER's main shortcuts
}

static void finishPanelDrag()
{
  if (g_analysisDirty) requestAnalysis(false);
  g_panelWidget = -1;
}

static INT_PTR WINAPI dlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
  case WM_NCHITTEST:
    return HTCLIENT; // otherwise SWELL turns clicks into WM_NCLBUTTONDOWN etc.
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    paint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_SIZE:
    redraw();
    return 0;
  case WM_KEYDOWN:
    return onKey(keyCode((int)wParam, (int)lParam)) ? 0 : 1;
  case WM_LBUTTONDOWN:
  {
    const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
    syncViewOrigin();
    if (g_edit) endLyric(true, 0); // clicking elsewhere finishes the lyric
    SetFocus(hwnd);
    if (x < KB_W && y >= RULER_H && y < noteArea().b) { previewPitch(g_view.pitchAt(y)); return 0; }
    if (x < KB_W && y < RULER_H) { g_panel = !g_panel; redraw(); return 0; }
    if (panelShown() && panelArea().in(x, y)) g_drag = DRAG_PANEL, panelMouse(x, y, true);
    else if (rulerArea().in(x, y) && (mods() & MOD_SHIFT))
    {
      // shift+drag in the ruler: time selection, snapped like notes
      g_drag = DRAG_RULER_SELECT;
      g_selectAnchor = (mods() & MOD_ALT) ? g_view.t(x) : g_ed.snap(g_view.t(x));
      double a = g_selectAnchor, b = g_selectAnchor;
      GetSet_LoopTimeRange2(NULL, true, false, &a, &b, false);
    }
    else if (rulerArea().in(x, y)) g_drag = DRAG_RULER, SetEditCurPos(g_view.t(x), false, true);
    else if (noteArea().in(x, y))
    {
      int sug = -1;
      if (g_ed.hitTest(g_view, x, y, NULL) == NoteEditor::NONE && !(mods() & (MOD_SHIFT | MOD_CMD)) && (sug = suggestionAt(x, y)) >= 0)
      {
        acceptSuggestion(sug); // click a suggested note to make it a note
        return 0;
      }
      g_drag = DRAG_NOTES, g_ed.mouseDown(g_view, x, y, mods());
      int hit = -1;
      if (g_ed.hitTest(g_view, x, y, &hit) != NoteEditor::NONE) previewPitch(g_ed.notes[hit].pitch);
      g_lastDragPitch = hit >= 0 ? g_ed.notes[hit].pitch : -1;
    }
    else return 0;
    SetCapture(hwnd);
    redraw();
    return 0;
  }
  case WM_MOUSEMOVE:
  {
    const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
    g_mouseX = x, g_mouseY = y;
    if (g_drag == DRAG_NOTES) g_ed.mouseMove(g_view, x, y, mods()), previewDragged();
    else if (g_drag == DRAG_RULER) SetEditCurPos(g_view.t(x), false, true);
    else if (g_drag == DRAG_RULER_SELECT)
    {
      const double t = (mods() & MOD_ALT) ? g_view.t(x) : g_ed.snap(g_view.t(x));
      double a = std::min(t, g_selectAnchor), b = std::max(t, g_selectAnchor);
      GetSet_LoopTimeRange2(NULL, true, false, &a, &b, false);
    }
    else if (g_drag == DRAG_PANEL) panelMouse(x, y, false);
    redraw();
    return 0;
  }
  case WM_LBUTTONUP:
  {
    const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
    const Drag d = g_drag;
    g_drag = DRAG_NONE;
    ReleaseCapture();
    const int drawn = d == DRAG_NOTES ? g_ed.drawing() : -1;
    if (d == DRAG_NOTES && g_ed.mouseUp(g_view, x, y, mods()))
    {
      if (drawn >= 0 && drawn < (int)g_ed.notes.size() && g_pitchFromContour && !(mods() & MOD_ALT)) pitchFromContour(g_ed.notes[drawn]);
      commitNotes();
    }
    if (d == DRAG_PANEL) finishPanelDrag();
    redraw();
    return 0;
  }
  case WM_LBUTTONDBLCLK:
  {
    // double-click a note to type its lyric
    const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
    int hit = -1;
    if (noteArea().in(x, y) && g_ed.hitTest(g_view, x, y, &hit) != NoteEditor::NONE) beginLyric(hit);
    return 0;
  }
  case WM_RBUTTONDOWN:
  {
    const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
    if (noteArea().in(x, y) && !g_ed.active() && g_ed.deleteAt(g_view, x, y)) commitNotes();
    return 0;
  }
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
  {
    // trackpad friendly: scroll pans, cmd+scroll zooms time, alt+scroll zooms pitch, shift+scroll pans time
    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &pt);
    syncViewOrigin();
    const double steps = (short)HIWORD(wParam) / 120.0;
    const int m = mods();
    if (panelShown() && panelArea().in(pt.x, pt.y))
    {
      if (msg == WM_MOUSEWHEEL) scrollPanel((int)lround(-steps * 30));
      return 0;
    }
    if (msg == WM_MOUSEHWHEEL) g_view.t0 -= steps * 40 / g_view.pps;
    else if (m & MOD_CMD) zoomTime(pow(WHEEL_ZOOM, steps), pt.x);
    else if (m & MOD_ALT) zoomPitch(pow(WHEEL_ZOOM, steps), pt.y);
    else if (m & MOD_SHIFT) g_view.t0 -= steps * 40 / g_view.pps;
    else scrollPitch(steps * 2);
    redraw();
    return 0;
  }
  case WM_GESTURE:
  {
    // pinch zooms time (alt: pitch); SWELL reports the scale factor *1024 at GF_END
    GESTUREINFO *gi = (GESTUREINFO *)lParam;
    if (gi && gi->dwID == GID_ZOOM && (gi->dwFlags & GF_END) && gi->ullArguments)
    {
      POINT pt = {gi->ptsLocation.x, gi->ptsLocation.y};
      ScreenToClient(hwnd, &pt);
      syncViewOrigin();
      const double f = gi->ullArguments > 1024 ? PINCH_STEP : gi->ullArguments < 1024 ? 1 / PINCH_STEP : 1;
      if (mods() & MOD_ALT) zoomPitch(f, pt.y);
      else zoomTime(f, pt.x);
    }
    return 0;
  }
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    saveItemSettings();
    stopPreviews(false);
    cancelJob();
    clearCache();
    g_edit = NULL; // destroyed with the window
    if (g_memdc) SWELL_DeleteGfxContext(g_memdc);
    g_memdc = NULL, g_memw = g_memh = 0, g_hwnd = NULL, g_layerValid = false, g_drag = DRAG_NONE;
    return 0;
  }
  return 0;
}

// --- lifecycle ---------------------------------------------------------------------

void Window_Open(MediaItem *audio, MediaItem *midi)
{
  if (g_hwnd && audio == g_audio)
  {
    g_midi = midi;
    loadNotes();
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    return;
  }
  Window_Close();
  clearCache();
  g_audio = audio, g_midi = midi;
  g_audioPos = GetMediaItemInfo_Value(audio, "D_POSITION");
  g_audioLen = GetMediaItemInfo_Value(audio, "D_LENGTH");
  MediaItem_Take *take = GetActiveTake(audio);
  g_audioOffs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
  g_audioRate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
  g_spec = Spectrogram(), g_contour = Contour(), g_segments.clear(), g_onsets.clear(), g_snapTimes.clear();
  const bool hadSettings = loadItemSettings();
  updateValues();
  requestAnalysis(!hadSettings);
  setupEditorSnap();
  loadNotes();

  // 0x400001: SWELL's "no dialog resource, resizable top-level window"
  g_hwnd = CreateDialog(NULL, (const char *)(INT_PTR)0x400001, GetMainHwnd(), dlgProc);
  if (!g_hwnd) return;
  char title[512];
  snprintf(title, sizeof(title), "Reascribe - %s", take ? GetTakeName(take) : "");
  SetWindowText(g_hwnd, title);
  SetWindowPos(g_hwnd, NULL, 80, 80, 1300, 760, SWP_NOZORDER);
  fitItem();
  ShowWindow(g_hwnd, SW_SHOW);
}

void Window_Close()
{
  if (g_hwnd) DestroyWindow(g_hwnd);
}

void Window_Timer()
{
  stopPreviews(true);
  if (!g_hwnd) return;
  pollJob();
  if (g_drag == DRAG_NONE) saveItemSettings();
  if (!ValidatePtr2(NULL, g_audio, "MediaItem*")) { Window_Close(); return; }

  // the item may have been moved, trimmed, slipped or sped up: re-analyze
  MediaItem_Take *take = GetActiveTake(g_audio);
  const double pos = GetMediaItemInfo_Value(g_audio, "D_POSITION"), len = GetMediaItemInfo_Value(g_audio, "D_LENGTH");
  const double offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
  const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
  static double s_changedAt = -1; // re-analyze once the item has stopped changing for a moment
  if (pos != g_audioPos || len != g_audioLen || offs != g_audioOffs || rate != g_audioRate)
  {
    g_audioPos = pos, g_audioLen = len, g_audioOffs = offs, g_audioRate = rate;
    s_changedAt = time_precise();
    redraw();
  }
  else if (s_changedAt >= 0 && time_precise() - s_changedAt > 0.3)
  {
    s_changedAt = -1;
    requestAnalysis(false);
  }
  if (!g_ed.active() && takeHash(midiTake()) != g_hash) loadNotes(); // edited elsewhere, undo, ...

  const int state = GetPlayState(), change = GetProjectStateChangeCount(NULL);
  const double playPos = GetPlayPosition2(), cursor = GetCursorPosition();
  if (state != g_lastPlayState || ((state & 1) && playPos != g_lastPlayPos) || cursor != g_lastCursor ||
      change != g_lastProjChange)
    redraw();
  g_lastPlayState = state, g_lastPlayPos = playPos, g_lastCursor = cursor, g_lastProjChange = change;
}
