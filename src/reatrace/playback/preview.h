// Auditioning notes through the voice track.
#pragma once

#include "state.h"

void stopPreviews(bool onlyFinished);
void previewPitch(int pitch);
void previewDragged();
void previewDragStarted(int pitch); // a drag starts on a note already heard at this pitch
