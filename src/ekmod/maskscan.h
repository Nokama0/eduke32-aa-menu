#pragma once

// Names the renderDrawMasks call the player's own view goes through and patches it so the world-quad list flushes on
// top of the masked geometry. DLL ONLY below the rules: the suite links none of the host-image code.

// G_DrawRooms calls renderDrawMasks TWICE and only one is the player's (the other is the ud.camerasprite viewscreen
// branch); both take no arguments and lie in the bracket, so every rule tests the PAIR or the bytes before a site.
// Patching the wrong one would tint only a security monitor.

// Step (b)'s position test over a target's two sites and the two renderDrawRoomsQ16 heads: a member of both chains is
// called once BETWEEN the heads and once AFTER the second. The pair may arrive either way round.
inline bool ekm_mask_straddles(unsigned a, unsigned b,
                               unsigned d1, unsigned d2) {
  const unsigned lo = a < b ? a : b;
  const unsigned hi = a < b ? b : a;
  return d1 < d2 && lo > d1 && lo < d2 && hi > d2;
}

// Step (d), the line that picks the player over the security monitor: the player's call is the one after the SECOND
// head; 0 when both or neither are. Only sound behind ekm_mask_straddles (it is given d2, not d1).
inline unsigned ekm_mask_player_site(unsigned a, unsigned b, unsigned d2) {
  const bool aAfter = a > d2;
  const bool bAfter = b > d2;
  if (aAfter == bAfter) return 0;
  return aAfter ? a : b;
}

const unsigned EK_MASK_MIN_EXTENT = 4096u;

inline bool ekm_mask_table_is_tight(int found, int max) {
  return max > 0 && found > max - (max / 4);
}

const int EK_MASK_MIN_SITES = 5;
const int EK_MASK_MIN_OWNERS = 5;
inline bool ekm_mask_corroborated(int sites, int owners) {
  return sites >= EK_MASK_MIN_SITES && owners >= EK_MASK_MIN_OWNERS;
}

// Where the nearest call before `site` ENDS. Errors are asymmetric: a row wrongly left out of `anchors` cuts the
// window too long (a refusal, safe); one wrongly left IN cuts it too short and lets a wrong candidate through.
inline unsigned ekm_mask_prev_call_end(const unsigned* sites,
                                       const unsigned char* anchors,
                                       int n, unsigned site) {
  if (!sites || !anchors || n <= 0) return 0;
  unsigned best = 0;
  for (int i = 0; i < n; i++) {
    if (!anchors[i]) continue;
    const unsigned e = sites[i] + 5u;
    if (e <= site && e > best) best = e;
  }
  return best;
}

// How many of the sixteen bytes before a call are THAT CALL'S OWN setup: the window stops at the previous call (the
// viewscreen call sits five bytes after G_DoSpriteAnimations's); argument registers do not survive a call.
inline int ekm_mask_window_len(unsigned site, unsigned prevEnd) {
  const unsigned lo = site >= 16u ? site - 16u : 0u;
  const unsigned from = (prevEnd > lo && prevEnd <= site) ? prevEnd : lo;
  return (int)(site - from);
}

inline bool ekm_mask_is_arg_reg(int r) {
  return r == 1 || r == 2 || r == 6 || r == 7 || r == 8 || r == 9;
}

inline bool ekm_mask_op_reg_dest(unsigned char op) {
  return op == 0x03 || op == 0x0B || op == 0x23 || op == 0x2B ||
         op == 0x33 || op == 0x63 || op == 0x8B || op == 0x8D;
}

inline bool ekm_mask_op_rm_dest(unsigned char op) {
  return op == 0x01 || op == 0x09 || op == 0x21 || op == 0x29 ||
         op == 0x31 || op == 0x89 || op == 0x81 || op == 0x83 ||
         op == 0xC1 || op == 0xC7 || op == 0xD1 || op == 0xD3 ||
         op == 0xF7 || op == 0xFF;
}

// Which register a write at `p` lands in, or -1. Scanned at EVERY offset (x86 cannot be decoded backwards).
// mov r32,imm32 and pop r64 are left out on purpose: a loose byte in a displacement decodes as one.
inline int ekm_mask_arg_dest(const unsigned char* b, int n, int p) {
  if (!b || p < 0 || p >= n) return -1;
  int rexR = 0, rexB = 0;
  if (b[p] >= 0x40 && b[p] <= 0x4F) {
    rexR = (b[p] >> 2) & 1;
    rexB = b[p] & 1;
    if (++p >= n) return -1;
  }
  const unsigned char op = b[p];
  if (op == 0x0F) {
    if (p + 2 >= n) return -1;
    const unsigned char op2 = b[p + 1];
    if (op2 != 0xB6 && op2 != 0xB7 && op2 != 0xBE && op2 != 0xBF) return -1;
    return (int)((b[p + 2] >> 3) & 7) + (rexR << 3);
  }
  if (p + 1 >= n) return -1;
  const unsigned char modrm = b[p + 1];
  if (ekm_mask_op_reg_dest(op)) return (int)((modrm >> 3) & 7) + (rexR << 3);
  if (ekm_mask_op_rm_dest(op) && (modrm >> 6) == 3)
    return (int)(modrm & 7) + (rexB << 3);
  return -1;
}

inline bool ekm_mask_touches_arg(const unsigned char* b, int n) {
  if (!b || n <= 0) return false;
  for (int p = 0; p < n; p++)
    if (ekm_mask_is_arg_reg(ekm_mask_arg_dest(b, n, p))) return true;
  return false;
}


// The table, then the locate: runs at most once per process, after camera_scan has published the bracket, from
// gamestate.cpp's 1 Hz sampler. It PATCHES NOTHING: the thread that rewrites the bytes must be the one that runs them.
void maskscan_probe(void);

// Patch the located site so the flush comes through us. One attempt per session, only once the locate has published.
// SwapBuffers HOOK ONLY, like every code patch in this port.
void maskscan_install(void);

unsigned maskscan_player_q16_site_rva(void);
bool maskscan_answered(void);

void maskscan_remove(void);

