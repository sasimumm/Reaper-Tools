// Self-check for the window's REAPER-free logic: run `build/model_test`.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "model.h"

int main() {
    // settings round-trip through the project's text format
    {
        Settings a;
        a.np.gamma = 25;
        a.rp.contrast = 1.75;
        a.channel = CH_CENTRE;
        a.ghosts = false;
        a.snapMode = SNAP_GRID;
        a.guideOpacity = 0.4;
        a.estimateVote = false;
        a.suggestFrom = FROM_REFERENCE;
        a.reference = "{0A1B-2C} {3D4E-5F}";
        Settings b;
        decodeSettings(encodeSettings(a).c_str(), b);
        assert(encodeSettings(b) == encodeSettings(a));
        assert(b.np.gamma == 25 && b.rp.contrast == 1.75 && b.channel == CH_CENTRE && !b.ghosts);
        assert(b.snapMode == SNAP_GRID && !b.estimateVote && b.guideOpacity == 0.4);
        assert(b.suggestFrom == FROM_REFERENCE && b.reference == a.reference);
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
        // both ends snap to 3: the length is kept
        assert(notes[2].start == 3.0 && std::fabs(notes[2].end - 3.06) < 1e-9);
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

        // the panel hidden, or too narrow for it
        const Layout off = computeLayout(1300, 760, false);
        assert(!off.panelShown && off.notes.r == 1300);
        // no room for the panel
        const Layout narrow = computeLayout(KB_W + PANEL_W + 99, 760, true);
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

        // a negative size: too small, with an empty client area
        const Layout tiny = computeLayout(-5, 3, true);
        assert(tiny.tooSmall && tiny.client.r == 0);
    }

    // follow playback: pages when the cursor leaves the view, not when it was already outside
    {
        assert(followPage(10, 4, 13, 13.5) == 10);     // still inside
        assert(followPage(10, 4, 13.9, 14.1) == 13.9); // off the right edge: lands 1/20 in
        assert(followPage(10, 4, 13.9, 2) == 1.8);     // loop back to before the view
        assert(followPage(10, 4, 20, 20.1) == 10);     // scrolled away from the cursor
    }

    // isolate schedule: by start, only spans still to come (or sounding), at most `max`
    {
        const std::vector<Note> spans = {
            {3, 4, 64, false, -1},
            {0, 1, 60, false, -1},
            {1, 2.5, 62, false, -1},
            {5, 6, 67, false, -1}
        };
        const auto s = pitchSchedule(spans, 2, 2);
        assert(s.size() == 2 && s[0] == std::make_pair(1.0, 62) && s[1] == std::make_pair(3.0, 64));
        assert(pitchSchedule(spans, 10, 5).empty());
    }

    // intervals: Dorico's syntax, diatonic in the scale unless a quality is given
    {
        const auto in = parseIntervals(" -m3, 4 ,P5,aug4");
        assert(in && !in->transpose && in->intervals.size() == 4);
        assert(in->intervals[0].degree == -3 && in->intervals[0].quality == IQ_MINOR);
        assert(in->intervals[1].degree == 4 && in->intervals[1].quality == IQ_DIATONIC);
        assert(in->intervals[3].quality == IQ_AUGMENTED);
        const auto t = parseIntervals("t-min6");
        assert(t && t->transpose && t->intervals[0].degree == -6);
        assert(parseIntervals("M3") && parseIntervals("Maj3") && parseIntervals("dim7"));
        for (const char *bad : {"", "x", "M5", "P3", "3,", "0", "3b", "23"}) {
            assert(!parseIntervals(bad));
        }

        const int C = 0, E = 4, major = 0xAB5, minor = 0x5AD;         // E minor: E F# G A B C D
        assert(intervalPitch(62, {3, IQ_DIATONIC}, C, major) == 65);  // D -> F in C major
        assert(intervalPitch(60, {3, IQ_DIATONIC}, C, major) == 64);  // C -> E
        assert(intervalPitch(64, {-3, IQ_DIATONIC}, C, major) == 60); // E -> C
        assert(intervalPitch(71, {2, IQ_DIATONIC}, C, major) == 72);  // B -> C, over the octave
        assert(intervalPitch(60, {-2, IQ_DIATONIC}, C, major) == 59); // C -> B, below
        assert(intervalPitch(60, {8, IQ_DIATONIC}, C, major) == 72);
        assert(intervalPitch(61, {3, IQ_DIATONIC}, C, major) == 65); // C# -> E#
        assert(intervalPitch(64, {3, IQ_DIATONIC}, E, minor) == 67); // E -> G in E minor
        assert(intervalPitch(66, {3, IQ_DIATONIC}, E, minor) == 69); // F# -> A
        assert(intervalPitch(60, {3, IQ_DIATONIC}, E, minor) == 64); // C -> E, below the root
        assert(intervalPitch(60, {3, IQ_MAJOR}, E, minor) == 64);
        assert(intervalPitch(60, {-3, IQ_MINOR}, C, major) == 57);
        assert(intervalPitch(60, {5, IQ_DIMINISHED}, C, major) == 66);
        assert(intervalPitch(60, {4, IQ_AUGMENTED}, C, major) == 66);
        assert(intervalPitch(60, {10, IQ_MAJOR}, C, major) == 76);
        assert(intervalPitch(125, {3, IQ_MAJOR}, C, major) == -1);

        // a third, then another stacks on the new note, which is the selection now; below goes from
        // the lowest selected
        const auto parsed = [](const char *text) {
            const std::optional<IntervalInput> r = parseIntervals(text);
            assert(r);
            return r.value_or(IntervalInput{});
        };
        std::vector<Note> ns = {{1, 2, 60, true, 5, "la"}, {3, 4, 62, false, 6}};
        assert(addIntervals(ns, parsed("3"), C, major));
        assert(ns.size() == 3 && ns[2].pitch == 64 && ns[2].sel && !ns[0].sel && ns[2].idx == -1 &&
               ns[2].lyric.empty() && ns[2].start == 1 && ns[2].end == 2);
        assert(addIntervals(ns, parsed("3,-4"), C, major));
        assert(ns.size() == 5 && ns[3].pitch == 67 && ns[4].pitch == 59 && !ns[2].sel);
        assert(ns[3].sel && ns[4].sel);
        assert(addIntervals(ns, parsed("-4"), C, major) && ns.back().pitch == 53);
        std::vector<Note> tr = {{1, 2, 60, true, 5}, {3, 4, 62, false, 6}};
        assert(addIntervals(tr, parsed("t3"), C, major) && tr[0].pitch == 64 && tr[1].pitch == 62 &&
               tr.size() == 2);

        // note names: above (or below with -) the outer selected notes; tA moves to the nearest
        const auto bb = parsed("Bb, -f#");
        assert(bb.intervals.size() == 2 && bb.intervals[0].pc == 10 && bb.intervals[0].degree == 1);
        assert(bb.intervals[1].pc == 6 && bb.intervals[1].degree == -1);
        assert(!parseIntervals("H") && !parseIntervals("Bx") && !parseIntervals("A3b"));
        // with an octave, the pitch; lowercase a3, d5 stay intervals
        assert(parsed("A3").intervals[0].pitch == 57 && parsed("F#2").intervals[0].pitch == 42);
        assert(parsed("B#3").intervals[0].pitch == 60 && parsed("Cb4").intervals[0].pitch == 59);
        assert(parsed("a3").intervals[0].quality == IQ_AUGMENTED);
        assert(parsed("d5").intervals[0].quality == IQ_DIMINISHED && !parseIntervals("G#9"));
        std::vector<Note> nn = {{1, 2, 60, true, 5}, {1, 2, 64, true, 6}};
        assert(addIntervals(nn, parsed("A"), C, major) && nn.size() == 3 && nn[2].pitch == 69);
        nn[2].sel = false;
        nn[0].sel = nn[1].sel = true;
        assert(addIntervals(nn, parsed("-A"), C, major) && nn.size() == 4 && nn[3].pitch == 57);
        std::vector<Note> ta = {{1, 2, 60, true, 5}, {3, 4, 66, true, 6}, {5, 6, 69, true, 7}};
        assert(addIntervals(ta, parsed("tA"), C, major));
        assert(ta[0].pitch == 57 && ta[1].pitch == 69 && ta[2].pitch == 69);
        assert(addIntervals(ta, parsed("t+C"), C, major) && ta[0].pitch == 60 && ta[2].pitch == 72);
        assert(addIntervals(ta, parsed("tE2"), C, major) && ta[0].pitch == 40 && ta[2].pitch == 40);
        std::vector<Note> ab = {{1, 2, 60, true, 5}, {1, 2, 64, true, 6}};
        assert(addIntervals(ab, parsed("F#2"), C, major) && ab.size() == 3 && ab[2].pitch == 42);
    }

    // chord names to their notes
    {
        const auto tones = [](const char *name) {
            const std::optional<ChordTones> c = chordTones(name);
            assert(c);
            return c.value_or(ChordTones{});
        };
        const auto bits = [](std::initializer_list<int> st) {
            unsigned m = 0;
            for (const int s : st) {
                m |= 1u << s;
            }
            return m;
        };
        assert(tones("C").root == 0 && tones("C").tones == bits({0, 4, 7}) && tones("C").bass < 0);
        assert(tones("F#m").root == 6 && tones("F#m").tones == bits({0, 3, 7}));
        assert(tones("Bb7").root == 10 && tones("Bb7").tones == bits({0, 4, 7, 10}));
        assert(tones("Ebmaj7").root == 3 && tones("Ebmaj7").tones == bits({0, 4, 7, 11}));
        assert(tones("Am7b5").tones == bits({0, 3, 6, 10}));
        assert(tones("Bdim7").tones == bits({0, 3, 6, 9}) &&
               tones("Bdim").tones == bits({0, 3, 6}));
        assert(tones("Caug").tones == bits({0, 4, 8}) && tones("C+").tones == bits({0, 4, 8}));
        assert(tones("Dsus4").tones == bits({0, 5, 7}) && tones("Dsus2").tones == bits({0, 2, 7}));
        assert(tones("C6/9").tones == bits({0, 2, 4, 7, 9}));
        assert(tones("Cadd9").tones == bits({0, 2, 4, 7}));
        assert(tones("G9").tones == bits({0, 2, 4, 7, 10}));
        assert(tones("Cm(maj7)").tones == bits({0, 3, 7, 11}));
        assert(tones("Cmaj").tones == bits({0, 4, 7}) && tones("Cmaj6").tones == bits({0, 4, 7, 9}));
        assert(tones("C\u0394").tones == bits({0, 4, 7, 11}));
        assert(tones("C5").tones == bits({0, 7}));
        const ChordTones slash = tones("C/E");
        assert(slash.root == 0 && slash.bass == 4 && slash.tones == bits({0, 4, 7}));
        assert(tones("D\u266D").root == 1 && tones("F\u266F").root == 6);
        assert(!chordTones("N.C.") && !chordTones("") && !chordTones("Cxyz"));
    }

    // a project's key signature
    {
        const auto k = keySigLine("    0 4 1 0xAB5");
        assert(k && k->first == 4 && k->second == 0xAB5);
        assert(keySigLine("    0 4 1 0xAB5\r") == std::make_pair(4, 0xAB5)); // CRLF line ends
        assert(keySigLine("12.5 9 0 5AD") == std::make_pair(9, 0x5AD));
        for (const char *bad :
             {"", "0 4 1", "0 12 1 0xAB5", "0 4 1 0x1000", "0 4 1 0xAB5 7", "0 x 1 1"}) {
            assert(!keySigLine(bad));
        }
    }

    printf("ok\n");
}
