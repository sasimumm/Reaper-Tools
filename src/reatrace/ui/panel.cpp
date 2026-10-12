#include "ui/panel.h"

#include "analysis.h"
#include "notes/commands.h"
#include "settings.h"

static const char *const CHANNELS[] = {
    "Mix", "Left", "Right", "Side", "Centre"
}; // centre: mid minus side

static const char *const COLORMAPS[] = {"Graphite", "Magma"}; // COLORMAP_*
static const double SPEED_RATES[] = {1, 0.75, 0.5};
static int g_speed;
// Balance: which of audio and notes is heard (track mutes), and the voice tracks' volume.
enum { LISTEN_BOTH, LISTEN_AUDIO, LISTEN_NOTES };
static int g_listen;
static double g_notesDb; // read back from the lead voice track
static int g_panelWidget = -1;

// --- settings panel --------------------------------------------------------------

enum WidgetType { W_HEADER, W_SLIDER, W_TOGGLE, W_BUTTON, W_CHOICE, W_BUTTONS };
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
struct Widget {
    WidgetType type;
    const char *label;
    double *value = nullptr; // sliders
    bool *flag = nullptr;    // toggles
    double lo = 0, hi = 0;
    bool logScale = false;
    bool analysis = false; // changing it recomputes the analysis
    double display = 0;    // shown value = value * display
    const char *fmt = nullptr;
    int button = 0;
    int *choice = nullptr; // choices: a row of buttons, one per option
    const char *const *options = nullptr;
    int optionCount = 0;
};

static const char *const AUTO[] = {"Auto: whole item", "Auto: visible"};

