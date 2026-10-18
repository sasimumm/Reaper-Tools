// Command logicx-to-reaper converts a Logic Pro project into a REAPER project:
// tracks with their fader, pan, mute and solo, MIDI regions as MIDI items,
// audio regions as items playing their files, the tempo map, time signatures
// and markers. Plug-ins, sends, automation and chords are not converted.
package main

import (
	"cmp"
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"
	"unicode"

	"github.com/loov/logicx"
)

const (
	// ticksPerQuarter is Logic's resolution, which the MIDI items keep.
	ticksPerQuarter = 960
	// barOneTick is the Logic position of bar 1.
	barOneTick = 40 * ticksPerQuarter
)

func main() {
	output := flag.String("o", "", "REAPER project to write (default: beside the project, with .rpp)")
	alternative := flag.String("alternative", "", "project alternative (default: first)")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: logicx-to-reaper [-o song.rpp] [-alternative name] song.logicx")
		os.Exit(2)
	}
	project := filepath.Clean(flag.Arg(0))
	out := *output
	if out == "" {
		out = strings.TrimSuffix(project, filepath.Ext(project)) + ".rpp"
	}
	if err := convert(project, *alternative, out); err != nil {
		fmt.Fprintln(os.Stderr, "logicx-to-reaper:", err)
		os.Exit(1)
	}
}

func convert(project, name, out string) error {
	bundle, err := logicx.OpenBundle(project)
	if err != nil {
		return err
	}
	alt, err := chooseAlternative(bundle.Alternatives, name)
	if err != nil {
		return err
	}
	media, err := filepath.Abs(filepath.Join(project, "Media"))
	if err != nil {
		return err
	}
	return os.WriteFile(out, []byte(render(alt, media)), 0o644)
}

// chooseAlternative returns the named alternative, or the first one when no
// name is given. OpenBundle guarantees at least one alternative.
func chooseAlternative(alternatives []logicx.Alternative, name string) (logicx.Alternative, error) {
	if name == "" {
		return alternatives[0], nil
	}
	for _, alt := range alternatives {
		if alt.Name == name {
			return alt, nil
		}
	}
	return logicx.Alternative{}, fmt.Errorf("alternative %q not found", name)
}

// timeline turns Logic positions into REAPER seconds, from origin at zero.
type timeline struct {
	origin uint32
	bpm    float64 // tempo before the first change
	tempos []logicx.TempoChange
}

func newTimeline(alt logicx.Alternative) timeline {
	p := alt.Project
	tl := timeline{bpm: cmp.Or(alt.Metadata.BPM, 120), tempos: slices.Clone(p.TempoChanges)}
	slices.SortStableFunc(tl.tempos, func(a, b logicx.TempoChange) int { return cmp.Compare(a.Position, b.Position) })
	if len(tl.tempos) > 0 {
		tl.bpm = tl.tempos[0].BPM
	}

	// REAPER has no time before zero, so content before bar 1 moves zero
	// back by whole bars, keeping the bar lines where they were.
	earliest := uint32(barOneTick)
	for _, s := range p.Sequences {
		if !chordHolder(s) {
			earliest = min(earliest, s.Position)
		}
	}
	for _, a := range p.AudioPlacements {
		earliest = min(earliest, a.Position)
	}
	for _, m := range p.Markers {
		earliest = min(earliest, m.Position)
	}
	for _, c := range p.ProjectChords {
		earliest = min(earliest, c.Position)
	}
	bar := uint32(4 * ticksPerQuarter)
	if len(p.TimeSignatures) > 0 && p.TimeSignatures[0].Denominator > 0 {
		s := p.TimeSignatures[0]
		bar = max(uint32(s.Numerator)*4*ticksPerQuarter/uint32(s.Denominator), 1)
	}
	bars := (barOneTick - earliest + bar - 1) / bar
	tl.origin = barOneTick - min(bars*bar, barOneTick)
	return tl
}

// chordHolder reports whether s only holds project chords: such sequences sit
// on no track, at position 0, and carry no MIDI.
func chordHolder(s logicx.MIDISequence) bool {
	return s.Row == 0 && s.Track == "" && len(s.Notes) == 0 && len(s.Controls) == 0
}

// tempoAt is the tempo in effect at tick.
func (tl timeline) tempoAt(tick uint32) float64 {
	bpm := tl.bpm
	for _, c := range tl.tempos {
		if c.Position > tick {
			break
		}
		bpm = c.BPM
	}
	return bpm
}

