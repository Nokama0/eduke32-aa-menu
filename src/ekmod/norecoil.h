#pragma once
#include <stddef.h>

// AA's recoil kick is taken back one unit a tic (AAPLAYER.CON:12492-12499); this collapses it into one step.

inline int ekm_fix16_round(int q16) {
  const long long a = (long long)q16 + (q16 >= 0 ? 32768 : -32768);
  return (int)(a / 65536);
}

// Per tic: horiz = round(horiz) - 1, horizfall -= 1, zero once horiz <= savedhoriz. False when horizfall <= 0.
inline bool ekm_norecoil_collapse(int q16horiz, int horizfall, int savedhoriz,
                                  int* outQ16, int* outRemoved) {
  if (!outQ16 || !outRemoved || horizfall <= 0) return false;
  const int h = ekm_fix16_round(q16horiz);
  const long long gap = (long long)h - savedhoriz;
  long long n = gap < 1 ? 1 : gap;
  if (n > horizfall) n = horizfall;
  *outQ16 = (int)(((long long)h - n) * 65536);
  *outRemoved = (int)n;
  return true;
}

void norecoil_frame(void);

// Same collapse at G_DrawRooms' entry: the frame draws before SwapBuffers (game.cpp:6496).
void norecoil_before_draw(void);
