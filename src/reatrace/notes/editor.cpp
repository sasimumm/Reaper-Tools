// Note editing gestures (draw, move, resize, marquee) and editing commands on a note list.
#include "notes/editor.h"

#include "index.h"
#include <algorithm>
#include <cctype>
#include <span>

// Hit zones, only inside the note (the space around it draws): 9 px edge zones resize, notes
// narrower than 18 px can only be moved, and a drag starts after 3 px.
static const double EDGE = 9, MIN_EDGE_WIDTH = 18, DRAG_THRESHOLD = 3;
// Shift while moving locks to the axis the drag mostly went along once it passes this (DrawScribe).
static const double AXIS_LOCK_THRESHOLD = 10;

// Keeps a pitch within MIDI's 0..127.
static int clampPitch(int pitch) { return std::max(0, std::min(127, pitch)); }

NoteEditor::Hit NoteEditor::hitTest(const ViewMap &v, double x, double y, int *noteIndex) const {
    // last drawn is on top
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; i--) {
        const Note &n = notes[idx(i)];
        const double x0 = v.x(n.start), x1 = v.x(n.end);
        const double y0 = v.y(n.pitch + 0.5), y1 = v.y(n.pitch - 0.5);
        if (x < x0 || x > x1 || y < y0 || y > y1) {
            continue;
        }
        if (noteIndex) {
            *noteIndex = i;
        }
        if (x1 - x0 < MIN_EDGE_WIDTH) {
            return BODY;
        }
        if (x < x0 + EDGE) {
            return LEFT;
        }
        if (x > x1 - EDGE) {
            return RIGHT;
        }
        return BODY;
    }
    return NONE;
}

void NoteEditor::mouseDown(const ViewMap &v, double x, double y, int mods) {
    downX = curX = x;
    downY = curY = y;
    changed = false;
    axis = AXIS_FREE;
    orig = notes;

    int i = -1;
    Hit h = hitTest(v, x, y, &i);
    if (h == NONE) {
        mode = (mods & MOD_SHIFT) ? MARQUEE : PENDING_DRAW;
        cmdMarquee = (mods & MOD_CMD) != 0;
        return;
    }
    if (mods & MOD_CMD) {
        notes[idx(i)].sel = !notes[idx(i)].sel;
        if (notes[idx(i)].sel) {
            lastLen = notes[idx(i)].end - notes[idx(i)].start;
        }
        changed = true;
        mode = IDLE;
        return;
    }

    // pressing an unselected note selects just it; a selected one keeps the selection (to drag it)
    if (!notes[idx(i)].sel) {
        for (Note &n : notes) {
            n.sel = false;
        }
        notes[idx(i)].sel = true;
        changed = true;
        orig = notes;
    }
    anchor = i;
    dragHit = h;
    mode = PENDING_DRAG;
}

