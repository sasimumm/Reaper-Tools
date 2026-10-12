#include "ui/theme.h"

Colour mix(Colour a, Colour b, double f) {
    Colour out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        out |= (Colour)lround(((a >> sh) & 255) * (1 - f) + ((b >> sh) & 255) * f) << sh;
    }
    return out;
}

void applyTheme() {
    Theme t;
    auto col = [](const char *key, Colour dflt) -> Colour {
        const int c = g_settings.followTheme ? GetThemeColor(key, 0) : -1;
        return c < 0 ? dflt
                     : ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255); // COLORREF is 0xBBGGRR
    };
    t.bg = col("col_main_bg2", t.bg), t.panel = col("col_main_bg", t.panel);
    t.ruler = col("col_tl_bg", t.ruler), t.text = col("col_main_text2", t.text);
    t.on = col("col_toolbar_text_on", t.on);
    t.dim = mix(t.text, t.panel, 0.4), t.button = mix(t.panel, t.text, 0.1),
    t.track = mix(t.panel, t.text, 0.2);
    g_theme = t;
}
