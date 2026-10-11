// Per-item settings in the project, and defaults for new items.
#pragma once

#include "state.h"

static const char *EXT_SECTION = "reatrace", *EXT_DEFAULTS = "defaults";
const char *globalState(const char *key);
std::string saveSettings();
void loadSettings(const char *str);
void resetSettings();
bool loadItemSettings();
void saveItemSettings();