// Sections fold at their header; what changes while transcribing is in the toolbar.
static std::vector<Widget> allWidgets() {
    return {
        {.type = W_HEADER, .label = "Display"},
        {.type = W_BUTTONS, .label = "", .button = B_AUTO_ALL, .options = AUTO, .optionCount = 2},
        {.type = W_SLIDER,
         .label = "Floor",
         .value = &g_settings.rp.dbFloor,
         .lo = -120,
         .hi = -20,
         .display = 1,
         .fmt = "%.0f dB"},
        {.type = W_SLIDER,
         .label = "Contrast",
         .value = &g_settings.rp.contrast,
         .lo = 0.1,
         .hi = 3,
         .logScale = true,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_SLIDER,
         .label = "Brightness",
         .value = &g_settings.rp.brightness,
         .lo = -1,
         .hi = 1,
         .display = 1,
         .fmt = "%+.2f"},
        {.type = W_SLIDER,
         .label = "Colour curve",
         .value = &g_settings.rp.bias,
         .lo = -2,
         .hi = 2,
         .display = 1,
         .fmt = "%+.2f"},
        {.type = W_CHOICE,
         .label = "Colours",
         .choice = &g_settings.rp.colormap,
         .options = COLORMAPS,
         .optionCount = 2},
        {.type = W_TOGGLE, .label = "Peaks only", .flag = &g_settings.rp.peaksOnly},
        {.type = W_SLIDER,
         .label = "Peak dimming",
         .value = &g_settings.rp.peakDim,
         .hi = 1,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_TOGGLE, .label = "Noise subtraction", .flag = &g_settings.rp.subtractNoise},
        {.type = W_SLIDER,
         .label = "Noise strength",
         .value = &g_settings.rp.noiseAlpha,
         .hi = 3,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_SLIDER,
         .label = "Noise from quietest",
         .value = &g_settings.rp.noiseFraction,
         .lo = 0.01,
         .hi = 0.5,
         .display = 100,
         .fmt = "%.0f%%"},
        {.type = W_HEADER, .label = "Suggestions and guides"},
        {.type = W_TOGGLE,
         .label = "Front-end vote (slower)",
         .flag = &g_settings.estimateVote,
         .analysis = true},
        {.type = W_TOGGLE,
         .label = "Drawn notes take the sung pitch",
         .flag = &g_settings.pitchFromContour},
        {.type = W_SLIDER,
         .label = "Overtones",
         .value = &g_settings.harmonics,
         .lo = 2,
         .hi = 16,
         .display = 1,
         .fmt = "%.0f"},
        {.type = W_SLIDER,
         .label = "Guide opacity",
         .value = &g_settings.guideOpacity,
         .hi = 1,
         .display = 1,
         .fmt = "%.2f"},
        {.type = W_SLIDER,
         .label = "Quantise strength",
         .value = &g_settings.quantStrength,
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
         .choice = &g_settings.channel,
         .options = CHANNELS,
         .optionCount = 5},
        {.type = W_SLIDER,
         .label = "Resolution (gamma)",
         .value = &g_settings.np.gamma,
         .lo = 3,
         .hi = 40,
         .logScale = true,
         .analysis = true,
         .display = 1,
         .fmt = "%.1f Hz"},
        {.type = W_SLIDER,
         .label = "Lowest frequency",
         .value = &g_settings.np.fmin,
         .lo = 20,
         .hi = 500,
         .logScale = true,
         .analysis = true,
         .display = 1,
         .fmt = "%.0f Hz"},
        {.type = W_SLIDER,
         .label = "Highest frequency",
         .value = &g_settings.np.fmax,
         .lo = 1000,
         .hi = 11000,
         .logScale = true,
         .analysis = true,
         .display = 1,
         .fmt = "%.0f Hz"},
        {.type = W_SLIDER,
         .label = "Bins per semitone",
         .value = &g_settings.binsPerSemitone,
         .lo = 1,
         .hi = 10,
         .analysis = true,
         .display = 1,
         .fmt = "%.0f"},
        {.type = W_BUTTON, .label = "Reset to defaults", .button = B_RESET},
        {.type = W_HEADER, .label = "Playback"},
        {.type = W_TOGGLE, .label = "Play notes when clicked", .flag = &g_settings.audition},
        {.type = W_SLIDER,
         .label = "Notes volume",
         .value = &g_notesDb,
         .lo = -30,
         .hi = 6,
         .display = 1,
         .fmt = "%+.1f dB"},
        {.type = W_TOGGLE,
         .label = "Isolate: all shown voices (shift+I)",
         .flag = &g_settings.isolateAll},
        {.type = W_SLIDER,
         .label = "Isolate: band width",
         .value = &g_settings.isolateWidth,
         .lo = 20,
         .hi = 400,
         .logScale = true,
         .display = 1,
         .fmt = "%.0f cents"},
        {.type = W_SLIDER,
         .label = "Isolate: the rest",
         .value = &g_settings.isolateBackground,
         .lo = -60,
         .hi = 0,
         .display = 1,
         .fmt = "%.0f dB"},
        {.type = W_HEADER, .label = "Window"},
        {.type = W_TOGGLE, .label = "Show other voices", .flag = &g_ui.showOthers},
        {.type = W_TOGGLE, .label = "Colours from REAPER theme", .flag = &g_settings.followTheme},
        {.type = W_TOGGLE,
         .label = "Link time to arrange view (L)",
         .flag = &g_settings.linkArrange},
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

static int widgetHeight(WidgetType t) {
    return t == W_HEADER                     ? 30
           : t == W_SLIDER                   ? 32
           : t == W_TOGGLE                   ? 20
           : t == W_CHOICE || t == W_BUTTONS ? 42
                                             : 24;
}

// Spectrogram settings need the display values recomputed; guide settings only a redraw.
static bool isRenderParam(const void *p) {
    return p >= (const void *)&g_settings.rp && p < (const void *)(&g_settings.rp + 1);
}

static double sliderFrac(const Widget &w, double v) {
    return w.logScale ? log(v / w.lo) / log(w.hi / w.lo) : (v - w.lo) / (w.hi - w.lo);
}
static double sliderValue(const Widget &w, double f) {
    f = std::max(0.0, std::min(1.0, f));
    return w.logScale ? w.lo * pow(w.hi / w.lo, f) : w.lo + (w.hi - w.lo) * f;
}

static int g_panelScroll; // pixels the panel is scrolled down

static int panelContentHeight() {
    int h = 16;
    for (const Widget &w : widgets()) {
        h += widgetHeight(w.type);
    }
    return h + 12;
}

void scrollPanel(int dy) {
    const Rect p = panelArea();
    g_panelScroll = std::max(0, std::min(g_panelScroll + dy, panelContentHeight() - (p.b - p.t)));
    redraw();
}

static int widgetAt(int y) {
    std::vector<Widget> ws = widgets();
    int top = panelArea().t + 8 - g_panelScroll;
    for (size_t i = 0; i < ws.size(); i++) {
        const int h = widgetHeight(ws[i].type);
        if (y >= top && y < top + h) {
            return (int)i;
        }
        top += h;
    }
    return -1;
}

static void autoCalibrateView() {
    const Rect n = noteArea();
    const int f0 = (int)((g_ui.view.t(n.l) - g_session.audioPos) * g_analysis.spec.frameRate());
    const int f1 = (int)((g_ui.view.t(n.r) - g_session.audioPos) * g_analysis.spec.frameRate()) + 1;
    autoCalibrate(g_analysis.spec, g_settings.rp, f0, f1);
}

static void applyNotesVolume();

static void changedSetting(const Widget &w) {
    if (w.analysis) {
        g_ui.analysisDirty = true;
        redraw();
        return;
    } // recomputed on mouse up
    if (w.value == &g_settings.rp.noiseFraction) {
        estimateNoise(g_analysis.spec, g_settings.rp.noiseFraction);
    }
    if (w.value == &g_notesDb) {
        applyNotesVolume();
    }
    if (isRenderParam(w.value)) {
        updateValues();
    } else {
        redraw();
    }
}

static void applyPreset(int i) {
    if (i == 0) {
        g_settings.channel = CH_CENTRE, g_settings.rp.subtractNoise = true, g_settings.np.fmin = 60,
        g_settings.np.fmax = 5000, g_settings.np.gamma = 11;
    } else if (i == 1) {
        g_settings.channel = CH_MIX, g_settings.rp.subtractNoise = false, g_settings.np.fmin = 60,
        g_settings.np.fmax = 6000, g_settings.np.gamma = 11;
    } else if (i == 2) {
        g_settings.np.gamma = 25; // wider bands, shorter windows: sharper timing for fast runs
    }
    requestAnalysis(true);
}

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
        autoCalibrate(g_analysis.spec, g_settings.rp);
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
    const double rate = SPEED_RATES[g_speed];
    if (rate != 1 && GetToggleCommandState(CMD_TOGGLE_PRESERVE_PITCH) == 0) {
        Main_OnCommand(CMD_TOGGLE_PRESERVE_PITCH, 0);
    }
    CSurf_OnPlayRateChange(rate);
}

// The voice tracks that exist, for this audio item.
static std::vector<MediaTrack *> voiceTracks() {
    std::vector<MediaTrack *> out;
    for (int v = 0; v < WINDOW_VOICES; v++) {
        if (MediaItem *it = VoiceItem(g_session.audio, v, false)) {
            out.push_back(GetMediaItem_Track(it));
        }
    }
    return out;
}

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
    g_listen = std::clamp(mode, 0, 2);
    applyListen();
}
int speedIndex() { return g_speed; }
void setSpeed(int index) {
    g_speed = std::clamp(index, 0, 2);
    applySpeed();
}

