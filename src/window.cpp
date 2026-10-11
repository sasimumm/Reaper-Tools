#include "window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "reaper_plugin_functions.h"
#undef min
#undef max

#include "editor.h"
#include "nsgt.h"

// Layout, in pixels.
static const int KB_W = 56, RULER_H = 30, STATUS_H = 20, PANEL_W = 250;
static const double PINCH_SENSITIVITY = 0.3; // exponent applied to each pinch event's zoom factor

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
static bool g_snapGrid;            // off by default: recordings are usually free tempo
static bool g_panel = true;

enum Drag { DRAG_NONE, DRAG_NOTES, DRAG_RULER, DRAG_PANEL };
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
  int w, h, version;
  bool operator==(const LayerKey &o) const
  {
    return t0 == o.t0 && pps == o.pps && pitchTop == o.pitchTop && rowH == o.rowH && w == o.w && h == o.h &&
           version == o.version;
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

static void analyze()
{
  MediaItem_Take *take = GetActiveTake(g_audio);
  if (!take) return;
  AudioAccessor *acc = CreateTakeAudioAccessor(take);
  const int nch = std::max(1, std::min(2, GetMediaSourceNumChannels(GetMediaItemTake_Source(take))));
  std::vector<double> buf;
  const double sr = g_np.sampleRate;
  g_spec = computeNSGT(g_np, (long long)(g_audioLen * sr), [&](long long start, int n, float *out) {
    buf.assign((size_t)n * nch, 0); // the accessor leaves the buffer untouched where there's no audio
    GetAudioAccessorSamples(acc, (int)sr, nch, start / sr, n, buf.data()); // time relative to item start
    for (int i = 0; i < n; i++)
    {
      double s = 0;
      for (int c = 0; c < nch; c++) s += buf[(size_t)i * nch + c];
      out[i] = (float)(s / nch);
    }
  });
  DestroyAudioAccessor(acc);
  estimateNoise(g_spec, g_rp.noiseFraction);
  g_analysisDirty = false;
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
  std::vector<unsigned char> col(s.bins);
  for (int x = 0; x < w; x++)
  {
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
  if (take) MIDI_GetHash(take, true, h, sizeof(h));
  return h;
}

static void loadNotes()
{
  g_ed.cancel();
  g_loaded.clear();
  MediaItem_Take *take = midiTake();
  int cnt = 0;
  if (take) MIDI_CountEvts(take, &cnt, NULL, NULL);
  for (int i = 0; i < cnt; i++)
  {
    bool sel = false;
    double s = 0, e = 0;
    int pitch = 0;
    MIDI_GetNote(take, i, &sel, NULL, &s, &e, NULL, &pitch, NULL);
    g_loaded.push_back({MIDI_GetProjTimeFromPPQPos(take, s), MIDI_GetProjTimeFromPPQPos(take, e), pitch, sel, i});
  }
  g_ed.notes = g_loaded;
  g_ed.deleted.clear();
  g_hash = takeHash(take);
  redraw();
}

// Writes the editor's notes back to the take as one undo point (none for selection-only changes).
static void commitNotes()
{
  bool dataChanged = !g_ed.deleted.empty();
  for (const Note &n : g_ed.notes)
  {
    if (n.idx < 0) dataChanged = true;
    else if (n.idx < (int)g_loaded.size())
    {
      const Note &o = g_loaded[n.idx];
      dataChanged |= n.start != o.start || n.end != o.end || n.pitch != o.pitch;
    }
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

enum WidgetType { W_HEADER, W_SLIDER, W_TOGGLE, W_BUTTON };
enum { B_AUTO_ALL = 1, B_AUTO_VIEW, B_RESET };
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
};

static std::vector<Widget> widgets()
{
  return {
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
      {W_BUTTON, "Reset to defaults", NULL, NULL, 0, 0, false, false, 0, NULL, B_RESET},
  };
}

static int widgetHeight(WidgetType t) { return t == W_HEADER ? 24 : t == W_SLIDER ? 36 : t == W_TOGGLE ? 22 : 26; }

static double sliderFrac(const Widget &w, double v)
{
  return w.logScale ? log(v / w.lo) / log(w.hi / w.lo) : (v - w.lo) / (w.hi - w.lo);
}
static double sliderValue(const Widget &w, double f)
{
  f = std::max(0.0, std::min(1.0, f));
  return w.logScale ? w.lo * pow(w.hi / w.lo, f) : w.lo + (w.hi - w.lo) * f;
}

static int widgetAt(int y)
{
  std::vector<Widget> ws = widgets();
  int top = 8;
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
  updateValues();
}

static void pressButton(int b)
{
  if (b == B_AUTO_ALL) autoCalibrate(g_spec, g_rp);
  else if (b == B_AUTO_VIEW) autoCalibrateView();
  else if (b == B_RESET)
  {
    g_np = NSGTParams(), g_rp = RenderParams();
    analyze();
    autoCalibrate(g_spec, g_rp);
  }
  updateValues();
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
  else if (down && w.type == W_TOGGLE) *w.flag = !*w.flag, updateValues();
  else if (down && w.type == W_BUTTON) pressButton(w.button);
}

static void drawPanel(Canvas &cv, HDC dc)
{
  const Rect p = panelArea();
  if (p.r <= p.l) return;
  cv.fill(p.l, p.t, p.r, p.b, 0x222222);
  cv.fill(p.l, p.t, p.l + 1, p.b, 0x000000);
  std::vector<Widget> ws = widgets();
  int top = 8;
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
    else
    {
      cv.fill(l, top + 2, r, top + h - 2, 0x333333);
      text(dc, w.label, l, top + 2, r, top + h - 2, C_TEXT, DT_CENTER);
    }
    top += h;
  }
  text(dc, "Keys: S panel, G snap, F fit, Z zoom to", l, p.b - 58, r, p.b - 42, C_DIM, DT_LEFT);
  text(dc, "selection, [ ] brightness, - = contrast,", l, p.b - 42, r, p.b - 26, C_DIM, DT_LEFT);
  text(dc, ", . resolution", l, p.b - 26, r, p.b - 10, C_DIM, DT_LEFT);
}

// --- paint -------------------------------------------------------------------------

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
  LayerKey key = {g_view.t0, g_view.pps, g_view.pitchTop, g_view.rowH, n.r - n.l, n.b - n.t, g_valVersion};
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
  cv.clip = {0, 0, KB_W, RULER_H};
  cv.fill(0, 0, KB_W, RULER_H, 0x303030);
  text(dc, g_panel ? "Hide (S)" : "Settings", 0, 0, KB_W, RULER_H, C_TEXT, DT_CENTER);

  // cursors
  cv.clip = {n.l, 0, n.r, n.b};
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
  snprintf(buf, sizeof(buf), "snap: %s (G)", g_snapGrid ? "grid" : "off");
  text(dc, status.c_str(), 6, n.b, c.r - 120, c.b, C_TEXT, DT_LEFT);
  text(dc, buf, 6, n.b, c.r - 6, c.b, C_DIM, DT_RIGHT);

  cv.clip = {0, 0, w, c.b};
  if (panelShown()) drawPanel(cv, dc);

  BitBlt(hdc, 0, 0, c.r, c.b, g_memdc, 0, 0, SRCCOPY);
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

// --- input -------------------------------------------------------------------------

static int mods()
{
  return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) | ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MOD_CMD : 0) |
         ((GetAsyncKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0);
}

static void setupEditorSnap()
{
  g_ed.snap = g_snapGrid ? snapToGrid : [](double t) { return t; };
}

static void adjustGamma(double delta)
{
  g_np.gamma = std::max(3.0, std::min(40.0, g_np.gamma + delta));
  analyze();
  updateValues();
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
  else if (key == 'S' && !m) g_panel = !g_panel;
  else if (key == 'G' && !m) g_snapGrid = !g_snapGrid, setupEditorSnap();
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
  case VK_DELETE: case VK_BACK: case VK_ESCAPE:
  case '[' | CHARKEY: case ']' | CHARKEY: case '-' | CHARKEY: case '=' | CHARKEY: case ',' | CHARKEY: case '.' | CHARKEY:
    return true;
  case 'A': return cmd;
  case 'S': case 'G': case 'F': case 'Z': return !cmd && !(flags & (FALT | FSHIFT));
  }
  return false;
}

