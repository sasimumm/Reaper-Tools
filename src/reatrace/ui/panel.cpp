// The settings panel on the right of the window (its widget table, drawing and mouse handling),
// the presets, and the playback speed and audio/notes balance the toolbar also shows.
#include "ui/panel.h"

#include "analysis.h"
#include "index.h"
#include "notes/commands.h"
#include "settings.h"
#include "ui/theme.h"

// CH_*; centre: mid minus side
static const char *const CHANNELS[] = {"Mix", "Left", "Right", "Side", "Centre"};
static const char *const COLORMAPS[] = {"Graphite", "Magma"}; // COLORMAP_*

static const double SPEED_RATES[] = {1, 0.75, 0.5}; // master playrates
static int g_speed;                                 // index into SPEED_RATES

// Balance: which of audio and notes is heard (track mutes), and the voice tracks' volume.
enum { LISTEN_BOTH, LISTEN_AUDIO, LISTEN_NOTES };
static int g_listen;
static double g_notesDb; // read back from the lead voice track

static int g_panelWidget = -1; // index into widgets() of the one being dragged, -1 if none

// Panel geometry, in points: widgets' left and right margin, and where the first one starts.
static const int PANEL_MARGIN = 12;
static const int PANEL_TOP = 8;

// --- settings panel --------------------------------------------------------------

enum WidgetType { W_HEADER, W_SLIDER, W_TOGGLE, W_BUTTON, W_CHOICE, W_BUTTONS };

// What buttons do: W_BUTTON's `button`, or W_BUTTONS' `button` plus the option's index.
enum {
    B_AUTO_ALL = 1,
    B_AUTO_VIEW,
    B_RESET,
    B_QUANTISE,
    B_SAVE_DEFAULT,
    B_LOAD_DEFAULT,
    B_DOCK,
    B_PRESET = 100
};
static const char *const PRESETS[] = {"Vocal in mix", "Solo vocal", "Fast runs"};
static const char *const DEFAULTS[] = {"Save as default", "Load default"};
static const char *const AUTO[] = {"Auto: whole item", "Auto: visible"};

// A row of the panel, bound to the setting it shows and changes.
struct Widget {
    WidgetType type;
    const char *label;
    double *value = nullptr; // sliders
    bool *flag = nullptr;    // toggles
    double lo = 0, hi = 0;   // slider range, in `value`'s units
    bool logScale = false;
    bool analysis = false;                // changing it recomputes the analysis
    double display = 0;                   // shown value = value * display
    const char *fmt = nullptr;            // printf format of the shown value
    int button = 0;                       // B_*
    int *choice = nullptr;                // choices: a row of buttons, one per option
    std::span<const char *const> options; // choice and button row labels
    int optionCount = 0;
};