// seconds walks the whole tempo map per call; index it if long maps get slow.
func (tl timeline) seconds(tick uint32) float64 {
	tick = max(tick, tl.origin)
	pos, bpm := tl.origin, tl.tempoAt(tl.origin)
	var s float64
	for _, c := range tl.tempos {
		if c.Position <= pos {
			continue
		}
		if c.Position >= tick {
			break
		}
		s += float64(c.Position-pos) / ticksPerQuarter * 60 / bpm
		pos, bpm = c.Position, c.BPM
	}
	return s + float64(tick-pos)/ticksPerQuarter*60/bpm
}

// track is one REAPER track and the items rendered for it.
type track struct {
	name   string
	strip  *logicx.Track
	items  []string
	chords *track // the track below it holding its regions' chords, if any
}

func render(alt logicx.Alternative, media string) string {
	p := alt.Project
	tl := newTimeline(alt)

	strips := map[string]*logicx.Track{}
	for i := range p.Tracks {
		strips[p.Tracks[i].Name] = &p.Tracks[i]
	}
	var tracks []*track
	byRow := map[int]*track{}
	byName := map[string]*track{}
	for _, a := range p.ArrangeTracks {
		if a.Output {
			continue
		}
		t := &track{name: a.Track, strip: strips[a.Track]}
		if t.strip != nil && t.strip.TrackName != "" {
			t.name = t.strip.TrackName
		}
		tracks = append(tracks, t)
		byRow[a.Row] = t
		if byName[a.Track] == nil {
			byName[a.Track] = t
		}
	}
	trackFor := func(row int, name string) *track {
		if t := byRow[row]; t != nil {
			return t
		}
		if t := byName[name]; t != nil {
			return t
		}
		t := &track{name: cmp.Or(name, "Unassigned"), strip: strips[name]}
		tracks = append(tracks, t)
		byName[name] = t
		return t
	}

	for _, s := range p.Sequences {
		if chordHolder(s) {
			continue
		}
		t := trackFor(s.Row, s.Track)
		end := s.Position + s.Duration
		chords := namedChords(s.Chords)
		if len(s.Notes) == 0 && len(s.Controls) == 0 && len(chords) > 0 {
			// A region of only chords becomes just a chord item.
			t.items = append(t.items, chordItem(s.Name, chords, s.Position, end, tl))
			continue
		}
		t.items = append(t.items, midiItem(s, tl))
		if len(chords) > 0 {
			if t.chords == nil {
				t.chords = &track{name: t.name + " Chords"}
				tracks = slices.Insert(tracks, slices.Index(tracks, t)+1, t.chords)
			}
			t.chords.items = append(t.chords.items, chordItem(s.Name, chords, s.Position, end, tl))
		}
	}
	if chords := namedChords(p.ProjectChords); len(chords) > 0 {
		var end uint32
		for _, c := range chords {
			end = max(end, c.Position+c.Duration)
		}
		start := slices.MinFunc(chords, func(a, b logicx.Chord) int { return cmp.Compare(a.Position, b.Position) }).Position
		t := &track{name: "Chords", items: []string{chordItem("Chords", chords, start, end, tl)}}
		tracks = slices.Insert(tracks, 0, t)
	}
	files, ok := regionFiles(p)
	if !ok && len(p.AudioPlacements) > 0 {
		fmt.Fprintln(os.Stderr, "logicx-to-reaper: cannot match audio regions to their files; audio skipped")
	}
	for _, a := range p.AudioPlacements {
		if !ok || a.Region < 0 || a.Region >= len(p.AudioRegions) {
			continue
		}
		file := files[a.Region]
		t := trackFor(0, a.Track)
		t.items = append(t.items, audioItem(a, p.AudioRegions[a.Region], file, media, tl))
	}

	var b strings.Builder
	fmt.Fprintf(&b, "<REAPER_PROJECT 0.1 \"7.0/logicx-to-reaper\" %d 0\n", time.Now().Unix())
	if rate := sampleRate(alt); rate > 0 {
		fmt.Fprintf(&b, "  SAMPLERATE %d 1 0\n", rate)
	}
	num, den := uint64(4), uint64(4)
	if ts := alt.Metadata.TimeSignature; ts[0] > 0 && ts[1] > 0 {
		num, den = ts[0], ts[1]
	}
	if len(p.TimeSignatures) > 0 {
		num, den = uint64(p.TimeSignatures[0].Numerator), uint64(p.TimeSignatures[0].Denominator)
	}
	fmt.Fprintf(&b, "  TEMPO %s %d %d\n", fnum(tl.tempoAt(tl.origin)), num, den)
	writeTempoEnvelope(&b, p, tl)
	for i, m := range p.Markers {
		start := tl.seconds(m.Position)
		if end := markerEnd(p, i); end > m.Position {
			fmt.Fprintf(&b, "  MARKER %d %s %s 1\n", i+1, fnum(start), quote(m.Name))
			fmt.Fprintf(&b, "  MARKER %d %s \"\" 1\n", i+1, fnum(tl.seconds(end)))
		} else {
			fmt.Fprintf(&b, "  MARKER %d %s %s 0\n", i+1, fnum(start), quote(m.Name))
		}
	}
	for _, t := range tracks {
		b.WriteString("  <TRACK\n")
		fmt.Fprintf(&b, "    NAME %s\n", quote(t.name))
		if s := t.strip; s != nil {
			volume := math.Pow(10, s.VolumeDB()/20) // -Inf dB gives 0
			pan := max(-1, min(1, float64(s.Pan)/64))
			fmt.Fprintf(&b, "    VOLPAN %s %s -1\n", fnum(volume), fnum(pan))
			fmt.Fprintf(&b, "    MUTESOLO %d %d 0\n", bit(s.Mute), bit(s.Solo))
		}
		for _, item := range t.items {
			b.WriteString(item)
		}
		b.WriteString("  >\n")
	}
	b.WriteString(">\n")
	return b.String()
}

