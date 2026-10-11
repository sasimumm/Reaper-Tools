#include "ui.h"

// Settings are remembered per audio item (item extended state) and as defaults for new items
// (REAPER extended state), as "name=value;" pairs.
struct Setting {
    const char *name = nullptr;
    double *d = nullptr;
    bool *b = nullptr;
    int *i = nullptr;
    double lo = 0, hi = 0; // loaded values are clamped to this range
};
static const Setting SETTINGS[] = {
    {.name = "gamma", .d = &g_settings.np.gamma, .lo = 3, .hi = 40},
    {.name = "fmin", .d = &g_settings.np.fmin, .lo = 20, .hi = 500},
    {.name = "fmax", .d = &g_settings.np.fmax, .lo = 1000, .hi = 11000},
    {.name = "binspersemitone", .d = &g_settings.binsPerSemitone, .lo = 1, .hi = 10},
    {.name = "channel", .i = &g_settings.channel, .lo = 0, .hi = CH_CENTRE},
    {.name = "bass", .b = &g_settings.np.bass},
    {.name = "basscrossover", .d = &g_settings.np.bassCrossover, .lo = 80, .hi = 500},
    {.name = "floor", .d = &g_settings.rp.dbFloor, .lo = -120, .hi = -20},
    {.name = "contrast", .d = &g_settings.rp.contrast, .lo = 0.1, .hi = 3},
    {.name = "brightness", .d = &g_settings.rp.brightness, .lo = -1, .hi = 1},
    {.name = "bias", .d = &g_settings.rp.bias, .lo = -2, .hi = 2},
    {.name = "peaks", .b = &g_settings.rp.peaksOnly},
    {.name = "peakdim", .d = &g_settings.rp.peakDim, .lo = 0, .hi = 1},
    {.name = "denoise", .b = &g_settings.rp.subtractNoise},
    {.name = "noisealpha", .d = &g_settings.rp.noiseAlpha, .lo = 0, .hi = 3},
    {.name = "noisefrac", .d = &g_settings.rp.noiseFraction, .lo = 0.01, .hi = 0.5},
    {.name = "ghosts", .b = &g_settings.ghosts},
    {.name = "harmonics", .d = &g_settings.harmonics, .lo = 2, .hi = 16},
    {.name = "guideopacity", .d = &g_settings.guideOpacity, .lo = 0, .hi = 1},
    {.name = "explain", .b = &g_settings.explain},
    {.name = "contour", .b = &g_settings.showContour},
    {.name = "suggest", .b = &g_settings.suggest},
    {.name = "suggestfrom", .i = &g_settings.suggestFrom, .lo = 0, .hi = 1},
    {.name = "estimatevote", .b = &g_settings.estimateVote},
    {.name = "pitchfromcontour", .b = &g_settings.pitchFromContour},
    {.name = "audition", .b = &g_settings.audition},
    {.name = "theme", .b = &g_settings.followTheme},
    {.name = "linkarrange", .b = &g_settings.linkArrange},
    {.name = "snap", .i = &g_settings.snapMode, .lo = 0, .hi = SNAP_GRID},
    {.name = "quantise", .d = &g_settings.quantStrength, .lo = 0, .hi = 1},
};
static const char *OLD_EXT_SECTION =
    "reascribe"; // before the rename; read when there's nothing newer

const char *globalState(const char *key) {
    const char *v = GetExtState(EXT_SECTION, key);
    return v && *v ? v : GetExtState(OLD_EXT_SECTION, key);
}

std::string saveSettings() {
    std::string out;
    char buf[64];
    for (const Setting &st : SETTINGS) {
        if (st.d) {
            snprintf(buf, sizeof(buf), "%s=%.6g;", st.name, *st.d);
        } else if (st.b) {
            snprintf(buf, sizeof(buf), "%s=%d;", st.name, *st.b ? 1 : 0);
        } else if (st.i) {
            snprintf(buf, sizeof(buf), "%s=%d;", st.name, *st.i);
        } else {
            continue;
        }
        out += buf;
    }
    return out;
}

// Unknown names are ignored; values are clamped (they come from the project file).
void loadSettings(const char *str) {
    for (const char *p = str; p && *p;) {
        const char *eq = strchr(p, '='), *end = strchr(p, ';');
        if (!eq || !end || eq > end) {
            break;
        }
        const std::string name(p, eq - p);
        const double v = strtod(eq + 1, NULL);
        for (const Setting &st : SETTINGS) {
            if (name == st.name) {
                const double c = v >= st.lo ? std::min(v, st.hi) : st.lo; // NaN -> lo
                if (st.d) {
                    *st.d = c;
                } else if (st.b) {
                    *st.b = v != 0;
                } else if (st.i) {
                    *st.i = (int)c;
                }
            }
        }
        p = end + 1;
    }
}

void resetSettings() { g_settings = Settings(); }
static std::string g_savedSettings; // last written to the item, to only write on change

// Per-item settings live in the project's extended state under the item's GUID (that API takes
// a buffer size, unlike item P_EXT strings).
static bool itemKey(char *key) {
    const GUID *g =
        g_session.audio ? (const GUID *)GetSetMediaItemInfo(g_session.audio, "GUID", NULL) : NULL;
    if (!g) {
        return false;
    }
    guidToString(g, key);
    return true;
}

// Returns true if the item had saved settings.
bool loadItemSettings() {
    resetSettings();
    loadSettings(globalState(EXT_DEFAULTS));
    char key[64], buf[4096] = "";
    const bool has = itemKey(key) &&
                     ((GetProjExtState(NULL, EXT_SECTION, key, buf, sizeof(buf)) > 0 && *buf) ||
                      (GetProjExtState(NULL, OLD_EXT_SECTION, key, buf, sizeof(buf)) > 0 && *buf));
    buf[sizeof(buf) - 1] = 0;
    if (has) {
        loadSettings(buf);
    }
    g_savedSettings = saveSettings();
    return has;
}

void saveItemSettings() {
    if (!g_session.audio || !ValidatePtr2(NULL, g_session.audio, "MediaItem*")) {
        return;
    }
    const std::string cur = saveSettings();
    if (cur == g_savedSettings) {
        return;
    }
    char key[64];
    if (!itemKey(key)) {
        return;
    }
    SetProjExtState(NULL, EXT_SECTION, key, cur.c_str());
    g_savedSettings = cur;
}