int Window_TranslateAccel(MSG *msg)
{
  if (!g_hwnd || (msg->hwnd != g_hwnd && !IsChild(g_hwnd, msg->hwnd))) return 0;
  if (msg->message == WM_KEYDOWN && isOurKey(keyCode((int)msg->wParam, (int)msg->lParam), (int)msg->lParam)) return -1;
  return 0; // everything else (e.g. space for play/stop) goes to REAPER
}

static void finishPanelDrag()
{
  if (g_analysisDirty)
  {
    analyze();
    updateValues();
  }
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
    SetFocus(hwnd);
    if (x < KB_W && y < RULER_H) { g_panel = !g_panel; redraw(); return 0; }
    if (panelShown() && panelArea().in(x, y)) g_drag = DRAG_PANEL, panelMouse(x, y, true);
    else if (rulerArea().in(x, y)) g_drag = DRAG_RULER, SetEditCurPos(g_view.t(x), false, true);
    else if (noteArea().in(x, y)) g_drag = DRAG_NOTES, g_ed.mouseDown(g_view, x, y, mods());
    else return 0;
    SetCapture(hwnd);
    redraw();
    return 0;
  }
  case WM_MOUSEMOVE:
  {
    const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
    g_mouseX = x, g_mouseY = y;
    if (g_drag == DRAG_NOTES) g_ed.mouseMove(g_view, x, y, mods());
    else if (g_drag == DRAG_RULER) SetEditCurPos(g_view.t(x), false, true);
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
    if (d == DRAG_NOTES && g_ed.mouseUp(g_view, x, y, mods())) commitNotes();
    if (d == DRAG_PANEL) finishPanelDrag();
    redraw();
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
    if (msg == WM_MOUSEHWHEEL) g_view.t0 -= steps * 40 / g_view.pps;
    else if (m & MOD_CMD) zoomTime(pow(1.15, steps), pt.x);
    else if (m & MOD_ALT) zoomPitch(pow(1.15, steps), pt.y);
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
      const double f = pow(gi->ullArguments / 1024.0, PINCH_SENSITIVITY);
      if (mods() & MOD_ALT) zoomPitch(f, pt.y);
      else zoomTime(f, pt.x);
    }
    return 0;
  }
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
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
  g_audio = audio, g_midi = midi;
  g_audioPos = GetMediaItemInfo_Value(audio, "D_POSITION");
  g_audioLen = GetMediaItemInfo_Value(audio, "D_LENGTH");
  MediaItem_Take *take = GetActiveTake(audio);
  g_audioOffs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
  g_audioRate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
  analyze();
  autoCalibrate(g_spec, g_rp);
  updateValues();
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
  if (!g_hwnd) return;
  if (!ValidatePtr2(NULL, g_audio, "MediaItem*")) { Window_Close(); return; }

  // the item may have been moved, trimmed, slipped or sped up: re-analyze
  MediaItem_Take *take = GetActiveTake(g_audio);
  const double pos = GetMediaItemInfo_Value(g_audio, "D_POSITION"), len = GetMediaItemInfo_Value(g_audio, "D_LENGTH");
  const double offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0;
  const double rate = take ? GetMediaItemTakeInfo_Value(take, "D_PLAYRATE") : 1;
  if (pos != g_audioPos || len != g_audioLen || offs != g_audioOffs || rate != g_audioRate)
  {
    g_audioPos = pos, g_audioLen = len, g_audioOffs = offs, g_audioRate = rate;
    analyze();
    updateValues();
  }
  if (!g_ed.active() && takeHash(midiTake()) != g_hash) loadNotes(); // edited elsewhere, undo, ...

  const int state = GetPlayState(), change = GetProjectStateChangeCount(NULL);
  const double playPos = GetPlayPosition2(), cursor = GetCursorPosition();
  if (state != g_lastPlayState || ((state & 1) && playPos != g_lastPlayPos) || cursor != g_lastCursor ||
      change != g_lastProjChange)
    redraw();
  g_lastPlayState = state, g_lastPlayPos = playPos, g_lastCursor = cursor, g_lastProjChange = change;
}
