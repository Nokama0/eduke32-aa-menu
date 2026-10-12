#pragma once

// EDuke32's `user_defs ud`: the camera and most game settings. Field offsets are part of the type (computed from
// game.h:161-279), but user_defs is internal, so a build may add a field; ud_base() re-checks that every time. details:
// docs/re/ud-offsets.md.
const int EK_UD_CAMERAPOS      = 0;
const int EK_UD_SCREEN_SIZE    = 228;
const int EK_UD_FOV            = 428;
const int EK_UD_CAMERAQ16ANG   = 432;
const int EK_UD_CAMERAQ16HORIZ = 436;
const int EK_UD_CAMERASECT     = 440;
// ud.pause_on: the pause the game tests (player.cpp:2358); gm stays MODE_GAME through a pause.
const int EK_UD_PAUSE_ON       = 442;
const int EK_UD_LEVEL_NUMBER   = 324;
const int EK_UD_VOLUME_NUMBER  = 328;
const int EK_UD_CONFIG_AUTOAIM = 468;

// The two fields that decide whether G_DrawRooms draws the world (game.cpp:792-800). Read off the shipped image, not
// transcribed (past config/setup, whose sizes are build-dependent): `cmp [rdi+228],7`, `[rdi+1348],2`, `[rdi+1356],0`;
// 1356-1348 = 8 matches game.h:267-269. details: docs/re/ud-offsets.md.
const int EK_UD_OVERHEAD_ON    = 1348;
const int EK_UD_SHOW_HELP      = 1356;

// Build's angle unit: a full turn is 2048, not 360 degrees or 2*pi.
const int EK_BUILD_ANGLE_UNITS = 2048;

struct EkCamera {
  int x, y, z;
  int angFix, horizFix;
  int ang;
  int sect;
  int fov;
};

// Resolve &ud; NULL if the cross-check fails, meaning the layout moved.
const unsigned char* ud_base(void);

bool ud_camera(EkCamera* out);

// Writing ud.camera* is inside the port's rule: G_DrawRooms rebuilds all six every frame (game.cpp:1001-1110).

// Not put back, on purpose: leaving the six written keeps overlay, ESP and tracers on the drawn view (project.cpp:70).

bool ud_camera_write(int x, int y, int z, int angFix, int horizFix, int sect);

bool ud_camera_guarded(EkCamera* out);

// Guarded ud.volume_number / ud.level_number; refused outside 7 volumes / 64 levels (duke3d.h:72-73).
inline bool ekm_ud_level_plausible(int volume, int level) {
  return volume >= 0 && volume < 7 && level >= 0 && level < 64;
}
bool ud_level_identity(int* volume, int* level);

// ud.pause_on, guarded. False means "assume paused".
bool ud_paused(bool* out);

// Full automap or help screen up? (G_DrawRooms returns early, game.cpp:800.) Out-of-range values are refused.
bool ud_map_or_help(bool* out);
