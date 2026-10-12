#pragma once

// The engine's own ray cast, CALLED not hooked. It MUTATES shared engine globals, so ek_hitscan may be called ONLY
// inside the SwapBuffers hook on the tic's thread (docs/re/hitscan.md).

const int EK_HIT_X      = 0;
const int EK_HIT_Y      = 4;
const int EK_HIT_Z      = 8;
const int EK_HIT_SPRITE = 12;
const int EK_HIT_WALL   = 14;
const int EK_HIT_SECT   = 16;

struct EkHitData {
  int   x, y, z;
  short sprite, wall, sect;
};

const int EK_CLIPMASK1 = (256 << 16) + 64;

// THE SHOOTER'S OWN BLOCKING BITS: the ray starts inside the player's sprite; we clear bits 0 and 8 around the cast
// as the engine does and restore them in the same hook call.
const int EK_CSTAT_BLOCKING      = 1;
const int EK_CSTAT_BLOCK_HITSCAN = 256;
const int EK_SHOOTER_CSTATMASK   = EK_CSTAT_BLOCKING + EK_CSTAT_BLOCK_HITSCAN;

inline int ekm_shooter_cstat_cleared(int cstat) {
  return cstat & ~EK_SHOOTER_CSTATMASK;
}

inline bool ekm_shooter_cstat_changes(int cstat) {
  return (cstat & EK_SHOOTER_CSTATMASK) != 0;
}

// THE ORIGIN SECTOR: the player's own sector makes the walk hit through geometry the bullet stops at; corrected here.

const int EKM_US_INITIALDIST = 256;
const int EKM_US_MAXDIST     = 1536;

const int EKM_US_SHAPE_WINDOW = 0x40;

inline bool ekm_us_imm_at(const unsigned char* b, int n,
                          const unsigned char* op, int opLen, int want) {
  if (!b || !op || opLen <= 0) return false;
  for (int i = 0; i + opLen + 4 <= n; i++) {
    int k = 0;
    for (; k < opLen; k++) if (b[i + k] != op[k]) break;
    if (k != opLen) continue;
    const unsigned char* v = b + i + opLen;
    const unsigned imm = (unsigned)v[0] | ((unsigned)v[1] << 8) |
                         ((unsigned)v[2] << 16) | ((unsigned)v[3] << 24);
    if (imm == (unsigned)want) return true;
  }
  return false;
}

// updatesectorz has z in arg 5, so calling it with three args writes through a wild r9: this runs BEFORE any call.
inline bool ekm_us_shape_match(const unsigned char* b, int n) {
  const unsigned char movR9d[2]  = { 0x41, 0xB9 };
  const unsigned char movArg5[4] = { 0xC7, 0x44, 0x24, 0x20 };
  return ekm_us_imm_at(b, n, movR9d, 2, EKM_US_INITIALDIST) &&
         ekm_us_imm_at(b, n, movArg5, 4, EKM_US_MAXDIST);
}

inline unsigned ekm_us_rel32_target(unsigned siteRva, int disp) {
  return (unsigned)((int)siteRva + 5 + disp);
}

inline int ekm_us_origin_sector(bool haveCorrection, int corrected,
                                int passedIn, int maxSectors) {
  if (!haveCorrection) return passedIn;
  if (corrected < 0 || corrected >= maxSectors) return -1;
  return corrected;
}

bool hitscan_scan(void);
bool hitscan_ready(void);

unsigned hitscan_rva(void);

bool ek_hitscan(int x, int y, int z, int sect, int vx, int vy, int vz,
                EkHitData* out);

// Not pure: writes updatesectorneighborlist[]/map[]. Callable only from SwapBuffers, the polymer_drawrooms bracket
// and the G_DrawRooms hook.
bool updatesector_scan(void);
bool updatesector_ready(void);
unsigned updatesector_rva(void);

bool ek_updatesector(int x, int y, int from, int* out);