// Mutes and volume can also change in REAPER: show what the tracks say.
static void readBalance() {
    MediaTrack *a = GetMediaItem_Track(g_session.audio);
    const std::vector<MediaTrack *> vt = voiceTracks();
    const bool audioMuted = a && GetMediaTrackInfo_Value(a, "B_MUTE") != 0;
    const bool notesMuted = !vt.empty() && GetMediaTrackInfo_Value(vt[0], "B_MUTE") != 0;
    g_listen = audioMuted && !notesMuted   ? LISTEN_NOTES
               : notesMuted && !audioMuted ? LISTEN_AUDIO
                                           : LISTEN_BOTH;
    if (!vt.empty() && g_panelWidget < 0) {
        g_notesDb = 20 * log10(std::max(1e-6, GetMediaTrackInfo_Value(vt[0], "D_VOL")));
    }
}

// A slider's value in default settings: the same field of a fresh Settings; notes volume is 0 dB.
static double sliderDefault(const Widget &w) {
    static const Settings d;
    const char *base = (const char *)&g_settings, *p = (const char *)w.value;
    if (p >= base && p < base + sizeof(Settings)) {
        return *(const double *)((const char *)&d + (p - base));
    }
    return 0;
}

void panelDoubleClick(int y) {
    const int i = widgetAt(y);
    std::vector<Widget> ws = widgets();
    if (i >= 0 && ws[i].type == W_SLIDER && *ws[i].value != sliderDefault(ws[i])) {
        g_panelWidget = i, *ws[i].value = sliderDefault(ws[i]), changedSetting(ws[i]);
    }
}