void NoteEditor::mouseMove(const ViewMap &v, double x, double y, int mods) {
    curX = x;
    curY = y;
    moves++;
    const bool past = fabs(x - downX) >= DRAG_THRESHOLD || fabs(y - downY) >= DRAG_THRESHOLD;
    auto sn = [&](double t) { return (mods & MOD_ALT) ? t : snap(t); };

    // past the drag threshold, a pending gesture starts
    if (mode == PENDING_DRAW && past) {
        for (Note &n : notes) {
            n.sel = false;
        }
        const double t = sn(v.t(downX));
        notes.push_back({t, t + minLen, clampPitch(v.pitchAt(downY)), true, -1});
        drawIndex = static_cast<int>(notes.size()) - 1;
        mode = DRAW;
    }
    if (mode == PENDING_DRAG && past) {
        mode = dragHit == LEFT ? RESIZE_L : dragHit == RIGHT ? RESIZE_R : MOVE;
    }

    const double dtRaw = v.t(x) - v.t(downX); // seconds since mouse down, unsnapped
    if (mode == DRAW) {
        Note &n = notes[idx(drawIndex)];
        const double a = sn(v.t(downX));
        const double b = sn(v.t(x));
        n.start = std::min(a, b);
        n.end = std::max(std::max(a, b), n.start + minLen);
    } else if (mode == MOVE) {
        const double ax = fabs(x - downX), ay = fabs(y - downY);
        if (!(mods & MOD_SHIFT)) {
            axis = AXIS_FREE;
        } else if (axis == AXIS_FREE && (ax > AXIS_LOCK_THRESHOLD || ay > AXIS_LOCK_THRESHOLD)) {
            axis = ax >= ay ? AXIS_TIME : AXIS_PITCH;
        }
        // the anchor's start snaps; every selected note moves by the same amount
        const Note &a = orig[idx(anchor)];
        const double dt = axis == AXIS_PITCH ? 0 : sn(a.start + dtRaw) - a.start;
        const int dp = axis == AXIS_TIME ? 0 : v.pitchAt(y) - v.pitchAt(downY);
        for (size_t i = 0; i < notes.size(); i++) {
            if (orig[i].sel) {
                notes[i].start = orig[i].start + dt;
                notes[i].end = orig[i].end + dt;
                notes[i].pitch = clampPitch(orig[i].pitch + dp);
            }
        }
    } else if (mode == RESIZE_R || mode == RESIZE_L) {
        // the anchor's edge snaps; every selected note's edge moves by the same amount
        const Note &a = orig[idx(anchor)];
        const double edge = mode == RESIZE_R ? a.end : a.start;
        const double dt = sn(edge + dtRaw) - edge;
        for (size_t i = 0; i < notes.size(); i++) {
            if (!orig[i].sel) {
                continue;
            }
            if (mode == RESIZE_R) {
                notes[i].end = std::max(orig[i].start + minLen, orig[i].end + dt);
            } else {
                notes[i].start = std::min(orig[i].end - minLen, orig[i].start + dt);
            }
        }
    } else if (const std::optional<Box> r = marquee()) {
        // cmd+marquee adds to the selection made before it
        for (size_t i = 0; i < notes.size(); i++) {
            const Note &n = notes[i];
            const bool in = v.x(n.end) >= r->x0 && v.x(n.start) <= r->x1 &&
                            v.y(n.pitch - 0.5) >= r->y0 && v.y(n.pitch + 0.5) <= r->y1;
            notes[i].sel = in || (cmdMarquee && orig[i].sel);
        }
    }
}

bool NoteEditor::mouseUp(const ViewMap &v, double x, double y, int mods) {
    if (mode != IDLE) {
        mouseMove(v, x, y, mods);
    }

    if (mode == PENDING_DRAW) {
        // click on empty space
        for (Note &n : notes) {
            changed |= n.sel;
            n.sel = false;
        }
        if (lastLen > 0 && (clickPlaces || (mods & MOD_ALT))) {
            const double t = (mods & MOD_ALT) ? v.t(downX) : snap(v.t(downX));
            notes.push_back({t, t + lastLen, clampPitch(v.pitchAt(downY)), true, -1});
            changed = true;
        }
    } else if (mode == PENDING_DRAG) {
        // click on an already selected note: select just it
        for (size_t i = 0; i < notes.size(); i++) {
            const bool sel = static_cast<int>(i) == anchor;
            changed |= notes[i].sel != sel;
            notes[i].sel = sel;
        }
    } else if (mode != IDLE) {
        for (size_t i = 0; i < notes.size(); i++) {
            const Note &n = notes[i];
            if (i >= orig.size() || n.start != orig[i].start || n.end != orig[i].end ||
                n.pitch != orig[i].pitch || n.sel != orig[i].sel) {
                changed = true;
            }
        }
    }

    if (const int last = drawIndex >= 0 ? drawIndex : anchor; last >= 0) {
        lastLen = notes[idx(last)].end - notes[idx(last)].start;
    }
    mode = IDLE;
    anchor = drawIndex = -1;
    return changed;
}

void NoteEditor::cancel() {
    if (mode != IDLE) {
        notes = orig;
    }
    mode = IDLE;
    anchor = drawIndex = -1;
}

bool NoteEditor::deleteSelected() {
    bool any = false;
    for (size_t i = notes.size(); i-- > 0;) {
        if (!notes[i].sel) {
            continue;
        }
        if (notes[i].idx >= 0) {
            deleted.push_back(notes[i].idx);
        }
        notes.erase(notes.begin() + static_cast<std::ptrdiff_t>(i));
        any = true;
    }
    return any;
}

bool NoteEditor::selectAll(bool sel) {
    bool any = false;
    for (Note &n : notes) {
        any |= n.sel != sel;
        n.sel = sel;
    }
    return any;
}

