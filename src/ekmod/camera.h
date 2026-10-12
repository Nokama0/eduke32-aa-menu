#pragma once
#include "freecam.h"

// renderDrawRoomsQ16 is the one draw the free camera brackets; G_DrawRooms is located too because its extent says which
// of ten callers we stand in. The camera substitutes its pose into ud.camera* and the drawrooms arguments and leaves
// ud.camera* written so overlay, ESP and tracers match the drawn view. A failed locate costs the feature only.

bool camera_scan(void);

bool camera_ready(void);

// Image rvas, reported so the log can be checked against camerascan.cpp.
unsigned camera_rva(void);
unsigned camera_roomsQ16_rva(void);


// One attempt per session; SwapBuffers hook only: the thread that rewrites the five bytes must run them (hook.cpp).
bool camera_install(void);

// Unloading must restore the game's code: a patch into an unmapped module jumps into dead memory.
void camera_remove(void);

bool camera_active(void);

// Is the substituted draw in flight right now? Not camera_active(): a mirror or unreadable pose hands the draw back
// unchanged. Raised around the one `orig(pose...)` call (and a TROR map's base level), lowered on entry to every
// detoured drawrooms. Game thread only: nocull.cpp's sweep runs inside that call.
bool camera_render_armed(void);

// Drawn pose is above every sector holding it, or in none (FREECAM_Z_ABOVE). False unless camera_render_armed(). Render
// thread.
bool camera_above_map(void);
unsigned camera_zclass_frames(int cls);
unsigned camera_zclass_unreadable(void);
unsigned camera_zclass_us_last(void);
unsigned camera_zclass_us_max(void);

bool camera_extra_pass_active(void);

bool camera_tror_base_answered(void);
bool camera_tror_base_located(void);
bool camera_yax_span(unsigned* begin, unsigned* end);

unsigned camera_frame_number(void);

// The six arguments of the last player-view (or substituted) drawrooms call, always recorded: project_begin projects
// from them. Other fields are Debug-log-row only. `frame` is camera_frame_number() at the call. Render thread.
struct EkEngineView {
  unsigned frame;
  int x, y, z, angFix, horizFix, sect;
  int poseOk;
  int vr, yx, gotVr, gotYx;
  int calls;
  int vpX, vpY, vpW, vpH, xdim, ydim;
  int gotVp, gotDim;
};
bool camera_engine_view(EkEngineView* out);

void camera_q16_seed_entry(void);
void camera_q16_seed_exit(void);
void camera_q16_drawalls(void);
void camera_q16_alls(long long in, long long out);

// One tic on the game thread after it ran: publishes the pose the bracket reads. The seqlock's only writer.
void camera_tic(void);

// One presented frame, from overlay.cpp's per-frame path. A frame is not a tic: the step runs on the engine's tic
// counter (ototalclock, freecam_tic_advanced) so step and smoothRatio share a clock; per-frame stepping made speed the
// display's.
void camera_frame(void);

bool camera_installed(void);

// Installed AND still reached (the bracketed draw observed), re-proved not latched: EDuke32 can change renderer
// mid-session, and revoking returns the keyboard instead of swallowing keys with no change to the view.
bool camera_available(void);

// A separate answer: the sweep needs four more located functions, so it can refuse where the camera works; false after
// one fault.
bool camera_wholemap_available(void);

// Geometry needs only the sweep; ACTORS need the inlined polymer_scansprites and chain arrays; SKY needs the call-site
// redirect. The hint must say which.
bool camera_sprites_available(void);
bool camera_sky_available(void);

bool camera_fog_available(void);

bool camera_fullbright_available(void);

bool camera_transport_available(void);

// The published PAIR (before the last tic, and now) under one seqlock: 1 filled, 0 untouched. The camera steps at 30 Hz
// and the bracket blends by the engine's smoothRatio; both halves must come from one read.
int camera_pose_read(FreecamPose* prev, FreecamPose* cur);


int camera_viewpoint_read(FreecamPose* out);


// The frame's modelview and projection matrices (16 floats each) via the one glGetFloatv loader here. False: export
// missing. Render thread (needs the context current).
bool camera_gl_matrices(float* modelview16, float* projection16);

// The eye the substituted draw was placed with, the frustum apex (frustum.h); not the tic pose, which is published from
// the game thread and 128 units/tic stays under the 256 apex tolerance, so it would mislead. False when no draw is in
// flight.
bool camera_swept_eye(int* x, int* y, int* z);

// A viewpoint off the clipboard, window thread to game thread: latched (camera_frame is the only writer), latest wins,
// refused with a log line while the camera is down. A discontinuity: the previous-tic pose collapses onto it as in
// cam_start.
void camera_request_pose(const FreecamPose& pose);



int camera_teleport_plan_read(EkTeleportPlan* plan, int* have);

int camera_teleport_outcome(EkTeleportPlan* plan, unsigned* setMs, int* have);


// Floor/ceiling z of `sectnum` at (x, y) plus its record. `b->exact` 0 means the flat pair stands in for a slope
// (freecam.h:670 refuses on it). Callers: camera.cpp, cutpaint.cpp, nocull.cpp (game thread), secretvol.cpp (scan
// thread). A fault zeroes the shared s_getzRva; a map load can stale the scan thread's record.
struct EkSectorInfo;
bool cam_band_fill(int sectnum, int x, int y, FreecamBand* b, EkSectorInfo* si);
