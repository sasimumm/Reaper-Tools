// Auditioning notes through the voice track.
#pragma once

#include "state.h"

void stopPreviews(bool onlyFinished); // all, or only those that have had time to finish
void previewPitch(int pitch);         // plays a short note, if auditioning is on
void previewDragged();              // while dragging: plays the dragged note when its pitch changes
void previewDragStarted(int pitch); // a drag starts on a note already heard at this pitch
