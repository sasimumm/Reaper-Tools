#include "ui/theme.h"

unsigned int mix(unsigned int a, unsigned int b, double f) {
    unsigned int out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        out |= (unsigned int)lround(((a >> sh) & 255) * (1 - f) + ((b >> sh) & 255) * f) << sh;
    }
    return out;
}

void applyTheme() {
    auto col = [](const char *key, unsigned int dflt) -> unsigned int {
        const int c = g_settings.followTheme ? GetThemeColor(key, 0) : -1;
        return c < 0 ? dflt
                     : ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255); // COLORREF is 0xBBGGRR
    };
    g_theme.bg = col("col_main_bg2", 0x1b1b1b), g_theme.panel = col("col_main_bg", 0x222222);
    g_theme.ruler = col("col_tl_bg", 0x262626), g_theme.text = col("col_main_text2", 0xd0d0d0);
    g_theme.on = col("col_toolbar_text_on", C_ACCENT);
    g_theme.dim = mix(g_theme.text, g_theme.panel, 0.4),
    g_theme.button = mix(g_theme.panel, g_theme.text, 0.1),
    g_theme.track = mix(g_theme.panel, g_theme.text, 0.2);
}