// Sections fold at their header; what changes while transcribing is in the toolbar.
// The panel bound to the settings s (g_settings, or defaults to read a slider's default from).
static std::vector<Widget> allWidgets(Settings &s = g_settings) {
    return {
        {.type = W_HEADER, .label = "Display"},
        {.type = W_BUTTONS, .label = "", .button = B_AUTO_ALL, .options = AUTO, .optionCount = 2},
        {.type = W_SLIDER,
         .label = "Floor",
         .value = &s.rp.dbFloor,
         .lo = -120,
         .hi = -20,
         .display = 1,
         .fmt = "%.0f dB"},
        {.type = W_SLIDER,
         .label = "Contrast",
         .value = &s.rp.contrast,
         .lo = 0.1,
         .hi = 3,
         .logScale = true,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_SLIDER,
         .label = "Brightness",
         .value = &s.rp.brightness,
         .lo = -1,
         .hi = 1,
         .display = 1,
         .fmt = "%+.2f"},
        {.type = W_SLIDER,
         .label = "Colour curve",
         .value = &s.rp.bias,
         .lo = -2,
         .hi = 2,
         .display = 1,
         .fmt = "%+.2f"},
        {.type = W_CHOICE,
         .label = "Colours",
         .choice = &s.rp.colormap,
         .options = COLORMAPS,
         .optionCount = 2},
        {.type = W_TOGGLE, .label = "Peaks only", .flag = &s.rp.peaksOnly},
        {.type = W_SLIDER,
         .label = "Peak dimming",
         .value = &s.rp.peakDim,
         .hi = 1,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_TOGGLE, .label = "Noise subtraction", .flag = &s.rp.subtractNoise},
        {.type = W_SLIDER,
         .label = "Noise strength",
         .value = &s.rp.noiseAlpha,
         .hi = 3,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_SLIDER,
         .label = "Noise from quietest",
         .value = &s.rp.noiseFraction,
         .lo = 0.01,
         .hi = 0.5,
         .display = 100,
         .fmt = "%.0f%%"},
        {.type = W_HEADER, .label = "Suggestions and guides"},
        {.type = W_TOGGLE,
         .label = "Front-end vote (slower)",
         .flag = &s.estimateVote,
         .analysis = true},
        {.type = W_TOGGLE, .label = "Drawn notes take the sung pitch", .flag = &s.pitchFromContour},
        {.type = W_SLIDER,
         .label = "Overtones",
         .value = &s.harmonics,
         .lo = 2,
         .hi = 16,
         .display = 1,
         .fmt = "%.0f"},
        {.type = W_SLIDER,
         .label = "Guide opacity",
         .value = &s.guideOpacity,
         .hi = 1,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_SLIDER,
         .label = "Quantise strength",
         .value = &s.quantStrength,
         .hi = 1,
         .display = 100,
         .fmt = "%.0f%%"},
        {.type = W_BUTTON, .label = "Quantise selected notes (Q)", .button = B_QUANTISE},
        {.type = W_HEADER, .label = "Analysis"},
        {.type = W_BUTTONS,
         .label = "Presets",
         .button = B_PRESET,
         .options = PRESETS,
         .optionCount = 3},
        {.type = W_CHOICE,
         .label = "Channel",
         .analysis = true,
         .choice = &s.channel,
         .options = CHANNELS,
         .optionCount = 5},
        {.type = W_SLIDER,
         .label = "Resolution (gamma)",
         .value = &s.np.gamma,
         .lo = 3,
         .hi = 40,
         .logScale = true,
         .analysis = true,
         .display = 1,
         .fmt = "%.1f Hz"},
        {.type = W_SLIDER,
         .label = "Lowest frequency",
         .value = &s.np.fmin,
         .lo = 20,
         .hi = 500,
         .logScale = true,
         .analysis = true,
         .display = 1,
         .fmt = "%.0f Hz"},
        {.type = W_SLIDER,
         .label = "Highest frequency",
         .value = &s.np.fmax,
         .lo = 1000,
         .hi = 11000,
         .logScale = true,
         .analysis = true,
         .display = 1,
         .fmt = "%.0f Hz"},
        {.type = W_SLIDER,
         .label = "Bins per semitone",
         .value = &s.binsPerSemitone,
         .lo = 1,
         .hi = 10,
         .analysis = true,
         .display = 1,
         .fmt = "%.0f"},
        {.type = W_BUTTON, .label = "Reset to defaults", .button = B_RESET},
        {.type = W_HEADER, .label = "Playback"},
        {.type = W_TOGGLE, .label = "Play notes when clicked", .flag = &s.audition},
        {.type = W_SLIDER,
         .label = "Notes volume",
         .value = &g_notesDb,
         .lo = -30,
         .hi = 6,
         .display = 1,
         .fmt = "%+.1f dB"},
        {.type = W_TOGGLE, .label = "Isolate: all shown voices (shift+I)", .flag = &s.isolateAll},
        {.type = W_SLIDER,
         .label = "Isolate: band width",
         .value = &s.isolateWidth,
         .lo = 20,
         .hi = 400,
         .logScale = true,
         .display = 1,
         .fmt = "%.0f cents"},
        {.type = W_SLIDER,
         .label = "Isolate: the rest",
         .value = &s.isolateBackground,
         .lo = -60,
         .hi = 0,
         .display = 1,
         .fmt = "%.0f dB"},
        {.type = W_HEADER, .label = "Window"},
        {.type = W_TOGGLE, .label = "Show other voices", .flag = &g_ui.showOthers},
        {.type = W_TOGGLE, .label = "Colours from REAPER theme", .flag = &s.followTheme},
        {.type = W_TOGGLE, .label = "Link time to arrange view (L)", .flag = &s.linkArrange},
        {.type = W_BUTTON, .label = "Dock / undock window (D)", .button = B_DOCK},
        {.type = W_BUTTONS,
         .label = "Settings for new items",
         .button = B_SAVE_DEFAULT,
         .options = DEFAULTS,
         .optionCount = 2},
    };
}

