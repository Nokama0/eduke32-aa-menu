#pragma once

// Build limits from build.h, Windows branch.
const int EK_MAXSPRITES = 16384;
const int EK_MAXSECTORS = 4096;
const int EK_MAXSTATUS  = 1024;
const int EK_MAXTILES   = 30720;

// spritetype (buildtypes.h); offsets identical to the World Tour mod's. Fixed-width, so x64 shifts nothing.
const int EK_SPR_STRIDE  = 44;
const int EK_SPR_X       = 0x00;
const int EK_SPR_Y       = 0x04;
const int EK_SPR_Z       = 0x08;
const int EK_SPR_CSTAT   = 0x0C;
const int EK_SPR_PICNUM  = 0x0E;
// Read shade as `signed char`: AA's coin marker is -80 (coinmagnet.h). The player's record carries the frozen state
// here (tbot.cpp).
const int EK_SPR_SHADE   = 0x10;
const int EK_SPR_PAL     = 0x11;
const int EK_SPR_XREPEAT = 0x14;
const int EK_SPR_YREPEAT = 0x15;
// Signed bytes (buildtypes.h:211): read unsigned, a negative offset throws a wall sprite far off.
const int EK_SPR_XOFFSET = 0x16;
const int EK_SPR_YOFFSET = 0x17;
const int EK_SPR_SECTNUM = 0x18;
const int EK_SPR_STATNUM = 0x1A;
const int EK_SPR_ANG     = 0x1C;
const int EK_SPR_OWNER   = 0x1E;
const int EK_SPR_XVEL    = 0x20;
// A custom projectile's remaining-bounce count (player.cpp:1006).
const int EK_SPR_YVEL    = 0x22;
const int EK_SPR_ZVEL    = 0x24;
const int EK_SPR_LOTAG   = 0x26;
const int EK_SPR_HITAG   = 0x28;
const int EK_SPR_EXTRA   = 0x2A;

// Shared so the ESP item walk and targets.cpp cannot drift.
const int EK_STAT_ACTOR  = 1;
const int EK_STAT_ZOMBIE = 2;
const int EK_STAT_STANDABLE = 6;
// The exit button is on STAT_MISC (game.cpp:1647-1663), other switches on STAT_DEFAULT 0.
const int EK_STAT_DEFAULT = 0;
const int EK_STAT_MISC    = 5;
const int EK_STAT_PROJECTILE = 4;

const int EK_SPRITE_SCAN_DEPTH = 2048;

// Call once a level is loaded; before that the scan correctly finds nothing.
bool gamestate_scan(void);
bool gamestate_ready(void);

// Rva of the `sprite` pointer slot; logged as evidence.
unsigned gamestate_sprite_ptr_rva(void);

// Rva of the records: a second search must agree; the slot can differ honestly, the array cannot.
unsigned gamestate_sprite_array_rva(void);

// The image walk decides, the content scan witnesses: camera.cpp decodes the engine's own sprite load (a proof), the
// scan is a heuristic that once named a different slot because the live sprite count moves while it walks.
enum {
  EK_SPR_AUTH_WAITING = 0,
  EK_SPR_AUTH_IMAGE   = 1,
  EK_SPR_AUTH_REFUSED = 2
};
enum {
  EK_SPR_SRC_NONE  = 0,
  EK_SPR_SRC_IMAGE = 1,
  EK_SPR_SRC_SCAN  = 2
};

// Called once by camera.cpp: the `sprite` pointer's rva, or 0 for a refused walk.
void gamestate_sprite_authority(unsigned spritePtrRva);

const int EK_SPRITE_AUTHORITY_WAIT = 30;

const int EK_LOCATE_ATTEMPTS    = 900;
const int EK_LOCATE_EVERY_TICKS = 2;

// `requiredReady`: player, actorstate, apScript, wall[] and sector[] all up.
inline bool ekm_locate_phase_active(int attemptsSoFar, bool requiredReady) {
  return !requiredReady && attemptsSoFar >= 0 &&
         attemptsSoFar < EK_LOCATE_ATTEMPTS;
}
inline bool ekm_locate_pass_due(int tick, int attemptsSoFar, bool requiredReady) {
  if (!ekm_locate_phase_active(attemptsSoFar, requiredReady)) return false;
  return tick >= 0 && tick % EK_LOCATE_EVERY_TICKS == 0;
}

// `waitedOut` is the budget above having expired.
inline int ekm_sprite_source_pick(int authState, bool imageOk, bool scanOk,
                                  bool waitedOut) {
  if (authState == EK_SPR_AUTH_IMAGE)
    // The proof answers or nothing does: the scan's different answer is the wrong array.
    return imageOk ? EK_SPR_SRC_IMAGE : EK_SPR_SRC_NONE;
  if (authState == EK_SPR_AUTH_WAITING && !waitedOut) return EK_SPR_SRC_NONE;
  return scanOk ? EK_SPR_SRC_SCAN : EK_SPR_SRC_NONE;
}

const unsigned char* gamestate_sprite(int i);

// The one guarded write; a failure logs once and latches writes off. Engine-rewritten fields only.
bool gamestate_sprite_write(int i, int off, const void* src, size_t n);

// Only these writes: x/y/z, ang, xvel (actors.cpp:667), yvel (bounce count, player.cpp:1006), shade (Fullbright,
// actors.cpp:1238), zvel (player.cpp:1004). Offset and size only; yvel means other things on other sprites, so the
// caller re-checks picnum/statnum.
inline bool ekm_sprite_write_allowed(int off, size_t n) {
  return (off == EK_SPR_X && n == 12) || (off == EK_SPR_ANG && n == 2) ||
         (off == EK_SPR_XVEL && n == 2) || (off == EK_SPR_YVEL && n == 2) ||
         (off == EK_SPR_ZVEL && n == 2) || (off == EK_SPR_SHADE && n == 1);
}

int gamestate_live_sprites(void);

// Shared readable spans so the actor scan makes no system call per candidate. One scan thread, no writer to race.
void gamestate_regions_refresh(void);
// Whether `bytes` from `p` lie wholly in ONE merged committed span.
bool gamestate_region_contains(const void* p, size_t bytes);
int  gamestate_region_count(void);
bool gamestate_region_at(int i, const unsigned char** base, size_t* bytes);

const unsigned char* gamestate_module_base(void);
size_t gamestate_module_size(void);