bool NoteEditor::deleteAt(const ViewMap &v, double x, double y) {
    int i = -1;
    if (hitTest(v, x, y, &i) == NONE) {
        return false;
    }
    if (notes[idx(i)].idx >= 0) {
        deleted.push_back(notes[idx(i)].idx);
    }
    notes.erase(notes.begin() + i);
    return true;
}

std::vector<Note> NoteEditor::copySelected() const {
    std::vector<Note> out;
    for (const Note &n : notes) {
        if (n.sel) {
            Note c = n;
            c.sel = true;
            c.idx = c.lyricIdx = -1;
            out.push_back(c);
        }
    }
    return out;
}

void NoteEditor::paste(std::span<const Note> clip, double at) {
    if (clip.empty()) {
        return;
    }
    double first = clip[0].start;
    for (const Note &n : clip) {
        first = std::min(first, n.start);
    }
    for (Note &n : notes) {
        n.sel = false;
    }
    for (Note n : clip) {
        n.start += at - first;
        n.end += at - first;
        n.sel = true;
        n.idx = n.lyricIdx = -1;
        notes.push_back(n);
    }
}

bool NoteEditor::duplicateSelected() {
    const std::vector<Note> clip = copySelected();
    if (clip.empty()) {
        return false;
    }
    double end = clip[0].end;
    for (const Note &n : clip) {
        end = std::max(end, n.end);
    }
    paste(clip, end);
    return true;
}

bool NoteEditor::nudgeSelected(const std::function<double(double)> &move, int semitones) {
    // on a copy, so nothing changes if any note would go out of range
    std::vector<Note> out = notes;
    bool any = false;
    for (Note &n : out) {
        if (!n.sel) {
            continue;
        }
        n.start = move(n.start);
        n.end = move(n.end);
        n.pitch += semitones;
        any = true;
        if (n.pitch < 0 || n.pitch > 127 || n.start < 0 || !(n.end > n.start)) {
            return false;
        }
    }
    if (any) {
        notes = std::move(out);
    }
    return any;
}

bool NoteEditor::splitAt(double t, int note) {
    bool any = false;
    const size_t count = notes.size(); // not the halves added on the way
    for (size_t i = 0; i < count; i++) {
        const bool wanted = note >= 0 ? static_cast<int>(i) == note : notes[i].sel;
        if (!wanted || t < notes[i].start + minLen || t > notes[i].end - minLen) {
            continue;
        }
        // the lyric stays on the first half; velocity, channel and mute go to both
        Note second = notes[i];
        second.start = t;
        second.idx = second.lyricIdx = -1;
        second.lyric.clear();
        notes[i].end = t;
        notes.push_back(second);
        any = true;
    }
    return any;
}

bool NoteEditor::joinSelected() {
    std::vector<int> sel;
    for (size_t i = 0; i < notes.size(); i++) {
        if (notes[i].sel) {
            sel.push_back(static_cast<int>(i));
        }
    }
    if (sel.size() < 2) {
        return false;
    }
    std::sort(sel.begin(), sel.end(),
              [&](int a, int b) { return notes[idx(a)].start < notes[idx(b)].start; });

    Note &first = notes[idx(sel[0])];
    // lyrics are joined, dropping the hyphens between syllables: "hel-" + "lo" -> "hello"
    for (size_t k = 1; k < sel.size(); k++) {
        const Note &n = notes[idx(sel[k])];
        first.end = std::max(first.end, n.end);
        if (n.lyric.empty()) {
            continue;
        }
        if (!first.lyric.empty() && first.lyric.back() == '-') {
            first.lyric.pop_back();
        }
        first.lyric += n.lyric;
    }

    // remove the rest, highest index first so the others stay valid
    std::sort(sel.begin() + 1, sel.end(), std::greater<int>());
    for (size_t k = 1; k < sel.size(); k++) {
        if (notes[idx(sel[k])].idx >= 0) {
            deleted.push_back(notes[idx(sel[k])].idx);
        }
        notes.erase(notes.begin() + sel[k]);
    }
    return true;
}

std::optional<NoteEditor::Box> NoteEditor::marquee() const {
    if (mode != MARQUEE) {
        return std::nullopt;
    }
    return Box{
        std::min(downX, curX), std::min(downY, curY), std::max(downX, curX), std::max(downY, curY)
    };
}

std::vector<std::string> splitSyllables(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        if (isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += ch;
        // a lone "-" isn't a syllable: it's kept to start the next one
        if (ch == '-' && cur != "-") {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty() && cur != "-") {
        out.push_back(cur);
    }
    return out;
}
