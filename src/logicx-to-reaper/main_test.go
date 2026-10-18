package main

import (
	"math"
	"strings"
	"testing"

	"github.com/loov/logicx"
)

func TestSeconds(t *testing.T) {
	// 120 bpm for two bars, then 60 bpm.
	tl := timeline{origin: barOneTick, bpm: 120, tempos: []logicx.TempoChange{
		{Position: 0, BPM: 120},
		{Position: barOneTick + 8*ticksPerQuarter, BPM: 60},
	}}
	for _, c := range []struct {
		tick uint32
		want float64
	}{
		{0, 0}, // before the origin
		{barOneTick, 0},
		{barOneTick + 4*ticksPerQuarter, 2},
		{barOneTick + 8*ticksPerQuarter, 4},
		{barOneTick + 10*ticksPerQuarter, 6},
	} {
		if got := tl.seconds(c.tick); math.Abs(got-c.want) > 1e-9 {
			t.Errorf("seconds(%d) = %v, want %v", c.tick, got, c.want)
		}
	}
}

func TestMIDIEvents(t *testing.T) {
	var b strings.Builder
	writeMIDIEvents(&b, []midiEvent{
		{tick: 0, msg: [3]byte{0x90, 60, 80}},
		{tick: 480, muted: true, msg: [3]byte{0x80, 60, 0}},
	}, 960)
	want := "        E 0 90 3c 50\n        Em 480 80 3c 00\n        E 480 b0 7b 00\n"
	if b.String() != want {
		t.Errorf("got\n%s\nwant\n%s", b.String(), want)
	}
}

func TestQuote(t *testing.T) {
	for in, want := range map[string]string{
		`a b`:      `"a b"`,
		`say "x"`:  `'say "x"'`,
		`it's "x"`: "`it's \"x\"`",
		"a\n  <x":  `"a   <x"`,
	} {
		if got := quote(in); got != want {
			t.Errorf("quote(%q) = %s, want %s", in, got, want)
		}
	}
}

func TestMarkerEnd(t *testing.T) {
	p := logicx.ProjectData{
		Markers:   []logicx.Marker{{Position: 100, Length: 50}, {Position: 200, Length: 1}, {Position: 300, Length: 1}},
		Transport: &logicx.Transport{End: 1000},
	}
	for i, want := range []uint32{150, 300, 1000} {
		if got := markerEnd(p, i); got != want {
			t.Errorf("markerEnd(%d) = %d, want %d", i, got, want)
		}
	}
}

func TestLoopedAudioItem(t *testing.T) {
	tl := timeline{origin: barOneTick, bpm: 120}
	f := &logicx.AudioFile{Name: "a.wav", SampleRate: 1000}
	r := logicx.AudioRegion{Name: "r", Offset: 2000, Frames: 1000} // seconds 2-3 of the file
	a := logicx.AudioPlacement{Position: barOneTick}
	if got := audioItem(a, r, f, "/media", tl); !strings.Contains(got, "SOFFS 2\n") ||
		strings.Contains(got, "SECTION") {
		t.Errorf("unlooped item: %s", got)
	}
	a.Loop = 8 * ticksPerQuarter // 4 s: the region four times
	got := audioItem(a, r, f, "/media", tl)
	for _, want := range []string{"LENGTH 4\n", "LOOP 1\n", "<SOURCE SECTION\n", "LENGTH 1\n", "STARTPOS 2\n"} {
		if !strings.Contains(got, want) {
			t.Errorf("looped item lacks %q: %s", want, got)
		}
	}
	if strings.Contains(got, "SOFFS") {
		t.Errorf("looped item keeps its offset in the section, not SOFFS: %s", got)
	}
}

func TestDelayedSequenceOrigin(t *testing.T) {
	// A sequence at bar one played early: zero moves back a bar so its start
	// isn't clamped and its notes stay inside the item.
	s := logicx.MIDISequence{Position: barOneTick, Duration: 960, Delay: -120, Track: "t", Row: 1}
	tl := newTimeline(logicx.Alternative{Project: logicx.ProjectData{Sequences: []logicx.MIDISequence{s}}})
	start := sequenceStart(s)
	if start < tl.origin {
		t.Fatalf("origin %d after the delayed start %d", tl.origin, start)
	}
	if got := tl.seconds(start+s.Duration) - tl.seconds(start); math.Abs(got-0.5) > 1e-9 {
		t.Errorf("item length %v, want 0.5", got)
	}
}

func TestCoincidentTempoChanges(t *testing.T) {
	// The last of two changes on one tick holds, for seconds and the envelope.
	tl := newTimeline(logicx.Alternative{Project: logicx.ProjectData{TempoChanges: []logicx.TempoChange{
		{Position: barOneTick, BPM: 120},
		{Position: barOneTick + 960, BPM: 60},
		{Position: barOneTick + 960, BPM: 120},
	}}})
	if got := tl.seconds(barOneTick + 1920); math.Abs(got-1) > 1e-9 {
		t.Errorf("seconds = %v, want 1", got)
	}
	if got := tl.tempoAt(barOneTick + 960); got != 120 {
		t.Errorf("tempoAt = %v, want 120", got)
	}
}