func sampleRate(alt logicx.Alternative) uint64 {
	if t := alt.Project.Transport; t != nil && t.SampleRate > 0 {
		return uint64(t.SampleRate)
	}
	return alt.Metadata.SampleRate
}

// markerEnd returns where p.Markers[i] ends. Logic stores a length of 1 for a
// marker that runs to the next marker, or for the last one to the project end.
func markerEnd(p logicx.ProjectData, i int) uint32 {
	m := p.Markers[i]
	if m.Length > 1 {
		return m.Position + m.Length
	}
	if i+1 < len(p.Markers) {
		return p.Markers[i+1].Position
	}
	if p.Transport != nil {
		return p.Transport.End
	}
	return m.Position
}

// writeTempoEnvelope writes the tempo map, Logic's ramps already sampled into
// steps, with the time signatures on the same points.
func writeTempoEnvelope(b *strings.Builder, p logicx.ProjectData, tl timeline) {
	signatures := map[uint32]logicx.TimeSignatureChange{}
	var positions []uint32
	for _, c := range tl.tempos {
		positions = append(positions, max(c.Position, tl.origin))
	}
	for _, s := range p.TimeSignatures {
		at := max(s.Position, tl.origin)
		signatures[at] = s
		positions = append(positions, at)
	}
	slices.Sort(positions)
	positions = slices.Compact(positions)
	if len(positions) == 0 {
		return
	}
	b.WriteString("  <TEMPOENVEX\n    ACT 1 -1\n    VIS 1 0 1\n    DEFSHAPE 1 -1 -1\n")
	for _, at := range positions {
		fmt.Fprintf(b, "    PT %s %s 1", fnum(tl.seconds(at)), fnum(tl.tempoAt(at)))
		if s, ok := signatures[at]; ok && s.Numerator > 0 && s.Denominator > 0 {
			fmt.Fprintf(b, " %d 0 1", 65536*int(s.Denominator)+int(s.Numerator))
		}
		b.WriteString("\n")
	}
	b.WriteString("  >\n")
}

// namedChords returns the chords ReaChords can show, "no chord" as N.C.
// Scales are left out, being no chord, as are chords logicx cannot name.
func namedChords(chords []logicx.Chord) []logicx.Chord {
	var named []logicx.Chord
	for _, c := range chords {
		if c.NoChord {
			c.Name = "N.C."
		}
		if !c.Scale && c.Name != "" {
			named = append(named, c)
		}
	}
	return named
}

