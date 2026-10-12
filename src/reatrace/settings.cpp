#include "settings.h"

// Settings are remembered per audio item (item extended state) and as defaults for new items
// (REAPER extended state), as "name=value;" pairs.
static const char *OLD_EXT_SECTION =
    "reascribe"; // before the rename; read when there's nothing newer

const char *globalState(const char *key) {
    const char *v = GetExtState(EXT_SECTION, key);
    return v && *v ? v : GetExtState(OLD_EXT_SECTION, key);
}

std::string saveSettings() { return encodeSettings(g_settings); }

void loadSettings(const char *str) { decodeSettings(str, g_settings); }

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