// Folded sections, by header label; Analysis and Window start folded.
static std::vector<std::string> g_folded = {"Analysis", "Window"};

static bool folded(const char *header) {
    return std::find(g_folded.begin(), g_folded.end(), header) != g_folded.end();
}

// The widgets shown: every header, and what's under the unfolded ones.
static std::vector<Widget> widgets() {
    std::vector<Widget> out;
    bool hide = false;
    for (const Widget &w : allWidgets()) {
        if (w.type == W_HEADER) {
            hide = folded(w.label);
        }
        if (w.type == W_HEADER || !hide) {
            out.push_back(w);
        }
    }
    return out;
}

// In points.
static int widgetHeight(WidgetType t) {
    switch (t) {
    case W_HEADER: return 30;
    case W_SLIDER: return 32;
    case W_TOGGLE: return 20;
    case W_CHOICE:
    case W_BUTTONS: return 42;
    case W_BUTTON: return 24;
    }
    return 24;
}

// Whether p points into obj.
template <class T> static bool pointsInto(const void *p, const T &obj) {
    const auto *b = reinterpret_cast<const std::byte *>(&obj);
    // the end of obj's bytes, to compare p with
#pragma clang unsafe_buffer_usage begin
    const std::byte *e = b + sizeof(T);
#pragma clang unsafe_buffer_usage end
    return !std::less<const void *>{}(p, b) && std::less<const void *>{}(p, e);
}

// Spectrogram settings need the display values recomputed; guide settings only a redraw.
static bool isRenderParam(const void *p) { return pointsInto(p, g_settings.rp); }

// Where value v is along the slider, 0..1 inside its range.
static double sliderFrac(const Widget &w, double v) {
    return w.logScale ? log(v / w.lo) / log(w.hi / w.lo) : (v - w.lo) / (w.hi - w.lo);
}

// The value at fraction f along the slider (clamped to 0..1).
static double sliderValue(const Widget &w, double f) {
    f = std::max(0.0, std::min(1.0, f));
    return w.logScale ? w.lo * pow(w.hi / w.lo, f) : w.lo + (w.hi - w.lo) * f;
}

static int g_panelScroll; // points the panel is scrolled down

// The height of the shown widgets with the space around them, in points.
static int panelContentHeight() {
    int h = 16;
    for (const Widget &w : widgets()) {
        h += widgetHeight(w.type);
    }
    return h + 12;
}

void scrollPanel(int dy) {
    const Rect p = panelArea();
    const int maxScroll = panelContentHeight() - (p.b - p.t);
    g_panelScroll = std::max(0, std::min(g_panelScroll + dy, maxScroll));
    redraw();
}

// The index into widgets() of the one at client y, -1 if none.
static int widgetAt(int y) {
    const std::vector<Widget> ws = widgets();
    int top = panelArea().t + PANEL_TOP - g_panelScroll;
    for (size_t i = 0; i < ws.size(); i++) {
        const int h = widgetHeight(ws[i].type);
        if (y >= top && y < top + h) {
            return static_cast<int>(i);
        }
        top += h;
    }
    return -1;
}

// The display settings fitted to the frames in view.
static void autoCalibrateView() {
    const Rect n = noteArea();
    const double frameRate = g_analysis.spec->frameRate();
    const int f0 = static_cast<int>((g_ui.view.t(n.l) - g_session.audioPos) * frameRate);
    const int f1 = static_cast<int>((g_ui.view.t(n.r) - g_session.audioPos) * frameRate) + 1;
    autoCalibrate(*g_analysis.spec, g_settings.rp, f0, f1);
}

static void applyNotesVolume();

// Updates what depends on w's setting after it changed.
static void changedSetting(const Widget &w) {
    if (w.analysis) { // recomputed on mouse up
        g_ui.analysisDirty = true;
        redraw();
        return;
    }
    if (w.value == &g_settings.rp.noiseFraction) {
        estimateNoise(*g_analysis.spec, g_settings.rp.noiseFraction);
    }
    if (w.value == &g_notesDb) {
        applyNotesVolume();
    }
    if (w.value == &g_settings.rp.bias) {
        updateColormap();
    } else if (isRenderParam(w.value)) {
        updateValues();
    } else {
        redraw();
    }
}

