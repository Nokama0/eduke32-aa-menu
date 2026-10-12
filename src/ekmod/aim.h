#pragma once

// Once per frame from the overlay hook only: it calls ek_cansee, which may run only inside SwapBuffers.
void aim_frame(void);

bool aim_available(void);

// Aim solution for the current best target, or false. Solved from the muzzle, not the eye (else about pyoff + 1024 low;
// silentwt.cpp:369), and at the shot: a pre-solved angle was stale by 2048 z per tic of jetpack climb.
bool aim_solution_at(int muzzleX, int muzzleY, int muzzleZ, int q16horizoff,
                     int* q16ang, int* q16horiz);

// Same for the LIVE muzzle: pos + pyoff + ZOFFSET6 (player.cpp:1709). aim_solution_at takes the caller's muzzle so a
// ray and the shot share one line; q16horizoff is passed as the engine subtracts it separately (player.cpp:498).
bool aim_solution_for_shot(int* q16ang, int* q16horiz);

// The one read of whether this slot's live SHOOTS is hitscan, so every muzzle caller agrees (see projlead.h).
int aim_muzzle_z(int posZ, int pyoff);

// What the acquisition picked this frame, for the triggerbot: the plain un-led geometry of the winner beside where the
// player looked.
struct AimLock {
  int sprite;
  int yaw;
  int q16horiz;
  int myYaw;
  int myQ16Horiz;
  // marginZ: z between the aim point and the nearer edge of the visible run; range2d: range measured at. Both 0 = no
  // measurement (ekm_shot_placeable stands down).
  int marginZ;
  int range2d;
  // The engine's pitch offset that frame; q16horiz has it subtracted. Read only.
  int q16horizoff;
  int spanLo;
  int spanHi;
  int spanMid;
  int spanHeight;
  // Reach flag for the winner (1 reachable or unknown); 0 = no candidate reachable.
  int reachable;
};

bool aim_lock(AimLock* out);

// The calibration marker's two world points beside the lock: raw is the un-led body point, led the same after leading.
struct AimMarkerPoints {
  int sprite;
  int rawX, rawY, rawZ;
  int ledX, ledY, ledZ;
};
bool aim_marker_points(AimMarkerPoints* out);

unsigned aim_clock_ms(void);

// AA's aimbot goggles write ang/horiz every tic while locked, so ours stands down. True only when both reads succeeded.
inline bool ekm_goggles_hold_view(bool gotOn, int on, bool gotTarget, int target) {
  return gotOn && gotTarget && on == 1 && target >= 0;
}
