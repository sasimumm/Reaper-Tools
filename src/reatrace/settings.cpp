// Settings are remembered per audio item (project extended state) and as defaults for new items
// (REAPER extended state), as "name=value;" pairs.
#include "settings.h"

// before the rename; read when there's nothing newer
static const char *OLD_EXT_SECTION = "reascribe";

const char *globalState(const char *key) {
    const char *v = GetExtState(EXT_SECTION, key);
    return v && *v ? v : GetExtState(OLD_EXT_SECTION, key);
}

std::string saveSettings() { return encodeSettings(g_settings); }

void loadSettings(const char *str) { decodeSettings(str, g_settings); }

void resetSettings() { g_settings = Settings(); }

static std::string g_savedSettings; // last written to the item, to only write on change

// Per-item settings live in the project's extended state under the item's GUID (that API takes
// a buffer size, unlike item P_EXT strings). `key` takes 64 chars; false without an item.
static bool itemKey(char *key) {
    if (!g_session.audio) {
        return false;
    }
    const auto *g = static_cast<const GUID *>(GetSetMediaItemInfo(g_session.audio, "GUID", NULL));
    if (!g) {
        return false;
    }
    guidToString(g, key);
    return true;
}

bool loadItemSettings() {
    resetSettings();
    loadSettings(globalState(EXT_DEFAULTS));

    char key[64];
    char buf[4096] = "";
    // the item's settings in `section` into buf; true if there are any
    const auto read = [&](const char *section) {
        return GetProjExtState(NULL, section, key, buf, sizeof(buf)) > 0 && *buf;
    };
    const bool has = itemKey(key) && (read(EXT_SECTION) || read(OLD_EXT_SECTION));
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