// chordItem is a ReaChords item from start to end: its source lists each chord
// by quarter notes from the item's start, and the item follows tempo, as
// ReaChords' own items do.
func chordItem(name string, chords []logicx.Chord, start, end uint32, tl timeline) string {
	begin := tl.seconds(start)
	length := tl.seconds(end) - begin

	var b strings.Builder
	b.WriteString("    <ITEM\n")
	fmt.Fprintf(&b, "      POSITION %s\n", fnum(begin))
	fmt.Fprintf(&b, "      LENGTH %s\n", fnum(length))
	b.WriteString("      LOOP 0\n      BEAT 1\n")
	fmt.Fprintf(&b, "      NAME %s\n", quote(name))
	// 3600 s is what ReaChords gives a new item: the longest it can stretch.
	fmt.Fprintf(&b, "      <SOURCE REACHORDS\n        LENGTH %s\n", fnum(max(3600, length)))
	for _, c := range chords {
		if c.Position >= start {
			fmt.Fprintf(&b, "        BEAT %s %s\n", fnum(float64(c.Position-start)/ticksPerQuarter), oneLine(c.Name))
		}
	}
	b.WriteString("      >\n    >\n")
	return b.String()
}

// midiEvent is one message of a MIDI item. Order breaks ties at a tick.
type midiEvent struct {
	tick  int64
	order int
	muted bool
	msg   [3]byte
}

func midiItem(s logicx.MIDISequence, tl timeline) string {
	start := uint32(max(int64(s.Position)+int64(s.Delay), 0))
	begin := tl.seconds(start)

	var events []midiEvent
	for _, n := range s.Notes {
		pitch := int(n.Pitch) + int(s.Transpose)
		if n.Duration == 0 || pitch < 0 || pitch > 127 || n.Channel < 1 {
			continue
		}
		at := max(int64(n.Position)-int64(s.Position), 0)
		velocity := byte(max(1, min(127, int(n.Velocity)+int(s.Velocity))))
		ch := n.Channel - 1
		// A note-off sorts before a note-on at the same tick, so a repeated
		// pitch is released before it sounds again.
		events = append(events,
			midiEvent{tick: at, order: 1, muted: n.Muted, msg: [3]byte{0x90 | ch, byte(pitch), velocity}},
			midiEvent{tick: at + int64(n.Duration), muted: n.Muted, msg: [3]byte{0x80 | ch, byte(pitch), min(n.ReleaseVelocity, 127)}})
	}
	for _, c := range s.Controls {
		if msg, ok := controlMessage(c); ok {
			events = append(events, midiEvent{tick: max(int64(c.Position)-int64(s.Position), 0), msg: msg})
		}
	}
	slices.SortStableFunc(events, func(a, b midiEvent) int {
		return cmp.Or(cmp.Compare(a.tick, b.tick), cmp.Compare(a.order, b.order))
	})

	var b strings.Builder
	b.WriteString("    <ITEM\n")
	fmt.Fprintf(&b, "      POSITION %s\n", fnum(begin))
	fmt.Fprintf(&b, "      LENGTH %s\n", fnum(tl.seconds(start+s.Duration)-begin))
	fmt.Fprintf(&b, "      LOOP 0\n      MUTE %d\n", bit(s.Mute))
	fmt.Fprintf(&b, "      NAME %s\n", quote(s.Name))
	b.WriteString("      <SOURCE MIDI\n        HASDATA 1 960 QN\n")
	writeMIDIEvents(&b, events, int64(s.Duration))
	b.WriteString("      >\n    >\n")
	return b.String()
}

// writeMIDIEvents writes events as REAPER's delta-timed lines, closed by an
// all-notes-off at length, which is where REAPER ends the source.
func writeMIDIEvents(b *strings.Builder, events []midiEvent, length int64) {
	var previous int64
	for _, e := range events {
		flag := "E"
		if e.muted {
			flag = "Em"
		}
		fmt.Fprintf(b, "        %s %d %02x %02x %02x\n", flag, e.tick-previous, e.msg[0], e.msg[1], e.msg[2])
		previous = e.tick
	}
	fmt.Fprintf(b, "        E %d b0 7b 00\n", max(length-previous, 0))
}

func controlMessage(c logicx.MIDIControl) ([3]byte, bool) {
	if c.Channel < 1 {
		return [3]byte{}, false
	}
	status := byte(c.Kind) | (c.Channel - 1)
	switch c.Kind {
	case logicx.ControlChange, logicx.PolyPressure:
		return [3]byte{status, c.Number & 0x7f, byte(min(c.Value, 127))}, true
	case logicx.ProgramChange, logicx.ChannelPressure:
		return [3]byte{status, byte(min(c.Value, 127)), 0}, true
	case logicx.PitchBend:
		v := min(c.Value, 1<<14-1)
		return [3]byte{status, byte(v & 0x7f), byte(v >> 7)}, true
	}
	return [3]byte{}, false
}