void panelMouse(int x, int y, bool down) {
    const Rect p = panelArea();
    std::vector<Widget> ws = widgets();
    if (down) {
        g_panelWidget = widgetAt(y);
    }
    if (g_panelWidget < 0) {
        return;
    }
    const Widget &w = ws[g_panelWidget];
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
        const double v = sliderValue(w, (double)(x - p.l - 12) / (p.r - p.l - 24));
        if (v != *w.value) {
            *w.value = v, changedSetting(w);
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
        pressButton(w.button +
                    std::max(0, std::min(w.optionCount - 1, (x - p.l - 12) * w.optionCount /
                                                                std::max(1, p.r - p.l - 24))));
    } else if (down && w.type == W_CHOICE) {
        const int i = std::max(0, std::min(w.optionCount - 1, (x - p.l - 12) * w.optionCount /
                                                                  std::max(1, p.r - p.l - 24)));
        if (i == *w.choice) {
            return;
        }
        *w.choice = i;
        if (isRenderParam(w.choice)) {
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
    cv.fill(p.l, p.t, p.l + 1, p.b, 0x000000);
    syncPlayback();
    std::vector<Widget> ws = widgets();
    int top = panelArea().t + 8 - g_panelScroll;
    char buf[64];
    const int l = p.l + 12, r = p.r - 12;
    for (const Widget &w : ws) {
        const int h = widgetHeight(w.type);
        if (w.type == W_HEADER) {
            if (top > p.t + 4) {
                cv.fill(p.l + 1, top + 2, p.r, top + 3, g_theme.track);
            }
            text(dc, w.label, l, top + 6, r, top + h, g_theme.text, DT_LEFT);
            text(dc, folded(w.label) ? "+" : "–", l, top + 6, r, top + h, g_theme.dim, DT_RIGHT);
        } else if (w.type == W_SLIDER) {
            text(dc, w.label, l, top, r, top + 16, g_theme.text, DT_LEFT);
            snprintf(buf, sizeof(buf), w.fmt, *w.value * w.display);
            text(dc, buf, l, top, r, top + 16, g_theme.dim, DT_RIGHT);
            const int by = top + 20,
                      bx = l + (int)lround((r - l) *
                                           std::max(0.0, std::min(1.0, sliderFrac(w, *w.value))));
            cv.fill(l, by + 3, r, by + 7, g_theme.track);
            cv.fill(l, by + 3, bx, by + 7, w.analysis && g_ui.analysisDirty ? 0xc0a040 : C_ACCENT);
            cv.fill(bx - 3, by, bx + 3, by + 10, g_theme.text);
        } else if (w.type == W_TOGGLE) {
            cv.frame(l, top + 5, l + 12, top + 17, g_theme.text);
            if (*w.flag) {
                cv.fill(l + 3, top + 8, l + 9, top + 14, C_ACCENT);
            }
            text(dc, w.label, l + 18, top, r, top + 22, g_theme.text, DT_LEFT);
        } else if (w.type == W_CHOICE || w.type == W_BUTTONS) {
            text(dc, w.label, l, top, r, top + 16, g_theme.text, DT_LEFT);
            for (int i = 0; i < w.optionCount; i++) {
                const int bl = l + (r - l) * i / w.optionCount,
                          br = l + (r - l) * (i + 1) / w.optionCount;
                const bool on = w.choice && i == *w.choice;
                cv.fill(bl, top + 18, br - 2, top + 40, on ? C_ACCENT : g_theme.button);
                text(dc, w.options[i], bl, top + 18, br - 2, top + 40, on ? 0x14161c : g_theme.text,
                     DT_CENTER);
            }
        } else {
            cv.fill(l, top + 2, r, top + h - 2, g_theme.button);
            text(dc, w.label, l, top + 2, r, top + h - 2, g_theme.text, DT_CENTER);
        }
        top += h;
    }
}

// The speed and what is heard, as REAPER has them (both can change elsewhere).
void syncPlayback() {
    const double rate = Master_GetPlayRate(NULL);
    for (int i = 0; i < 3; i++) {
        if (fabs(SPEED_RATES[i] - rate) < fabs(SPEED_RATES[g_speed] - rate)) {
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
