// Per-item settings in the project, and defaults for new items.
#pragma once

#include "state.h"

// REAPER extended state: the section, and the key of the defaults for new items in it.
static const char *EXT_SECTION = "reatrace";
static const char *EXT_DEFAULTS = "defaults";
// A global extended state value, from before the rename if there is no newer one.
const char *globalState(const char *key);
std::string saveSettings();         // g_settings as text
void loadSettings(const char *str); // over g_settings; what str doesn't mention stays
void resetSettings();               // g_settings to the built-in defaults
// g_settings for the open item: the defaults, then the item's own; true if it had its own.
bool loadItemSettings();
void saveItemSettings(); // to the open item, if they changed since loaded or last saved
