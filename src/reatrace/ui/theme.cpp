// The window's palette: the built-in colours, optionally replaced by REAPER's theme colours.
#include "ui/theme.h"
#include "index.h"

// Each channel separately, rounded.
RGB mix(RGB a, RGB b, double f) {
    RGB out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        out |= static_cast<RGB>(lround(((a >> sh) & 255) * (1 - f) + ((b >> sh) & 255) * f)) << sh;
    }
    return out;
}

void applyTheme() {
    Theme t;
    // the theme's colour for a reaper.ini key, or dflt when not following the theme or unset
    auto col = [](const char *key, RGB dflt) -> RGB {
        const int c = g_settings.followTheme ? GetThemeColor(key, 0) : -1;
        if (c < 0) {
            return dflt;
        }
        // COLORREF is 0xBBGGRR
        return static_cast<RGB>(((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255));
    };

    t.bg = col("col_main_bg2", t.bg);
    t.panel = col("col_main_bg", t.panel);
    t.ruler = col("col_tl_bg", t.ruler);
    t.text = col("col_main_text2", t.text);
    t.on = col("col_toolbar_text_on", t.on);

    // the derived greys follow whichever text and panel colours were chosen
    t.dim = mix(t.text, t.panel, 0.4);
    t.button = mix(t.panel, t.text, 0.1);
    t.track = mix(t.panel, t.text, 0.2);

    g_theme = t;
}
