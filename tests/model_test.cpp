// Self-check for the window's REAPER-free logic: run `build/model_test`.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/model.h"

int main() {
    // settings round-trip through the project's text format
    {
        Settings a;
        a.np.gamma = 25, a.rp.contrast = 1.75, a.channel = CH_CENTRE, a.ghosts = false;
        a.snapMode = SNAP_GRID, a.guideOpacity = 0.4, a.estimateVote = false;
        Settings b;
        decodeSettings(encodeSettings(a).c_str(), b);
        assert(encodeSettings(b) == encodeSettings(a));
        assert(b.np.gamma == 25 && b.rp.contrast == 1.75 && b.channel == CH_CENTRE && !b.ghosts);
        assert(b.snapMode == SNAP_GRID && !b.estimateVote && b.guideOpacity == 0.4);
    }
    // defaults survive an empty or unrelated string; unknown names are ignored
    {
        Settings s;
        const std::string before = encodeSettings(s);
        decodeSettings("", s);
        decodeSettings(nullptr, s);
        decodeSettings("nosuch=5;", s);
        assert(encodeSettings(s) == before);
    }
    // values from a project file are clamped; NaN goes to the bottom of the range; a malformed
    // pair stops reading (what came before it still applies)
    {
        Settings s;
        decodeSettings("gamma=1000;fmin=nan;channel=99;harmonics=-3;quantise=0.5;", s);
        assert(s.np.gamma == 40 && s.np.fmin == 20 && s.channel == CH_CENTRE && s.harmonics == 2);
        assert(s.quantStrength == 0.5);
        decodeSettings("ghosts=0;contrast 2;suggest=0;", s);
        assert(!s.ghosts && s.suggest); // stopped at the pair without '='
        decodeSettings("snap=1.9;", s);
        assert(s.snapMode == SNAP_ONSETS); // ints are truncated
    }
    // quantise: selected notes only, towards the grid by the strength, never shorter than minLen
    {
        auto grid = [](double t) { return std::round(t * 4) / 4; }; // quarter seconds
        std::vector<Note> notes = {
            {1.1, 1.4, 60, true, 0}, {2.1, 2.4, 62, false, 1}, {3.02, 3.08, 64, true, 2}
        };
        assert(quantiseNotes(notes, grid, 1, 0.05));
        assert(notes[0].start == 1.0 && notes[0].end == 1.5);
        assert(notes[1].start == 2.1 && notes[1].end == 2.4); // not selected
        assert(notes[2].start == 3.0 &&
               std::fabs(notes[2].end - 3.06) < 1e-9); // both snap to 3: length kept
        std::vector<Note> half = {{1.1, 1.4, 60, true, 0}};
        assert(quantiseNotes(half, grid, 0.5, 0.05));
        assert(std::fabs(half[0].start - 1.05) < 1e-9 && std::fabs(half[0].end - 1.45) < 1e-9);
        assert(!quantiseNotes(notes, grid, 1, 0.05)); // already on the grid
    }
    // a suggestion is hidden by a note within a semitone over a third of its length
    {
        const Note n = {1.0, 2.0, 60, false, 0};
        assert(coversSuggestion(n, 1.2, 1.8, 60));
        assert(coversSuggestion(n, 1.2, 1.8, 61));  // a corrected pitch
        assert(!coversSuggestion(n, 1.2, 1.8, 62)); // a harmony
        assert(coversSuggestion(n, 1.5, 2.5, 60));  // half overlaps
        assert(!coversSuggestion(n, 1.8, 2.8, 60)); // a fifth overlaps
    }
    // layout: the areas stack without gaps, the panel only when there is room
    {
        const Layout L = computeLayout(1300, 760, true);
        assert(L.panelShown && !L.tooSmall);
        assert(L.toolbar.t == 0 && L.toolbar.r == 1300 && L.toolbar.b == L.ruler.t);
        assert(L.ruler.b == L.chords.t && L.chords.b == L.notes.t);
        assert(L.notes.b == L.lyrics.t && L.lyrics.b == L.overview.t && L.overview.b == L.status.t);
        assert(L.status.b == 760 && L.status.b - L.status.t == STATUS_H);
        assert(L.notes.l == KB_W && L.notes.r == L.panel.l && L.panel.r == 1300 &&
               1300 - L.panel.l == PANEL_W);
        assert(L.panel.t == L.toolbar.b && L.panel.b == L.status.t);
        assert(L.ruler.l == L.notes.l && L.ruler.r == L.notes.r && L.overview.r == L.notes.r);

        const Layout off = computeLayout(1300, 760, false);
        assert(!off.panelShown && off.notes.r == 1300);
        const Layout narrow =
            computeLayout(KB_W + PANEL_W + 99, 760, true); // no room for the panel
        assert(!narrow.panelShown && narrow.notes.r == KB_W + PANEL_W + 99);

        // at the minimum size every area still fits; below it only the background is drawn
        const Layout min = computeLayout(MIN_W, MIN_H, true);
        assert(!min.tooSmall && min.notes.b > min.notes.t && min.status.b <= MIN_H &&
               min.notes.r > min.notes.l);
        assert(computeLayout(MIN_W - 1, MIN_H, true).tooSmall &&
               computeLayout(MIN_W, MIN_H - 1, true).tooSmall);
        // folded lanes become thin strips and give their room to the notes
        const Layout f = computeLayout(1300, 760, true, LANE_CHORDS | LANE_OVERVIEW);
        assert(f.chords.b - f.chords.t == FOLDED_H && f.overview.b - f.overview.t == FOLDED_H);
        assert(f.lyrics.b - f.lyrics.t == LYRICS_H && f.overview.b == f.status.t);
        assert((f.notes.b - f.notes.t) - (L.notes.b - L.notes.t) ==
               (CHORDS_H - FOLDED_H) + (OVERVIEW_H - FOLDED_H));
        const Layout tiny = computeLayout(-5, 3, true);
        assert(tiny.tooSmall && tiny.client.r == 0);
    }
    printf("ok\n");
}
