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
    printf("ok\n");
}
