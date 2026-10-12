// Adding an extension's actions to REAPER's menus, from its "hookcustommenu" hook. REAPER calls the
// hook with flag 0 while it builds a default menu; items added then become part of that menu, which
// the user can rearrange. Items are found by command id, not position, as the order can change.
#pragma once

#include <string_view>

#include "reaper_plugin.h"

// The REAPER menus the extensions add to, as the hook names them.
inline constexpr std::string_view MENU_EXTENSIONS = "Main extensions"; // needs AddExtensionsMainMenu()
inline constexpr std::string_view MENU_INSERT = "Main insert";
inline constexpr std::string_view MENU_ITEM_CONTEXT = "Media item context";

// REAPER's "Insert new MIDI item..." action.
inline constexpr int CMD_INSERT_MIDI_ITEM = 40214;

// Inserts an item running command at position pos (-1: at the end).
inline void insertMenuCommand(HMENU menu, int pos, const char *label, int command) {
    MENUITEMINFO mi = {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_TYPE | MIIM_ID;
    mi.fType = MFT_STRING;
    mi.wID = static_cast<unsigned int>(command);
    mi.dwTypeData = const_cast<char *>(label); // only read
    InsertMenuItem(menu, pos < 0 ? GetMenuItemCount(menu) : pos, TRUE, &mi);
}

// Appends a separator line.
inline void addMenuSeparator(HMENU menu) {
    MENUITEMINFO mi = {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_TYPE;
    mi.fType = MFT_SEPARATOR;
    InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &mi);
}

// Appends a submenu `label` holding one item running command.
inline void addMenuSubmenu(HMENU menu, const char *label, const char *itemLabel, int command) {
    HMENU sub = CreatePopupMenu();
    insertMenuCommand(sub, -1, itemLabel, command);

    MENUITEMINFO mi = {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_TYPE | MIIM_SUBMENU;
    mi.fType = MFT_STRING;
    mi.hSubMenu = sub; // the menu owns it from here
    mi.dwTypeData = const_cast<char *>(label);
    InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &mi);
}

// Inserts an item running command right after the one running `after`, or at the end if the menu
// has none (the user may have removed it).
inline void insertMenuCommandAfter(HMENU menu, int after, const char *label, int command) {
    const int n = GetMenuItemCount(menu);
    int pos = -1;
    for (int i = 0; i < n; i++) {
        if (GetMenuItemID(menu, i) == after) {
            pos = i + 1;
            break;
        }
    }
    insertMenuCommand(menu, pos, label, command);
}