// PRESETS[i]; "Fast runs" changes only the resolution, on top of the current settings.
static void applyPreset(int i) {
    if (i == 0) {
        g_settings.channel = CH_CENTRE;
        g_settings.rp.subtractNoise = true;
        g_settings.np.fmin = 60;
        g_settings.np.fmax = 5000;
        g_settings.np.gamma = 11;
    } else if (i == 1) {
        g_settings.channel = CH_MIX;
        g_settings.rp.subtractNoise = false;
        g_settings.np.fmin = 60;
        g_settings.np.fmax = 6000;
        g_settings.np.gamma = 11;
    } else if (i == 2) {
        g_settings.np.gamma = 25; // wider bands, shorter windows: sharper timing for fast runs
    }
    requestAnalysis(true);
}

// Does what button b (B_*) does.
static void pressButton(int b) {
    if (b >= B_PRESET) {
        applyPreset(b - B_PRESET);
        return;
    }
    if (b == B_DOCK) {
        g_ui.dockPending = true;
        return;
    }
    if (b == B_SAVE_DEFAULT) {
        SetExtState(EXT_SECTION, EXT_DEFAULTS, saveSettings().c_str(), true);
        return;
    }
    if (b == B_LOAD_DEFAULT) {
        resetSettings();
        loadSettings(globalState(EXT_DEFAULTS));
        setupEditorSnap();
        requestAnalysis(true);
        return;
    }
    if (b == B_QUANTISE) {
        quantiseSelected();
        return;
    }
    if (b == B_AUTO_ALL) {
        autoCalibrate(*g_analysis.spec, g_settings.rp);
    } else if (b == B_AUTO_VIEW) {
        autoCalibrateView();
    } else if (b == B_RESET) {
        resetSettings();
        setupEditorSnap();
        requestAnalysis(true);
    }
    updateValues();
}

// Master playrate; slowed down with pitch preserved (REAPER's "preserve pitch in audio items when
// changing master playrate" toggle is switched on if it is off).
static const int CMD_TOGGLE_PRESERVE_PITCH = 40671;

static void applySpeed() {
    const double rate = std::span(SPEED_RATES)[idx(g_speed)];
    if (rate != 1 && GetToggleCommandState(CMD_TOGGLE_PRESERVE_PITCH) == 0) {
        Main_OnCommand(CMD_TOGGLE_PRESERVE_PITCH, 0);
    }
    CSurf_OnPlayRateChange(rate);
}

// The voice tracks that exist for the audio item; looked up again after each project change (read
// every frame).
static std::vector<MediaTrack *> voiceTracks() {
    static std::vector<MediaTrack *> out;
    static int change = -1;
    static MediaItem *audio = NULL;
    if (change == GetProjectStateChangeCount(NULL) && audio == g_session.audio) {
        return out;
    }
    change = GetProjectStateChangeCount(NULL);
    audio = g_session.audio;
    out.clear();
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if (MediaItem *it = VoiceItem(g_session.audio, v, false)) {
            out.push_back(GetMediaItem_Track(it));
        }
    }
    return out;
}

// Mutes the audio or the voice tracks for g_listen.
static void applyListen() {
    if (MediaTrack *a = GetMediaItem_Track(g_session.audio)) {
        SetMediaTrackInfo_Value(a, "B_MUTE", g_listen == LISTEN_NOTES);
    }
    for (MediaTrack *t : voiceTracks()) {
        SetMediaTrackInfo_Value(t, "B_MUTE", g_listen == LISTEN_AUDIO);
    }
    TrackList_AdjustWindows(false);
}

// A/B: swap between hearing only the audio and only the notes.
void swapListen() {
    g_listen = g_listen == LISTEN_AUDIO ? LISTEN_NOTES : LISTEN_AUDIO;
    applyListen();
}

static void applyNotesVolume() {
    for (MediaTrack *t : voiceTracks()) {
        SetMediaTrackInfo_Value(t, "D_VOL", pow(10.0, g_notesDb / 20));
    }
    TrackList_AdjustWindows(false);
}

int listenMode() { return g_listen; }

