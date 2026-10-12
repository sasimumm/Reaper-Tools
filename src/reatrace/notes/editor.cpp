#include "notes/editor.h"

#include <algorithm>
#include <cctype>

// Hit zones from DrawScore: note rects are inflated by 3 px, 6 px edge zones resize,
// notes narrower than 12 px can only be moved, and a drag starts after 3 px.
static const double INFLATE = 3, EDGE = 6, MIN_EDGE_WIDTH = 12, DRAG_THRESHOLD = 3;
// Shift while moving locks to the axis the drag mostly went along once it passes this (DrawScribe).
static const double AXIS_LOCK_THRESHOLD = 10;

NoteEditor::Hit NoteEditor::hitTest(const ViewMap &v, double x, double y, int *noteIndex) const {
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; i--) // last drawn is on top
    {
        const Note &n = notes[i];
        const double x0 = v.x(n.start), x1 = v.x(n.end), y0 = v.y(n.pitch + 0.5),
                     y1 = v.y(n.pitch - 0.5);
        if (x < x0 - INFLATE || x > x1 + INFLATE || y < y0 - INFLATE || y > y1 + INFLATE) {
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
        notes[i].sel = !notes[i].sel;
        changed = true;
        mode = IDLE;
        return;
    }
    if (!notes[i].sel) {
        for (Note &n : notes) {
            n.sel = false;
        }
        notes[i].sel = true;
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

    if (mode == PENDING_DRAW && past) {
        for (Note &n : notes) {
            n.sel = false;
        }
        const double t = sn(v.t(downX));
        notes.push_back({t, t + minLen, std::max(0, std::min(127, v.pitchAt(downY))), true, -1});
        drawIndex = static_cast<int>(notes.size()) - 1;
        mode = DRAW;
    }
    if (mode == PENDING_DRAG && past) {
        mode = dragHit == LEFT ? RESIZE_L : dragHit == RIGHT ? RESIZE_R : MOVE;
    }

    const double dtRaw = v.t(x) - v.t(downX);
    if (mode == DRAW) {
        Note &n = notes[drawIndex];
        const double a = sn(v.t(downX)), b = sn(v.t(x));
        n.start = std::min(a, b);
        n.end = std::max(std::max(a, b), n.start + minLen);
    } else if (mode == MOVE) {
        const double ax = fabs(x - downX), ay = fabs(y - downY);
        if (!(mods & MOD_SHIFT)) {
            axis = AXIS_FREE;
        } else if (axis == AXIS_FREE && (ax > AXIS_LOCK_THRESHOLD || ay > AXIS_LOCK_THRESHOLD)) {
            axis = ax >= ay ? AXIS_TIME : AXIS_PITCH;
        }
        const Note &a = orig[anchor];
        const double dt = axis == AXIS_PITCH ? 0 : sn(a.start + dtRaw) - a.start;
        const int dp = axis == AXIS_TIME ? 0 : v.pitchAt(y) - v.pitchAt(downY);
        for (size_t i = 0; i < notes.size(); i++) {
            if (orig[i].sel) {
                notes[i].start = orig[i].start + dt;
                notes[i].end = orig[i].end + dt;
                notes[i].pitch = std::max(0, std::min(127, orig[i].pitch + dp));
            }
        }
    } else if (mode == RESIZE_R || mode == RESIZE_L) {
        const Note &a = orig[anchor];
        const double edge = mode == RESIZE_R ? a.end : a.start, dt = sn(edge + dtRaw) - edge;
        for (size_t i = 0; i < notes.size(); i++) {
            if (orig[i].sel) {
                if (mode == RESIZE_R) {
                    notes[i].end = std::max(orig[i].start + minLen, orig[i].end + dt);
                } else {
                    notes[i].start = std::min(orig[i].end - minLen, orig[i].start + dt);
                }
            }
        }
    } else if (mode == MARQUEE) {
        double r[4];
        marquee(r);
        for (size_t i = 0; i < notes.size(); i++) {
            const Note &n = notes[i];
            const bool in = v.x(n.end) >= r[0] && v.x(n.start) <= r[2] &&
                            v.y(n.pitch - 0.5) >= r[1] && v.y(n.pitch + 0.5) <= r[3];
            notes[i].sel = in || (cmdMarquee && orig[i].sel);
        }
    }
}

bool NoteEditor::mouseUp(const ViewMap &v, double x, double y, int mods) {
    if (mode != IDLE) {
        mouseMove(v, x, y, mods);
    }
    if (mode == PENDING_DRAW) { // click on empty space
        for (Note &n : notes) {
            changed |= n.sel;
            n.sel = false;
        }
    } else if (mode == PENDING_DRAG) { // click on an already selected note: select just it
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
        if (notes[i].sel) {
            if (notes[i].idx >= 0) {
                deleted.push_back(notes[i].idx);
            }
            notes.erase(notes.begin() + i);
            any = true;
        }
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
    if (notes[i].idx >= 0) {
        deleted.push_back(notes[i].idx);
    }
    notes.erase(notes.begin() + i);
    return true;
}

std::vector<Note> NoteEditor::copySelected() const {
    std::vector<Note> out;
    for (const Note &n : notes) {
        if (n.sel) {
            out.push_back({n.start, n.end, n.pitch, true, -1, n.lyric});
        }
    }
    return out;
}

void NoteEditor::paste(const std::vector<Note> &clip, double at) {
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
    double a = clip[0].start, b = clip[0].end;
    for (const Note &n : clip) {
        a = std::min(a, n.start);
        b = std::max(b, n.end);
    }
    paste(clip, b);
    return true;
}

bool NoteEditor::nudgeSelected(const std::function<double(double)> &move, int semitones) {
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
        if (n.pitch < 0 || n.pitch > 127 || n.start < 0) {
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
    const size_t count = notes.size();
    for (size_t i = 0; i < count; i++) {
        if (note >= 0 ? static_cast<int>(i) != note : !notes[i].sel) {
            continue;
        }
        if (t < notes[i].start + minLen || t > notes[i].end - minLen) {
            continue;
        }
        Note second = {
            t, notes[i].end, notes[i].pitch, notes[i].sel, -1
        }; // the lyric stays on the first half
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
              [&](int a, int b) { return notes[a].start < notes[b].start; });
    Note &first = notes[sel[0]];
    // lyrics are joined, dropping the hyphens between syllables: "hel-" + "lo" -> "hello"
    for (size_t k = 1; k < sel.size(); k++) {
        const Note &n = notes[sel[k]];
        first.end = std::max(first.end, n.end);
        if (n.lyric.empty()) {
            continue;
        }
        if (!first.lyric.empty() && first.lyric.back() == '-') {
            first.lyric.pop_back();
        }
        first.lyric += n.lyric;
    }
    std::sort(sel.begin() + 1, sel.end(), std::greater<int>());
    for (size_t k = 1; k < sel.size(); k++) {
        if (notes[sel[k]].idx >= 0) {
            deleted.push_back(notes[sel[k]].idx);
        }
        notes.erase(notes.begin() + sel[k]);
    }
    return true;
}

bool NoteEditor::marquee(double r[4]) const {
    if (mode != MARQUEE) {
        return false;
    }
    r[0] = std::min(downX, curX);
    r[1] = std::min(downY, curY);
    r[2] = std::max(downX, curX);
    r[3] = std::max(downY, curY);
    return true;
}

std::vector<std::string> splitSyllables(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        if (isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(cur), cur.clear();
            }
            continue;
        }
        cur += ch;
        if (ch == '-' && cur != "-") {
            out.push_back(cur), cur.clear();
        }
    }
    if (!cur.empty() && cur != "-") {
        out.push_back(cur);
    }
    return out;
}