// regionFiles returns, for each of p.AudioRegions, its file. logicx keeps that
// link in the chunk headers, unexported, so this reads the headers itself and
// pairs them with the decoded values by order; it gives up when the counts
// disagree. Drop this once logicx exports AudioRegion's file.
func regionFiles(p logicx.ProjectData) ([]*logicx.AudioFile, bool) {
	const fileID = 10 // header offset of the file ID, in both chunk types
	var regionIDs, fileIDs []uint32
	for _, c := range p.Chunks {
		switch c.Type {
		case "AuRg":
			regionIDs = append(regionIDs, binary.LittleEndian.Uint32(c.Header[fileID:]))
		case "AuFl":
			fileIDs = append(fileIDs, binary.LittleEndian.Uint32(c.Header[fileID:]))
		}
	}
	if len(regionIDs) != len(p.AudioRegions) || len(fileIDs) != len(p.AudioFiles) {
		return nil, false
	}
	files := make([]*logicx.AudioFile, len(regionIDs))
	for i, id := range regionIDs {
		at := slices.Index(fileIDs, id)
		if at < 0 {
			return nil, false
		}
		files[i] = &p.AudioFiles[at]
	}
	return files, true
}

func audioItem(a logicx.AudioPlacement, r logicx.AudioRegion, f *logicx.AudioFile, media string, tl timeline) string {
	begin := tl.seconds(a.Position)
	rate := float64(cmp.Or(f.SampleRate, 44100))
	length, loop := float64(r.Frames)/rate, 0
	if a.Loop > 0 {
		length, loop = tl.seconds(a.Position+a.Loop)-begin, 1
	}

	var b strings.Builder
	b.WriteString("    <ITEM\n")
	fmt.Fprintf(&b, "      POSITION %s\n", fnum(begin))
	fmt.Fprintf(&b, "      LENGTH %s\n", fnum(length))
	fmt.Fprintf(&b, "      LOOP %d\n      MUTE %d\n", loop, bit(a.Mute))
	fmt.Fprintf(&b, "      FADEIN 1 %s 0\n", fnum(float64(a.FadeIn)/1000))
	fmt.Fprintf(&b, "      FADEOUT 1 %s 0\n", fnum(float64(a.FadeOut)/1000))
	fmt.Fprintf(&b, "      NAME %s\n", quote(r.Name))
	fmt.Fprintf(&b, "      VOLPAN 1 0 %s -1\n", fnum(math.Pow(10, float64(a.Gain)/20)))
	offset, file := float64(r.Offset)/rate, quote(audioPath(f, media))
	if loop == 0 {
		fmt.Fprintf(&b, "      SOFFS %s\n", fnum(offset))
		fmt.Fprintf(&b, "      <SOURCE WAVE\n        FILE %s\n      >\n    >\n", file)
		return b.String()
	}
	// A looped item repeats its whole source, so the source is cut to the region.
	fmt.Fprintf(&b, "      <SOURCE SECTION\n")
	fmt.Fprintf(&b, "        LENGTH %s\n        STARTPOS %s\n        OVERLAP 0\n",
		fnum(float64(r.Frames)/rate), fnum(offset))
	fmt.Fprintf(&b, "        <SOURCE WAVE\n          FILE %s\n        >\n      >\n    >\n", file)
	return b.String()
}

// audioPath resolves a file's folder, which Logic writes relative to the
// bundle's Media folder or as a "~/" path.
func audioPath(f *logicx.AudioFile, media string) string {
	dir := f.Dir
	switch {
	case strings.HasPrefix(dir, "~/"):
		if home, err := os.UserHomeDir(); err == nil {
			dir = filepath.Join(home, dir[2:])
		}
	case !filepath.IsAbs(dir):
		dir = filepath.Join(media, dir)
	}
	return filepath.Join(dir, f.Name)
}

// quote writes s as an RPP string: REAPER takes double quotes, single quotes
// or backticks, whichever s does not contain.
func quote(s string) string {
	s = oneLine(s)
	switch {
	case !strings.Contains(s, `"`):
		return `"` + s + `"`
	case !strings.Contains(s, "'"):
		return "'" + s + "'"
	}
	return "`" + strings.ReplaceAll(s, "`", "'") + "`"
}

// oneLine replaces control characters with spaces: a project line ends at a
// newline, so one in a name from the Logic project would start a line of its own.
func oneLine(s string) string {
	return strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return ' '
		}
		return r
	}, s)
}

func fnum(v float64) string { return fmt.Sprintf("%.10g", v) }

func bit(b bool) int {
	if b {
		return 1
	}
	return 0
}
