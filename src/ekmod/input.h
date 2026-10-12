#pragma once
#include <windows.h>
#include "freecam.h"

// Subclass the game's window for panel input. Idempotent.
bool input_install(HWND wnd);

void input_remove(void);

// Window thread writes, render thread reads: interlocked scalars only.
bool input_panel_open(void);
void input_mouse_pos(float* x, float* y);

// Cursor instrument (Debug log only); each call zeroes the counts.
void input_debug_take(int* rawSeen, int* rawLive, int* rawCount, int* moveCount);

// This tic's camera input from interlocked scalars. Mouse deltas are DRAINED: false, with *out zeroed, when the camera
// does not own input (drained even then).
bool input_freecam_state(FreecamInput* out);

// Toggle-key edges since the last call (zeroes); recorded only while the free camera row is on.
int input_freecam_toggle_take(void);

int input_teleport_take(void);