void setListen(int mode) {
    g_listen = std::clamp(mode, static_cast<int>(LISTEN_BOTH), static_cast<int>(LISTEN_NOTES));
    applyListen();
}

int speedIndex() { return g_speed; }

void setSpeed(int index) {
    g_speed = std::clamp(index, 0, static_cast<int>(std::size(SPEED_RATES)) - 1);
    applySpeed();
}

// Mutes and volume can also change in REAPER: show what the tracks say.
static void readBalance() {
    MediaTrack *a = GetMediaItem_Track(g_session.audio);
    const std::vector<MediaTrack *> vt = voiceTracks();
    const bool audioMuted = a && GetMediaTrackInfo_Value(a, "B_MUTE") != 0;
    const bool notesMuted = !vt.empty() && GetMediaTrackInfo_Value(vt[0], "B_MUTE") != 0;
    if (audioMuted && !notesMuted) {
        g_listen = LISTEN_NOTES;
    } else if (notesMuted && !audioMuted) {
        g_listen = LISTEN_AUDIO;
    } else {
        g_listen = LISTEN_BOTH;
    }
    // not while a panel slider is being dragged: the drag sets the volume itself
    if (!vt.empty() && g_panelWidget < 0) {
        g_notesDb = 20 * log10(std::max(1e-6, GetMediaTrackInfo_Value(vt[0], "D_VOL")));
    }
}

// A slider's value in default settings: the same slider bound to a fresh Settings; notes volume
// (not a setting) is 0 dB.
static double sliderDefault(const Widget &w) {
    if (!pointsInto(w.value, g_settings)) {
        return 0;
    }
    static Settings d;
    const std::vector<Widget> live = allWidgets();
    const std::vector<Widget> dflt = allWidgets(d);
    for (size_t i = 0; i < live.size(); i++) {
        if (live[i].value == w.value) {
            return *dflt[i].value;
        }
    }
    return 0;
}

void panelDoubleClick(int y) {
    const int i = widgetAt(y);
    if (i < 0) {
        return;
    }
    const std::vector<Widget> ws = widgets();
    const Widget &w = ws[idx(i)];
    if (w.type == W_SLIDER && *w.value != sliderDefault(w)) {
        g_panelWidget = i;
        *w.value = sliderDefault(w);
        changedSetting(w);
    }
}

// The option of a choice or button row at client x.
static int optionAt(const Widget &w, int x, const Rect &p) {
    const int option =
        (x - p.l - PANEL_MARGIN) * w.optionCount / std::max(1, p.r - p.l - 2 * PANEL_MARGIN);
    return std::max(0, std::min(w.optionCount - 1, option));
}

void panelMouse(int x, int y, bool down) {
    const Rect p = panelArea();
    const std::vector<Widget> ws = widgets();
    if (down) {
        g_panelWidget = widgetAt(y);
    }
    if (g_panelWidget < 0) {
        return;
    }
    const Widget &w = ws[idx(g_panelWidget)];
    if (down && w.type == W_HEADER) { // fold or unfold the section
        const auto it = std::find(g_folded.begin(), g_folded.end(), w.label);
        if (it != g_folded.end()) {
            g_folded.erase(it);
        } else {
            g_folded.push_back(w.label);
        }
        g_panelWidget = -1;
        scrollPanel(0); // stay within the shorter content
        redraw();
        return;
    }

    if (w.type == W_SLIDER) {
        const double f =
            static_cast<double>(x - p.l - PANEL_MARGIN) / (p.r - p.l - 2 * PANEL_MARGIN);
        const double v = sliderValue(w, f);
        if (v != *w.value) {
            *w.value = v;
            changedSetting(w);
        }
    } else if (down && w.type == W_TOGGLE) {
        *w.flag = !*w.flag;
        if (w.analysis) {
            requestAnalysis(false);
        } else if (isRenderParam(w.flag)) {
            updateValues();
        } else {
            redraw();
        }
    } else if (down && w.type == W_BUTTON) {
        pressButton(w.button);
    } else if (down && w.type == W_BUTTONS) {
        pressButton(w.button + optionAt(w, x, p));
    } else if (down && w.type == W_CHOICE) {
        const int i = optionAt(w, x, p);
        if (i == *w.choice) {
            return;
        }
        *w.choice = i;
        if (w.choice == &g_settings.rp.colormap) {
            updateColormap();
        } else if (isRenderParam(w.choice)) {
            updateValues();
        } else if (w.analysis) {
            requestAnalysis(false);
        }
    }
}

