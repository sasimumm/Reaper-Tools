#include "editor.h"

#include <algorithm>

// Hit zones from DrawScore: note rects are inflated by 3 px, 6 px edge zones resize,
// notes narrower than 12 px can only be moved, and a drag starts after 3 px.
static const double INFLATE = 3, EDGE = 6, MIN_EDGE_WIDTH = 12, DRAG_THRESHOLD = 3;

NoteEditor::Hit NoteEditor::hitTest(const ViewMap &v, double x, double y, int *noteIndex) const
{
  for (int i = (int)notes.size() - 1; i >= 0; i--) // last drawn is on top
  {
    const Note &n = notes[i];
    const double x0 = v.x(n.start), x1 = v.x(n.end), y0 = v.y(n.pitch + 0.5), y1 = v.y(n.pitch - 0.5);
    if (x < x0 - INFLATE || x > x1 + INFLATE || y < y0 - INFLATE || y > y1 + INFLATE) continue;
    if (noteIndex) *noteIndex = i;
    if (x1 - x0 < MIN_EDGE_WIDTH) return BODY;
    if (x < x0 + EDGE) return LEFT;
    if (x > x1 - EDGE) return RIGHT;
    return BODY;
  }
  return NONE;
}

void NoteEditor::mouseDown(const ViewMap &v, double x, double y, int mods)
{
  downX = curX = x, downY = curY = y;
  changed = false;
  orig = notes;
  int i = -1;
  Hit h = hitTest(v, x, y, &i);
  if (h == NONE)
  {
    mode = (mods & MOD_SHIFT) ? MARQUEE : PENDING_DRAW;
    cmdMarquee = (mods & MOD_CMD) != 0;
    return;
  }
  if (mods & MOD_CMD)
  {
    notes[i].sel = !notes[i].sel;
    changed = true;
    mode = IDLE;
    return;
  }
  if (!notes[i].sel)
  {
    for (Note &n : notes) n.sel = false;
    notes[i].sel = true;
    changed = true;
    orig = notes;
  }
  anchor = i, dragHit = h, mode = PENDING_DRAG;
}

void NoteEditor::mouseMove(const ViewMap &v, double x, double y, int mods)
{
  curX = x, curY = y;
  const bool past = fabs(x - downX) >= DRAG_THRESHOLD || fabs(y - downY) >= DRAG_THRESHOLD;
  auto sn = [&](double t) { return (mods & MOD_ALT) ? t : snap(t); };

  if (mode == PENDING_DRAW && past)
  {
    for (Note &n : notes) n.sel = false;
    const double t = sn(v.t(downX));
    notes.push_back({t, t + minLen, std::max(0, std::min(127, v.pitchAt(downY))), true, -1});
    drawIndex = (int)notes.size() - 1;
    mode = DRAW;
  }
  if (mode == PENDING_DRAG && past) mode = dragHit == LEFT ? RESIZE_L : dragHit == RIGHT ? RESIZE_R : MOVE;

  const double dtRaw = v.t(x) - v.t(downX);
  if (mode == DRAW)
  {
    Note &n = notes[drawIndex];
    const double a = sn(v.t(downX)), b = sn(v.t(x));
    n.start = std::min(a, b);
    n.end = std::max(std::max(a, b), n.start + minLen);
  }
  else if (mode == MOVE)
  {
    const Note &a = orig[anchor];
    const double dt = sn(a.start + dtRaw) - a.start;
    const int dp = v.pitchAt(y) - v.pitchAt(downY);
    for (size_t i = 0; i < notes.size(); i++)
      if (orig[i].sel)
      {
        notes[i].start = orig[i].start + dt;
        notes[i].end = orig[i].end + dt;
        notes[i].pitch = std::max(0, std::min(127, orig[i].pitch + dp));
      }
  }
  else if (mode == RESIZE_R || mode == RESIZE_L)
  {
    const Note &a = orig[anchor];
    const double edge = mode == RESIZE_R ? a.end : a.start, dt = sn(edge + dtRaw) - edge;
    for (size_t i = 0; i < notes.size(); i++)
      if (orig[i].sel)
      {
        if (mode == RESIZE_R) notes[i].end = std::max(orig[i].start + minLen, orig[i].end + dt);
        else notes[i].start = std::min(orig[i].end - minLen, orig[i].start + dt);
      }
  }
  else if (mode == MARQUEE)
  {
    double r[4];
    marquee(r);
    for (size_t i = 0; i < notes.size(); i++)
    {
      const Note &n = notes[i];
      const bool in = v.x(n.end) >= r[0] && v.x(n.start) <= r[2] && v.y(n.pitch - 0.5) >= r[1] && v.y(n.pitch + 0.5) <= r[3];
      notes[i].sel = in || (cmdMarquee && orig[i].sel);
    }
  }
}

bool NoteEditor::mouseUp(const ViewMap &v, double x, double y, int mods)
{
  if (mode != IDLE) mouseMove(v, x, y, mods);
  if (mode == PENDING_DRAW) // click on empty space
    for (Note &n : notes) changed |= n.sel, n.sel = false;
  else if (mode == PENDING_DRAG) // click on an already selected note: select just it
    for (size_t i = 0; i < notes.size(); i++)
    {
      const bool sel = (int)i == anchor;
      changed |= notes[i].sel != sel;
      notes[i].sel = sel;
    }
  else if (mode != IDLE)
    for (size_t i = 0; i < notes.size(); i++)
    {
      const Note &n = notes[i];
      if (i >= orig.size() || n.start != orig[i].start || n.end != orig[i].end || n.pitch != orig[i].pitch ||
          n.sel != orig[i].sel)
        changed = true;
    }
  mode = IDLE, anchor = drawIndex = -1;
  return changed;
}

void NoteEditor::cancel()
{
  if (mode != IDLE) notes = orig;
  mode = IDLE, anchor = drawIndex = -1;
}

bool NoteEditor::deleteSelected()
{
  bool any = false;
  for (size_t i = notes.size(); i-- > 0;)
    if (notes[i].sel)
    {
      if (notes[i].idx >= 0) deleted.push_back(notes[i].idx);
      notes.erase(notes.begin() + i);
      any = true;
    }
  return any;
}

bool NoteEditor::selectAll(bool sel)
{
  bool any = false;
  for (Note &n : notes) any |= n.sel != sel, n.sel = sel;
  return any;
}

bool NoteEditor::deleteAt(const ViewMap &v, double x, double y)
{
  int i = -1;
  if (hitTest(v, x, y, &i) == NONE) return false;
  if (notes[i].idx >= 0) deleted.push_back(notes[i].idx);
  notes.erase(notes.begin() + i);
  return true;
}

bool NoteEditor::marquee(double r[4]) const
{
  if (mode != MARQUEE) return false;
  r[0] = std::min(downX, curX), r[1] = std::min(downY, curY);
  r[2] = std::max(downX, curX), r[3] = std::max(downY, curY);
  return true;
}