void drawPanel(Canvas &cv, HDC dc) {
    const Rect p = panelArea();
    if (p.r <= p.l) {
        return;
    }
    cv.clip = p; // scrolled widgets stay out of the toolbar and the status line
    cv.fill(p.l, p.t, p.r, p.b, g_theme.panel);
    cv.fill(p.l, p.t, p.l + 1, p.b, g_theme.separator);

    const std::vector<Widget> ws = widgets();
    int top = p.t + PANEL_TOP - g_panelScroll;
    char buf[64];
    const int l = p.l + PANEL_MARGIN, r = p.r - PANEL_MARGIN;
    for (const Widget &w : ws) {
        const int h = widgetHeight(w.type);
        if (w.type == W_HEADER) { // a line above (unless at the top), the label, + when folded
            if (top > p.t + 4) {
                cv.fill(p.l + 1, top + 2, p.r, top + 3, g_theme.track);
            }
            text(dc, w.label, l, top + 6, r, top + h, g_theme.text, DT_LEFT);
            text(dc, folded(w.label) ? "+" : "–", l, top + 6, r, top + h, g_theme.dim, DT_RIGHT);
        } else if (w.type == W_SLIDER) {
            text(dc, w.label, l, top, r, top + 16, g_theme.text, DT_LEFT);
            // The format comes from the widget table, so it is not a literal.
#pragma clang unsafe_buffer_usage begin
            snprintf(buf, sizeof(buf), w.fmt, *w.value * w.display);
#pragma clang unsafe_buffer_usage end
            text(dc, buf, l, top, r, top + 16, g_theme.dim, DT_RIGHT);
            // the bar, filled up to the knob at bx
            const int by = top + 20;
            const double frac = std::max(0.0, std::min(1.0, sliderFrac(w, *w.value)));
            const int bx = l + static_cast<int>(lround((r - l) * frac));
            cv.fill(l, by + 3, r, by + 7, g_theme.track);
            cv.fill(l, by + 3, bx, by + 7,
                    w.analysis && g_ui.analysisDirty ? g_theme.dirty : g_theme.accent);
            cv.fill(bx - 3, by, bx + 3, by + 10, g_theme.text);
        } else if (w.type == W_TOGGLE) {
            cv.frame(l, top + 5, l + 12, top + 17, g_theme.text);
            if (*w.flag) {
                cv.fill(l + 3, top + 8, l + 9, top + 14, g_theme.accent);
            }
            text(dc, w.label, l + 18, top, r, top + 22, g_theme.text, DT_LEFT);
        } else if (w.type == W_CHOICE || w.type == W_BUTTONS) {
            text(dc, w.label, l, top, r, top + 16, g_theme.text, DT_LEFT);
            for (int i = 0; i < w.optionCount; i++) {
                const int bl = l + (r - l) * i / w.optionCount;
                const int br = l + (r - l) * (i + 1) / w.optionCount;
                const bool on = w.choice && i == *w.choice;
                cv.fill(bl, top + 18, br - 2, top + 40, on ? g_theme.accent : g_theme.button);
                text(dc, w.options[idx(i)], bl, top + 18, br - 2, top + 40,
                     on ? g_theme.onText : g_theme.text, DT_CENTER);
            }
        } else { // W_BUTTON
            cv.fill(l, top + 2, r, top + h - 2, g_theme.button);
            text(dc, w.label, l, top + 2, r, top + h - 2, g_theme.text, DT_CENTER);
        }
        top += h;
    }
}

// The speed and what is heard, as REAPER has them (both can change elsewhere).
void syncPlayback() {
    const double rate = Master_GetPlayRate(NULL);
    const std::span<const double> rates = SPEED_RATES;
    for (int i = 0; i < static_cast<int>(rates.size()); i++) { // the nearest one
        if (fabs(rates[idx(i)] - rate) < fabs(rates[idx(g_speed)] - rate)) {
            g_speed = i;
        }
    }
    readBalance();
}

void finishPanelDrag() {
    if (g_ui.analysisDirty) {
        requestAnalysis(false);
    }
    g_panelWidget = -1;
}
