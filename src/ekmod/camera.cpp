// The bracket, the pose, the sweep and the frame driver: substitutes the camera's pose into the located draw call.
// camerascan.cpp locates the call and says how renderDrawRoomsQ16 was identified.

#include <windows.h>
#include <intrin.h>
#include <string.h>
#include <stdint.h>
#include <type_traits>
#include "log.h"
#include "camera.h"
#include "camerascan.h"
#include "hitscan.h"
#include "imagescan.h"
#include "sectorstate.h"
#include "actorstate.h"
#include "tsprites.h"
#include "frustum.h"
#include "gamestate.h"
#include "hook.h"
#include "playerstate.h"
#include "aimsolve.h"
#include "config.h"
#include "input.h"
#include "udstate.h"
#include "cvars.h"
#include "shot.h"
#include "cutaway.h"
#include "nocull.h"
#include "perf.h"
#include "cutpaint.h"
#include "mapextent.h"
#include "ui/wallclock.h"
#include "norecoil.h"
#include "console.h"
#include "storeyflush.h"
#include "gamevars.h"
#include "project.h"

#pragma intrinsic(_ReturnAddress)

// One engine call wide: the pose goes into ud.camera* and the call's six arguments, and ud.camera* is deliberately
// NOT put back (G_DrawRooms rebuilds it; the overlay, ESP and tracers project from it). Every refusal calls the
// original unmodified: a frozen-looking camera is a bug report.

// The sector whose band contains the camera (freecam.h has the rule). The search is the last sector plus one ring of
// its wall neighbours: it runs inside polymer_drawrooms every frame, and a camera leaves a band through a floor,
// ceiling or wall. More walls than the walk looks at are truncated, not refused.
static const int EK_CAM_BAND_SLOTS = 24;
static const int EK_CAM_BAND_WALLS = 64;

typedef void (*GetZsOfSlopePtrFn)(const void* sec, int dax, int day,
                                  int* ceilz, int* florz);

static volatile LONG s_saidNoGetz = 0;

bool cam_band_fill(int sectnum, int x, int y, FreecamBand* b,
                   EkSectorInfo* si) {
  if (!sectorstate_info(sectnum, si)) return false;
  b->sect = sectnum;
  b->ceilz = si->ceilz;
  b->florz = si->florz;
  b->exact = si->sloped ? 0 : 1;
  if (b->exact) return true;

  const LONG rva = InterlockedCompareExchange(&s_getzRva, 0, 0);
  const unsigned char* const base = s_base;
  if (!rva || !base) return true;

  // Bounds-checked before the engine gets it: getzsofslopeptr indexes wall[sec->wallptr] unchecked. The guard covers
  // the first hop only; the engine then follows wall[wal->point2] unchecked, which on the game thread lands within
  // +-1 MB of wall[] in a 21 MB.bss (a wrong z for one frame).
  if (!ekm_sector_chain_record_ok(si->wallptr, si->wallnum)) return true;
  const void* const rec = sectorstate_record(sectnum);
  if (!rec) return true;

  int cz = b->ceilz, fz = b->florz;
  __try {
    ((GetZsOfSlopePtrFn)(const void*)(base + rva))(rec, x, y, &cz, &fz);
    b->ceilz = cz;
    b->florz = fz;
    b->exact = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchange(&s_getzRva, 0);
    if (InterlockedExchange(&s_saidNoGetz, 1) == 0) {
      ek_log("camera: the located getzsofslopeptr faulted; it will not be "
             "called again. Sloped sectors fall back to their flat band and "
             "the sector pick declines to substitute on one");
    }
  }
  return true;
}

static int cam_band_sect(int sect, int x, int y, int z) {
  if (!sectorstate_ready()) return sect;

  // Candidate zero is the sector the engine resolved, so a camera already inside its band picks what it always
  // picked; when the band cannot be trusted, freecam_band_pick declines to substitute.
  FreecamBand bands[EK_CAM_BAND_SLOTS];
  EkSectorInfo here = { 0, 0, 0, 0, 0 };
  if (!cam_band_fill(sect, x, y, &bands[0], &here)) return sect;
  int n = 1;

  const int walls = here.wallnum < EK_CAM_BAND_WALLS ? here.wallnum : EK_CAM_BAND_WALLS;
  for (int k = 0; k < walls && n < EK_CAM_BAND_SLOTS; k++) {
    EkWallFace face;
    if (!wallstate_face(here.wallptr + k, &face)) continue;
    const int ns = face.nextsector;
    if (ns < 0 || ns >= EK_MAXSECTORS || ns == sect) continue;
    bool seen = false;
    for (int j = 0; j < n; j++) { if (bands[j].sect == ns) { seen = true; break; } }
    if (seen) continue;
    EkSectorInfo nb = { 0, 0, 0, 0, 0 };
    if (!cam_band_fill(ns, x, y, &bands[n], &nb)) continue;
    n++;
  }

  return freecam_band_pick(bands, n, z, sect, EK_MAXSECTORS);
}

// The whole map on top of the sky: polymer.cpp's own two loops, run after the normal arm returned. Only the sectors
// the walk skipped (Polymer's drawingstate bitmap says which were drawn); drawing every sector duplicated the whole
// map and cost 30-40 fps.
typedef void (*PolyUpdateSectorFn)(short sectnum);
typedef void (*PolyDrawSectorFn)(short sectnum, int domasks);
typedef void (*PolyUpdateWallFn)(short wallnum);
typedef void (*PolyDrawWallFn)(short sectnum, short wallnum);

static bool s_saidSweepFault = false;

// GL_CULL_FACE: renderDrawRoomsQ16 ends with buildgl_setDisabled(GL_CULL_FACE), so the sweep inherited culling OFF
// and drew both faces of every wall (sectorstate.h ekm_sweep_cull). Resolved like project.cpp's glGetIntegerv from
// opengl32.dll (GL 1.1); a failure refuses the sweep rather than draw the defect.
#define EK_GL_CULL_FACE 0x0B44u
#define EK_GL_MODELVIEW_MATRIX 0x0BA6u
#define EK_GL_PROJECTION_MATRIX 0x0BA7u
typedef void (WINAPI* GlToggleFn)(unsigned int);
typedef unsigned char (WINAPI* GlIsEnabledFn)(unsigned int);
typedef void (WINAPI* GlGetFloatvFn)(unsigned int, float*);
static GlToggleFn s_glEnable = NULL;
static GlToggleFn s_glDisable = NULL;
static GlIsEnabledFn s_glIsEnabled = NULL;
static GlGetFloatvFn s_glGetFloatv = NULL;
static bool s_glTried = false;
static bool s_saidNoGl = false;

static int s_saidCull = -1;

static bool cam_gl_entries(void) {
  if (!s_glTried) {
    s_glTried = true;
    const HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) {
      s_glEnable = (GlToggleFn)GetProcAddress(gl, "glEnable");
      s_glDisable = (GlToggleFn)GetProcAddress(gl, "glDisable");
      s_glIsEnabled = (GlIsEnabledFn)GetProcAddress(gl, "glIsEnabled");
      s_glGetFloatv = (GlGetFloatvFn)GetProcAddress(gl, "glGetFloatv");
    }
  }
  return s_glEnable != NULL && s_glDisable != NULL && s_glIsEnabled != NULL;
}

// The sweep's visibility test (frustum.h has the criterion). Nothing is kept between frames: sector effectors move
// floorz, ceilingz and walls, so a cached bound would be stale while the geometry moves.

static int s_sweptEyeX = 0, s_sweptEyeY = 0, s_sweptEyeZ = 0;

static unsigned char s_cullSloped[EK_MAXSECTORS];

static bool s_saidNoGetFloatv = false;
static bool s_saidApex = false;

bool camera_gl_matrices(float* modelview16, float* projection16) {
  if (!modelview16 || !projection16) return false;
  (void)cam_gl_entries();
  if (!s_glGetFloatv) return false;
  s_glGetFloatv(EK_GL_MODELVIEW_MATRIX, modelview16);
  s_glGetFloatv(EK_GL_PROJECTION_MATRIX, projection16);
  return true;
}

bool camera_swept_eye(int* x, int* y, int* z) {
  if (!x || !y || !z) return false;
  if (!camera_render_armed()) return false;
  *x = s_sweptEyeX;
  *y = s_sweptEyeY;
  *z = s_sweptEyeZ;
  return true;
}

struct SweepFrustum {
  EkSidePlane left, right;
  float offL, offR;
  int ready;
};

static void cam_build_frustum(EkCullSlab slab, SweepFrustum* out) {
  out->ready = 0;
  out->offL = 0.0f;
  out->offR = 0.0f;
  if (!s_glGetFloatv) {
    if (!s_saidNoGetFloatv) {
      s_saidNoGetFloatv = true;
      ek_log("camera: opengl32.dll did not export glGetFloatv, so the whole-map "
             "sweep cannot read the matrices it would test visibility against. "
             "It draws every sector the portal walk skipped, exactly as it did "
             "before; the row and the camera are unaffected");
    }
    return;
  }
  if (!ekm_slab_usable(slab)) return;

  float mv[16], pr[16], pm[16];
  s_glGetFloatv(EK_GL_MODELVIEW_MATRIX, mv);
  s_glGetFloatv(EK_GL_PROJECTION_MATRIX, pr);
  ekm_mat4_mul(pr, mv, pm);

  const EkSidePlane L = ekm_plane_normalise(ekm_frustum_left(pm));
  const EkSidePlane R = ekm_plane_normalise(ekm_frustum_right(pm));
  if (!ekm_plane_usable(L) || !ekm_plane_usable(R)) return;

  const EkGlPoint eye = ekm_build_to_gl(s_sweptEyeX, s_sweptEyeY, s_sweptEyeZ);
  const float fL = ekm_plane_eval(L, eye);
  const float fR = ekm_plane_eval(R, eye);
  if (!ekm_frustum_apex_ok(fL, fR, EK_CULL_APEX_TOL)) {
    if (!s_saidApex) {
      s_saidApex = true;
      ek_log("camera: the frustum GL handed back does not have the camera at "
             "its apex (left %.1f, right %.1f build units, tolerance %.1f), so "
             "it is not this frame's view. The whole-map sweep draws every "
             "sector the portal walk skipped and culls nothing",
             fL, fR, EK_CULL_APEX_TOL);
    }
    return;
  }

  const float yLo = ekm_slab_ylo(slab);
  const float yHi = ekm_slab_yhi(slab);
  out->left = L;
  out->right = R;
  out->offL = ekm_cull_offset(L, yLo, yHi, EK_CULL_MARGIN);
  out->offR = ekm_cull_offset(R, yLo, yHi, EK_CULL_MARGIN);
  out->ready = 1;
}

// True only when every corner of the outline is outside the same side plane; false (draw it) on a sloped sector, a
// sloped or unknown neighbour, an unreadable wall or a span shorter than a polygon. The footprint is exactly these
// corners: wall quads take x,y from the same two wall records (WALLS_ARE_CONSISTENT).
static int cam_sector_offscreen(int wallFirst, int wallCount, int sloped,
                                int chainEnd, const SweepFrustum* fr) {
  if (sloped) return 0;
  int corners = 0, outL = 0, outR = 0;
  const int last = wallFirst + wallCount;
  for (int w = wallFirst; w < last; w++) {
    EkWallCorner c;
    if (!wallstate_corner(w, &c)) return 0;
    const int nsSloped =
        (c.nextsector >= 0 && c.nextsector < chainEnd)
            ? (int)s_cullSloped[c.nextsector] : 1;
    if (ekm_cull_neighbour_blocks(c.nextsector, chainEnd, nsSloped)) return 0;
    const EkGlPoint p = ekm_build_to_gl(c.x, c.y, 0);
    outL += ekm_cull_column_out(fr->left, fr->offL, p.x, p.z);
    outR += ekm_cull_column_out(fr->right, fr->offR, p.x, p.z);
    corners++;
    if (outL != corners && outR != corners) return 0;
  }
  return ekm_cull_reject(corners, outL, outR, 0, 1);
}

// The swept sectors' sprites: polymer_scansprites written out (GCC inlined both call sites; no body exists). The
// chain is walked, not sprite[] scanned: headspritesect/nextspritesect and sprite[].sectnum move in lockstep, and the
// scan walked all 16384 records every drawn frame (~720 KB at 240 fps), seen as choppiness.

static bool s_saidTsprRefused = false;
static bool s_saidTsprFull = false;

static unsigned char s_inList[(EK_MAXSPRITES + 7) / 8];

static void cam_sprites_refuse(const char* why) {
  InterlockedExchange(&s_spritesOk, 0);
  if (s_saidTsprRefused) return;
  s_saidTsprRefused = true;
  ek_log("camera: the swept sectors' sprites cannot be collected -- %s. The "
         "sprite pass is off for the session, so the sectors only the sweep "
         "reaches draw without their actors; THE SKY REDIRECT STAYS ARMED, the "
         "geometry sweep keeps running and the free camera keeps flying; "
         "nothing was left changed", why);
}

static void cam_append_sprites(int sweptEnd, int* outAdded) {
  *outAdded = 0;
  const unsigned char* const base = s_base;
  const unsigned tsprSlot = (unsigned)InterlockedCompareExchange(&s_tsprSlotRva, 0, 0);
  const unsigned cntSlot = (unsigned)InterlockedCompareExchange(&s_ssCntSlotRva, 0, 0);
  const unsigned sprSlot = (unsigned)InterlockedCompareExchange(&s_sprSlotRva, 0, 0);
  const unsigned headSlot = (unsigned)InterlockedCompareExchange(&s_headSlotRva, 0, 0);
  const unsigned nextSlot = (unsigned)InterlockedCompareExchange(&s_nextSlotRva, 0, 0);
  const unsigned dstate = (unsigned)InterlockedCompareExchange(&s_dstateRva, 0, 0);
  if (!base || !tsprSlot || !cntSlot || !sprSlot || !dstate) return;
  if (!headSlot || !nextSlot) return;
  if (!InterlockedCompareExchange(&s_spritesOk, 0, 0)) return;
  if (sweptEnd <= 0) return;
  if (!gamestate_ready()) return;

  // The witness is about the array: the walk names `sprite` itself, the content scan whichever global it latched
  // (once the CON VM's vm.pSprite, same array at rva 0x127C640), so slot rvas legitimately differ; the address of the
  // records is what is compared.
  const unsigned found = gamestate_sprite_array_rva();
  const size_t sprVar = slot_target_rva(base, sprSlot);
  const unsigned predicted =
      sprVar ? (unsigned)slot_target_rva(base, (unsigned)sprVar) : 0u;
  if (!ekm_sprite_array_agrees(predicted, found)) {
    ek_log("camera: the inlined scansprites names sprite[] at array rva 0x%07X "
           "(through the `sprite` pointer at rva 0x%llX) and the array in use "
           "is at 0x%07X; the sprite pass appends nothing rather than copy "
           "records it cannot vouch for",
           predicted, (unsigned long long)sprVar, found);
    cam_sprites_refuse("the array the image walk decodes is not the array "
                       "gamestate is handing out");
    return;
  }

  const size_t image = host_image_size(base);
  __try {
    const unsigned char* const drawn = (const unsigned char*)(base + dstate);
    unsigned char* const* const tspritep =
        *(unsigned char* const* const*)(base + tsprSlot);
    unsigned char* const arr = *tspritep;
    int* const cnt = *(int* const*)(base + cntSlot);
    const size_t arrRva = (arr > base) ? (size_t)(arr - base) : 0;
    if (!ekm_tsprite_array_ok(arrRva, image)) {
      cam_sprites_refuse("tsprite[] does not point inside the host image with "
                         "room for the whole list");
      return;
    }
    int n = *cnt;
    if (n < 0 || n > EK_MAXSPRITESONSCREEN) {
      cam_sprites_refuse("spritesortcnt is outside the list it indexes");
      return;
    }

    memset(s_inList, 0, sizeof(s_inList));
    for (int k = 0; k < n; k++) {
      const int owner = *(const short*)(arr + (size_t)k * EK_TSPR_STRIDE +
                                        EK_TSPR_OWNER);
      if (ekm_tsprite_owner_ok(owner, EK_MAXSPRITES))
        s_inList[ekm_bitmap_byte(owner)] |= (unsigned char)ekm_bitmap_mask(owner);
    }

    const short* const head =
        *(const short* const*)(base + headSlot);
    const short* const nxt =
        *(const short* const*)(base + nextSlot);
    int added = 0;
    int truncated = 0;
    int walked = 0;
    for (int s = 0; s < sweptEnd && !truncated; s++) {
      if (ekm_bitmap_test(drawn[ekm_bitmap_byte(s)], s)) continue;
      for (int i = head[s]; ekm_sprite_chain_live(i, EK_MAXSPRITES);
           i = nxt[i]) {
        if (!ekm_chain_budget_ok(++walked, EK_MAXSPRITES)) {
          *cnt = n;
          *outAdded = added;
          cam_sprites_refuse("a sector's sprite chain does not end -- more "
                             "links than there are sprites in the map");
          return;
        }
        const unsigned char* const r = gamestate_sprite(i);
        if (!r) {
          *cnt = n;
          *outAdded = added;
          cam_sprites_refuse("a chain link is outside the sprite array the "
                             "content scan found");
          return;
        }
        const int statnum = *(const short*)(r + EK_SPR_STATNUM);
        const int sectnum = *(const short*)(r + EK_SPR_SECTNUM);
        if (!ekm_sprite_chain_agrees(sectnum, s) ||
            !ekm_sprite_swept(statnum, sectnum, sweptEnd, EK_MAXSECTORS)) {
          *cnt = n;
          *outAdded = added;
          cam_sprites_refuse("a sprite reached through a sector's own chain "
                             "does not say it is in that sector");
          return;
        }
        // The list's own record, which the chain does not replace: on an external-arm frame the walk's bitmap is the
        // last walk's while the engine has scanned every sector, so the sector gate can be stale-clear; this one is
        // right on both.
        if (ekm_bitmap_test(s_inList[ekm_bitmap_byte(i)], i)) continue;
        const unsigned cstat = *(const unsigned short*)(r + EK_SPR_CSTAT);
        if (!ekm_tsprite_eligible(cstat, r[EK_SPR_XREPEAT], r[EK_SPR_YREPEAT], 0))
          continue;
        if (!ekm_tsprite_admit(n, EK_MAXSPRITESONSCREEN)) { truncated = 1; break; }
        unsigned char* const dst = arr + (size_t)n * EK_TSPR_STRIDE;
        memcpy(dst, r, (size_t)EK_TSPR_STRIDE);
        dst[EK_TSPR_CLIPDIST] = 0;
        *(short*)(dst + EK_TSPR_OWNER) = (short)i;
        n++;
        added++;
      }
    }
    *cnt = n;
    *outAdded = added;

    if (truncated && !s_saidTsprFull) {
      s_saidTsprFull = true;
      ek_log("camera: the frame's sprite list filled at %d entries, which is "
             "the engine's own MAXSPRITESONSCREEN, after the sweep added %d of "
             "the swept sectors' sprites. The rest of the map's actors, items "
             "and effects are not drawn on a frame this happens; the picture is "
             "short, and nothing was written past the list",
             EK_MAXSPRITESONSCREEN, added);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    cam_sprites_refuse("a read or a write of the engine's sprite list faulted");
  }
}

static void cam_draw_whole_map(void) {
  const unsigned char* const base = s_base;
  if (!base || !InterlockedCompareExchange(&s_sweepOk, 0, 0)) return;
  if (!sectorstate_ready()) return;

  const int skip = perf_skip_mask();

  const PolyUpdateSectorFn us =
      (PolyUpdateSectorFn)(const void*)(base + (unsigned)s_usRva);
  const PolyDrawSectorFn ds =
      (PolyDrawSectorFn)(const void*)(base + (unsigned)s_dsRva);
  const PolyUpdateWallFn uw =
      (PolyUpdateWallFn)(const void*)(base + (unsigned)s_uwRva);
  const PolyDrawWallFn dw =
      (PolyDrawWallFn)(const void*)(base + (unsigned)s_dwRva);

  const int glOk = cam_gl_entries() ? 1 : 0;
  const EkSweepCull cull =
      ekm_sweep_cull(glOk, glOk && s_glIsEnabled(EK_GL_CULL_FACE) ? 1 : 0);
  if (cull.refuse) {
    if (!s_saidNoGl) {
      s_saidNoGl = true;
      ek_log("camera: the whole-map sweep needs glEnable, glDisable and "
             "glIsEnabled from opengl32.dll so it can draw with backface "
             "culling on, and could not resolve all three. Refusing: the row "
             "delivers nothing, and the free camera and the game are "
             "unaffected");
    }
    return;
  }
  if (cull.enable != s_saidCull) {
    s_saidCull = cull.enable;
    ek_log("camera: the whole-map sweep found backface culling %s. %s",
           cull.enable ? "OFF, which is what renderDrawRoomsQ16 leaves behind "
                         "(engine.cpp:9563)"
                       : "already ON, which this file did not expect",
           cull.enable ? "Turning it on for the sweep's own draws and off again "
                         "after, so glbuild's cached state is left as it was"
                       : "Leaving it exactly as found; if the sweep still shows "
                         "wall backs, the cause is not this");
  }
  if (cull.enable) s_glEnable(EK_GL_CULL_FACE);


  int prevPtr = 0, prevNum = 0;
  volatile int sweptEnd = 0;
  __try {
    const void* const* const prsectors =
        *(const void* const* const*)(base + (unsigned)s_prsRva);
    const void* const* const prwalls =
        *(const void* const* const*)(base + (unsigned)s_prwRva);
    const unsigned char* const drawn =
        (const unsigned char*)(base + (unsigned)s_dstateRva);

    EkCullSlab slab = ekm_slab_init();
    int chainEnd = 0;
    perf_zone_begin(EK_PZ_SWEEP_SCAN);
    {
      int pPtr = 0, pNum = 0;
      for (int i = 0; i < EK_MAXSECTORS; i++) {
        EkSectorInfo si = { 0, 0, 0, 0, 0 };
        const int ok = sectorstate_info(i, &si) ? 1 : 0;
        const int live = prsectors[i] != NULL ? 1 : 0;
        const EkSectorSweep step = ekm_sector_sweep_step(
            i, ok, live, si.wallptr, si.wallnum, pPtr, pNum, EK_MAXSECTORS);
        if (step.stop) break;
        s_cullSloped[i] = (unsigned char)(si.sloped ? 1 : 0);
        slab = ekm_slab_add(slab, si.ceilz, si.florz);
        chainEnd = i + 1;
        pPtr = si.wallptr;
        pNum = si.wallnum;
      }
    }
    perf_zone_end(EK_PZ_SWEEP_SCAN, chainEnd);

    SweepFrustum fr;
    perf_zone_begin(EK_PZ_SWEEP_FRUSTUM);
    cam_build_frustum(slab, &fr);
    perf_zone_end(EK_PZ_SWEEP_FRUSTUM, 1);

    int drewSectors = 0;
    perf_zone_begin(EK_PZ_SWEEP_LOOP);
    for (int i = 0; i < EK_MAXSECTORS; i++) {
      EkSectorInfo si = { 0, 0, 0, 0, 0 };
      const int ok = sectorstate_info(i, &si) ? 1 : 0;
      const int live = prsectors[i] != NULL ? 1 : 0;
      const EkSectorSweep step = ekm_sector_sweep_step(
          i, ok, live, si.wallptr, si.wallnum, prevPtr, prevNum, EK_MAXSECTORS);
      if (step.stop) break;
      sweptEnd = i + 1;
      if (ekm_bitmap_test(drawn[ekm_bitmap_byte(i)], i)) {
        prevPtr = si.wallptr;
        prevNum = si.wallnum;
        continue;
      }
      if (fr.ready) {
        perf_zone_begin(EK_PZ_SWEEP_CULL);
        const int off = cam_sector_offscreen(step.wallFirst, step.wallCount,
                                             si.sloped, chainEnd, &fr);
        perf_zone_end(EK_PZ_SWEEP_CULL, 1);
        if (off && !ekm_perf_skips(skip, EK_PERF_SKIP_NOCULL)) {
          prevPtr = si.wallptr;
          prevNum = si.wallnum;
          continue;
        }
      }
      if (!ekm_perf_skips(skip, EK_PERF_SKIP_UPDATES)) {
        perf_zone_begin(EK_PZ_SWEEP_US);
        us((short)i);
        perf_zone_end(EK_PZ_SWEEP_US, 1);
      }
      perf_zone_begin(EK_PZ_SWEEP_DS);
      ds((short)i, 1);
      perf_zone_end(EK_PZ_SWEEP_DS, 1);
      drewSectors++;
      const int last = ekm_perf_skips(skip, EK_PERF_SKIP_WALLS)
                           ? step.wallFirst
                           : step.wallFirst + step.wallCount;
      for (int w = step.wallFirst; w < last; w++) {
        // The sector's own walls, so wallsect[] need not be located: the engine fills it from exactly this span and
        // the sectors tile wall[].
        if (!ekm_wall_drawable(prwalls[w] != NULL ? 1 : 0)) continue;
        if (!ekm_perf_skips(skip, EK_PERF_SKIP_UPDATES)) {
          perf_zone_begin(EK_PZ_SWEEP_UW);
          uw((short)w);
          perf_zone_end(EK_PZ_SWEEP_UW, 1);
        }
        perf_zone_begin(EK_PZ_SWEEP_DW);
        dw((short)i, (short)w);
        perf_zone_end(EK_PZ_SWEEP_DW, 1);
      }
      prevPtr = si.wallptr;
      prevNum = si.wallnum;
    }
    perf_zone_end(EK_PZ_SWEEP_LOOP, drewSectors);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchange(&s_sweepOk, 0);
    if (!s_saidSweepFault) {
      s_saidSweepFault = true;
      ek_log("camera: the whole-map sweep faulted and will not run again this "
             "session. The free camera keeps flying and the row now reads "
             "Unavailable; out of bounds the picture is the portal walk's "
             "again");
    }
    sweptEnd = 0;
  }

  if (cull.restore) s_glDisable(EK_GL_CULL_FACE);

  if (!ekm_perf_skips(skip, EK_PERF_SKIP_SPRITES)) {
    int spritesAdded = 0;
    perf_zone_begin(EK_PZ_SWEEP_SPRITES);
    cam_append_sprites((int)sweptEnd, &spritesAdded);
    perf_zone_end(EK_PZ_SWEEP_SPRITES, spritesAdded);
  }
}

// Remove fog: saved and restored, never written one way. The restored value is the one READ, not an assumed 512 (Duke
// moves visibility itself). No termination handler: G_DrawRooms recomputes g_visibility from the player at its head,
// so a missed restore costs the rest of one frame.
struct FogHold { int* p; int saved; };

static bool cam_fog_arm(FogHold* h) {
  h->p = (int*)(INT_PTR)InterlockedCompareExchange64(&s_gvisAddr, 0, 0);
  h->saved = 0;
  if (!h->p) return false;
  __try {
    h->saved = *h->p;
    *h->p = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchange64(&s_gvisAddr, 0);
    ek_log("camera: g_visibility would not read or write at the located "
           "address; Remove fog is off for the session and nothing was left "
           "changed");
    return false;
  }
  return true;
}

static void cam_fog_release(FogHold* h) {
  if (!h->p) return;
  __try {
    *h->p = h->saved;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Fullbright: the map's shades, lowered for one G_DrawRooms, armed in EK_DrawRoomsDetour around orig and released
// before it returns. No tic or save write runs inside, so no save can bake in a 0. Writes only what the engine writes
// itself (sector, wall and sprite shade), each on its array's allow-list and put back unless the draw rewrote it.
struct FbEntry { unsigned off; signed char saved; };
static FbEntry s_fbSecE[2 * EK_SECTOR_SLOTS];
static FbEntry s_fbWallE[EK_WALL_SLOTS];
static FbEntry s_fbSprE[EK_MAXSPRITES];
static unsigned char* s_fbSec = NULL;
static unsigned char* s_fbWall = NULL;
static unsigned char* s_fbSpr = NULL;
static int s_fbSecN = 0, s_fbWallN = 0, s_fbSprN = 0;
static int s_fbArmed = 0;
static int s_fbKeyN = 0, s_fbKeyW = 0, s_fbLoggedN = -1, s_fbLoggedW = -1;
static volatile LONG s_fbDead = 0;

bool camera_fullbright_available(void) {
  return camera_available() && sectorstate_ready() && wallstate_ready() &&
         gamestate_ready() && InterlockedCompareExchange(&s_fbDead, 0, 0) == 0;
}

static void fb_die(const char* why) {
  if (InterlockedExchange(&s_fbDead, 1) != 0) return;
  ek_log("camera: Fullbright %s; it is off for the session", why);
}

static bool fb_span_writable(const void* p, size_t bytes) {
  const unsigned char* cur = (const unsigned char*)p;
  const unsigned char* const end = cur + bytes;
  const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                  PAGE_EXECUTE_WRITECOPY;
  for (int k = 0; k < 16 && cur < end; k++) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(cur, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(mbi.Protect & w)) return false;
    cur = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  }
  return cur >= end;
}

static void fb_lower(unsigned char* base, unsigned off, FbEntry* list, int* n) {
  signed char* const p = (signed char*)(base + off);
  const signed char s = *p;
  if (!ekm_fullbright_records(s)) return;
  list[*n].off = off;
  list[*n].saved = s;
  (*n)++;
  *p = (signed char)ekm_fullbright_shade(s);
}

static void fb_restore(unsigned char* base, const FbEntry* list, int n,
                       int* restored, int* kept) {
  for (int i = 0; i < n; i++) {
    signed char* const p = (signed char*)(base + list[i].off);
    const signed char cur = *p;
    const int v = ekm_fullbright_restore(cur, list[i].saved);
    if (v != cur) { *p = (signed char)v; (*restored)++; }
    else (*kept)++;
  }
}

static void cam_fullbright_release(void) {
  if (!s_fbArmed) return;
  s_fbArmed = 0;
  perf_zone_begin(EK_PZ_FULLBRIGHT);
  int restored = 0, kept = 0;
  bool ok = true;
  __try {
    fb_restore(s_fbSec, s_fbSecE, s_fbSecN, &restored, &kept);
    fb_restore(s_fbWall, s_fbWallE, s_fbWallN, &restored, &kept);
    fb_restore(s_fbSpr, s_fbSprE, s_fbSprN, &restored, &kept);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  perf_zone_end(EK_PZ_FULLBRIGHT, 0);
  if (!ok) {
    fb_die("faulted putting shades back (slots after the fault may stay lowered "
           "until the engine rewrites them)");
    return;
  }
  if (g_cfg.debuglog && (s_fbKeyN != s_fbLoggedN || s_fbKeyW != s_fbLoggedW)) {
    s_fbLoggedN = s_fbKeyN;
    s_fbLoggedW = s_fbKeyW;
    ek_log("camera fullbright: %d sectors / %d walls in the map; lowered %d "
           "sector surface(s), %d wall(s), %d sprite(s); %d restored, %d kept "
           "because the draw rewrote them",
           s_fbKeyN, s_fbKeyW, s_fbSecN, s_fbWallN, s_fbSprN, restored, kept);
  }
}

static bool cam_fullbright_arm(void) {
  if (s_fbArmed) return false;
  if (!freecam_fullbright_wants(camera_active(),
                                g_cfg.freecamfullbright != 0,
                                camera_fullbright_available()))
    return false;
  int ns = 0, nw = 0;
  if (!mapextent_extent(&ns, &nw)) return false;
  if (ns > EK_SECTOR_SLOTS) ns = EK_SECTOR_SLOTS;
  if (nw > EK_WALL_SLOTS) nw = EK_WALL_SLOTS;
  unsigned char* const sec = (unsigned char*)sectorstate_record(0);
  unsigned char* const wal = (unsigned char*)wallstate_record(0);
  unsigned char* const spr = (unsigned char*)gamestate_sprite(0);
  if (!sec || !wal || !spr) return false;
  if (!ekm_sector_write_allowed(EK_SECT_CEILINGSHADE, 1) ||
      !ekm_sector_write_allowed(EK_SECT_FLOORSHADE, 1) ||
      !ekm_wall_write_allowed(EK_WALL_SHADE, 1) ||
      !ekm_sprite_write_allowed(EK_SPR_SHADE, 1)) {
    fb_die("found a shade field off its allow-list");
    return false;
  }
  if (!fb_span_writable(sec, (size_t)ns * EK_SECTOR_STRIDE) ||
      !fb_span_writable(wal, (size_t)nw * EK_WALL_STRIDE) ||
      !fb_span_writable(spr, (size_t)EK_MAXSPRITES * EK_SPR_STRIDE)) {
    fb_die("found sector[], wall[] or sprite[] not writable; nothing was changed");
    return false;
  }
  perf_zone_begin(EK_PZ_FULLBRIGHT);
  s_fbSec = sec;
  s_fbWall = wal;
  s_fbSpr = spr;
  s_fbSecN = s_fbWallN = s_fbSprN = 0;
  s_fbKeyN = ns;
  s_fbKeyW = nw;
  s_fbArmed = 1;
  bool ok = true;
  __try {
    for (int i = 0; i < ns; i++) {
      const unsigned r = (unsigned)i * EK_SECTOR_STRIDE;
      fb_lower(sec, r + EK_SECT_CEILINGSHADE, s_fbSecE, &s_fbSecN);
      fb_lower(sec, r + EK_SECT_FLOORSHADE, s_fbSecE, &s_fbSecN);
    }
    for (int i = 0; i < nw; i++)
      fb_lower(wal, (unsigned)i * EK_WALL_STRIDE + EK_WALL_SHADE, s_fbWallE,
               &s_fbWallN);
    for (int i = 0; i < EK_MAXSPRITES; i++) {
      const unsigned r = (unsigned)i * EK_SPR_STRIDE;
      const short st = *(const short*)(spr + r + EK_SPR_STATNUM);
      if (st < 0 || st >= EK_MAXSTATUS) continue;
      fb_lower(spr, r + EK_SPR_SHADE, s_fbSprE, &s_fbSprN);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  perf_zone_end(EK_PZ_FULLBRIGHT, s_fbSecN + s_fbWallN + s_fbSprN);
  if (!ok) {
    cam_fullbright_release();
    fb_die("faulted lowering shades; what was lowered has been put back");
    return false;
  }
  return true;
}

typedef void (*DrawRoomsFn)(int playerNum, int smoothRatio);

typedef int32_t (*DrawRoomsQ16Fn)(int x, int y, int z, int angFix,
                                  int horizFix, short sect);

static EkHook s_hook;
static EkHook s_roomsQ16Hook;

static DrawRoomsFn volatile s_drawOrig = NULL;
static DrawRoomsQ16Fn volatile s_roomsQ16Orig = NULL;

// The renderer is observed, not asserted (there is no `rendmode` cvar).
static volatile LONG s_drawSeenOk = 0;
static volatile LONG s_roomsQ16Idle = 0;

static bool s_attempted = false;

// The pose and the seqlock's engine half (freecam.h has the counter arithmetic). Five ints cannot be read with one
// interlocked op, so a reader landing inside a write retries; the interlocked ops are the full-fence barriers.
static volatile LONG s_seq = 0;
static FreecamPose s_pose;

static FreecamPose s_posePrev;

static unsigned seq_load(void) {
  return (unsigned)InterlockedCompareExchange(&s_seq, 0, 0);
}

static volatile LONG s_active = 0;

static FreecamState s_cam;

static const FreecamSens s_camSens = { FREECAM_SENS_MILLI_UNIT,
                                       FREECAM_SENS_MILLI_UNIT };

// The last sector the camera resolved into: the next resolve's seed and the answer kept when one fails. No interlock:
// the detour and cam_start run on the game's draw thread.
static int s_camSect = -1;

static const int EK_CAM_POSE_TRIES = 4;

// The viewpoint a copy takes (camera.h), on its own counter since it is published on the frame clock and may hold the
// player's pose while the camera is down. s_vpHave is set once so a copy before the first publish says so.
static volatile LONG s_vpSeq = 0;
static volatile LONG s_vpHave = 0;
static FreecamPose s_vp;

static volatile LONG s_reqSeq = 0;
static volatile LONG s_reqPending = 0;
static FreecamPose s_req;

bool camera_active(void) { return s_active != 0; }

int camera_pose_read(FreecamPose* prev, FreecamPose* cur) {
  if (!prev || !cur) return 0;
  for (int i = 0; i < EK_CAM_POSE_TRIES; i++) {
    const unsigned before = seq_load();
    const FreecamPose q = s_posePrev;
    const FreecamPose p = s_pose;
    const unsigned after = seq_load();
    if (freecam_seq_read_ok(before, after)) { *prev = q; *cur = p; return 1; }
  }
  return 0;
}

int camera_viewpoint_read(FreecamPose* out) {
  if (!out) return 0;
  if (!InterlockedCompareExchange(&s_vpHave, 0, 0)) return 0;
  for (int i = 0; i < EK_CAM_POSE_TRIES; i++) {
    const unsigned before = (unsigned)InterlockedCompareExchange(&s_vpSeq, 0, 0);
    const FreecamPose p = s_vp;
    const unsigned after = (unsigned)InterlockedCompareExchange(&s_vpSeq, 0, 0);
    if (freecam_seq_read_ok(before, after)) { *out = p; return 1; }
  }
  return 0;
}

static void cam_publish_viewpoint(const FreecamPose& p) {
  const unsigned begin =
      freecam_seq_write_begin((unsigned)InterlockedCompareExchange(&s_vpSeq, 0, 0));
  InterlockedExchange(&s_vpSeq, (LONG)begin);
  s_vp = p;
  InterlockedExchange(&s_vpSeq, (LONG)freecam_seq_write_end(begin));
  InterlockedExchange(&s_vpHave, 1);
}

void camera_request_pose(const FreecamPose& pose) {
  const unsigned begin =
      freecam_seq_write_begin((unsigned)InterlockedCompareExchange(&s_reqSeq, 0, 0));
  InterlockedExchange(&s_reqSeq, (LONG)begin);
  s_req = pose;
  InterlockedExchange(&s_reqSeq, (LONG)freecam_seq_write_end(begin));
  InterlockedExchange(&s_reqPending, 1);
}

static bool cam_take_request(FreecamPose* out) {
  if (!InterlockedExchange(&s_reqPending, 0)) return false;
  for (int i = 0; i < EK_CAM_POSE_TRIES; i++) {
    const unsigned before = (unsigned)InterlockedCompareExchange(&s_reqSeq, 0, 0);
    const FreecamPose p = s_req;
    const unsigned after = (unsigned)InterlockedCompareExchange(&s_reqSeq, 0, 0);
    if (freecam_seq_read_ok(before, after)) { *out = p; return true; }
  }
  return false;
}

void camera_tic(void) {
  if (!s_cam.active) {
    InterlockedExchange(&s_active, 0);
    return;
  }

  FreecamPose p, q;
  p.x = s_cam.x;
  p.y = s_cam.y;
  p.z = s_cam.z;
  p.ang = s_cam.ang & (FREECAM_ANG_UNITS - 1);
  p.horiz = freecam_pitch_to_horiz(s_cam.pitch);
  q.x = s_cam.px;
  q.y = s_cam.py;
  q.z = s_cam.pz;
  q.ang = s_cam.pang & (FREECAM_ANG_UNITS - 1);
  q.horiz = freecam_pitch_to_horiz(s_cam.ppitch);

  const unsigned begin = freecam_seq_write_begin(seq_load());
  InterlockedExchange(&s_seq, (LONG)begin);
  s_posePrev = q;
  s_pose = p;
  InterlockedExchange(&s_seq, (LONG)freecam_seq_write_end(begin));

  InterlockedExchange(&s_active, 1);
}

// Following a teleporter or water surface: the rules are freecam.h's freecam_transport_jump and its hold; this reads
// the engine. It walks the chain of the sector the camera is tracked into and keeps the effectors there, the same set
// G_MoveTransports reaches for one chain's cost.
static int s_transportHold = 0;
static bool s_saidNoTransport = false;
static bool s_saidTransportFault = false;
static bool s_saidTransportChain = false;

// The effector's t_data[4]. For an SE 7 it is not the CON action id actorstate.h names it after: the engine writes
// (sector.floorz == sprite.z) at spawn and reads it as the ONFLOORZ flag. A failed read is not a zero (zero is the
// airborne teleporter).
static bool cam_transport_onfloor(int i, int* out) {
  const unsigned char* const r = actorstate_record(i);
  if (!r || !out) return false;
  __try {
    *out = *(const int*)(r + EK_ACT_ACTION_ID);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static void camera_transport_tic(void) {
  if (g_cfg.freecamtransport == 0) {
    s_transportHold = 0;
    return;
  }
  // One tic off the hold before any refusal can skip it (a frozen hold would keep the camera held for the flight).
  // The top-up below is the other way round: it needs evidence the camera is on a warping sector, so refusals release
  // it.
  const FreecamHold held = freecam_transport_hold_next(s_transportHold);
  s_transportHold = held.tics;
  if (s_transportHold != 0) return;

  const unsigned char* const base = s_base;
  const unsigned headSlot = (unsigned)InterlockedCompareExchange(&s_headSlotRva, 0, 0);
  const unsigned nextSlot = (unsigned)InterlockedCompareExchange(&s_nextSlotRva, 0, 0);
  if (!base || !headSlot || !nextSlot || !gamestate_ready() ||
      !sectorstate_ready() || !actorstate_ready() || !updatesector_ready()) {
    if (!s_saidNoTransport) {
      s_saidNoTransport = true;
      ek_log("camera: follow teleporters is on but the parts it reads are not "
             "all up (chains %s, sprites %s, sectors %s, actors %s, "
             "updatesector %s); the camera flies through transports until they "
             "are. Nothing else is affected",
             (headSlot && nextSlot) ? "yes" : "no",
             gamestate_ready() ? "yes" : "no",
             sectorstate_ready() ? "yes" : "no",
             actorstate_ready() ? "yes" : "no",
             updatesector_ready() ? "yes" : "no");
    }
    return;
  }

  // Whether the camera is inside the map and still inside the sector it draws from. Seeded with s_camSect: the seeded
  // query returns the seed untouched while the point is inside it, so a different answer means the camera left it.
  // Silent: flying outside the map is ordinary.
  int resolved = -1;
  const bool inMap = ek_updatesector(s_cam.x, s_cam.y, s_camSect, &resolved);
  const int inSect =
      freecam_transport_sector(inMap, resolved, s_camSect, EK_MAXSECTORS);
  if (inSect < 0) return;

  const int camDz = clamp_ll_to_int((long long)s_cam.z - (long long)s_cam.pz);

  int chainBad = 0, onWarping = 0;
  int jumpSe = -1, jumpSect = -1, jumpLotag = 0;
  FreecamJump jump = { 0, 0, 0, 0, 0 };

  __try {
    const short* const head = *(const short* const*)(base + headSlot);
    const short* const nxt = *(const short* const*)(base + nextSlot);
    int walked = 0;
    for (int i = head[inSect]; ekm_sprite_chain_live(i, EK_MAXSPRITES);
         i = nxt[i]) {
      if (!ekm_chain_budget_ok(++walked, EK_MAXSPRITES)) { chainBad = 1; break; }
      const unsigned char* const r = gamestate_sprite(i);
      if (!r) { chainBad = 1; break; }
      // The same witness as the sprite pass, one link at a time.
      if (!ekm_sprite_chain_agrees(*(const short*)(r + EK_SPR_SECTNUM), inSect)) {
        chainBad = 1;
        break;
      }
      if (*(const short*)(r + EK_SPR_STATNUM) != FREECAM_TRANSPORT_STAT) continue;
      const int owner = *(const short*)(r + EK_SPR_OWNER);
      if (owner < 0 || owner >= EK_MAXSPRITES || owner == i) continue;
      onWarping = 1;
      const unsigned char* const o = gamestate_sprite(owner);
      if (!o) continue;
      const int ownSect = *(const short*)(o + EK_SPR_SECTNUM);
      if (ownSect < 0 || ownSect >= EK_MAXSECTORS) continue;

      EkSectorInfo sec, own;
      if (!sectorstate_info(inSect, &sec)) continue;
      if (!sectorstate_info(ownSect, &own)) continue;
      int td4 = 0;
      if (!cam_transport_onfloor(i, &td4)) continue;

      FreecamTransport t;
      t.lotag = sec.lotag;
      t.seX = *(const int*)(r + EK_SPR_X);
      t.seY = *(const int*)(r + EK_SPR_Y);
      t.seZ = *(const int*)(r + EK_SPR_Z);
      t.ownerX = *(const int*)(o + EK_SPR_X);
      t.ownerY = *(const int*)(o + EK_SPR_Y);
      t.ownerZ = *(const int*)(o + EK_SPR_Z);
      t.ownerCeilZ = own.ceilz;
      t.ownerFloorZ = own.florz;
      t.secCeilZ = sec.ceilz;
      t.secFloorZ = sec.florz;
      t.tempData4 = td4;

      const FreecamJump j = freecam_transport_jump(t, s_cam.x, s_cam.y, s_cam.z,
                                                   camDz);
      if (!j.hit) continue;
      jump = j;
      jumpSe = i;
      jumpSect = ownSect;
      jumpLotag = t.lotag;
      break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (!s_saidTransportFault) {
      s_saidTransportFault = true;
      ek_log("camera: reading the transports in sector %d faulted; the camera "
             "keeps flying and follows none for the rest of this tic. Nothing "
             "was written", inSect);
    }
    return;
  }

  if (chainBad && !s_saidTransportChain) {
    s_saidTransportChain = true;
    ek_log("camera: sector %d's sprite chain does not agree with sprite[] -- "
           "the camera follows no transports while that is true, and nothing "
           "was written", inSect);
  }

  // The top-up wins over the jump on the tic the hold expires, as in the engine (P_ProcessInput re-arms the hold to
  // 2). Without it the camera drifts inside the far pad's window for 13 tics and is carried straight back.
  const int topup = freecam_transport_hold_topup(held.expired, onWarping);
  if (topup != 0) {
    s_transportHold = topup;
    return;
  }
  if (jumpSe < 0) return;

  s_cam.x = jump.x;
  s_cam.y = jump.y;
  s_cam.z = jump.z;
  freecam_pose_carry_pos(&s_cam);
  const int fromSect = inSect;
  s_camSect = jumpSect;
  s_transportHold = jump.hold;
  ek_log("camera: followed transport sprite %d (sector %d lotag %d) to "
         "(%d,%d,%d) in sector %d", jumpSe, fromSect, jumpLotag, s_cam.x,
         s_cam.y, s_cam.z, jumpSect);
}

// One tic of the camera, read off the engine's game-tic counter (freecam_tic_advanced owns the detection). A
// GetTickCount accumulator rubber-banded (step and blend on different clocks) and smoothRatio's wrap goes blind below
// 30 fps.
static int s_prevTic = 0;
static bool s_haveTic = false;
static bool s_saidNoClock = false;

static void camera_step_on_engine_tic(void) {
  const LONGLONG addr = InterlockedCompareExchange64(&s_otclkAddr, 0, 0);
  if (addr == 0) return;
  int tic = 0;
  __try {
    tic = *(const volatile int*)(size_t)addr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchange64(&s_otclkAddr, 0);
    if (!s_saidNoClock) {
      s_saidNoClock = true;
      ek_log("camera: reading the engine's tic clock faulted; the free camera "
             "stops here and the row now reads Unavailable. The game is "
             "unaffected");
    }
    return;
  }
  const bool began = freecam_tic_advanced(s_prevTic, tic, s_haveTic);
  s_prevTic = tic;
  s_haveTic = true;
  if (!began || !s_cam.active) return;

  freecam_pose_carry(&s_cam);

  FreecamInput in;
  input_freecam_state(&in);
  freecam_step(&s_cam, in, freecam_speed_clamped(g_cfg.freecamspeed), s_camSens);

  camera_transport_tic();

  camera_tic();
}

static bool s_saidNoPose = false;
static bool s_saidNoSector = false;
static bool s_saidNoWrite = false;
static bool s_saidNotView = false;
static bool s_saidNoRatio = false;

// This frame's smoothRatio, taken from the argument (the engine's value is what the player's own view draws with).
// "This frame" is the dynamic extent of G_DrawRooms: set before the original, cleared after. The hooks install both
// or neither; the window left is teardown, where the frame draws the current pose unblended.
static int s_smoothRatio = 0;
static bool s_smoothFresh = false;

// The TROR base level: yax_drawrooms re-issues the armed call's arguments, so the six handed to orig are kept and
// compared. Valid only inside the G_DrawRooms that recorded them: cleared at its entry (a mirror's yax pass runs
// BEFORE the player's view), its exit, the player-view call, and once spent.
static FreecamViewArgs s_armedArgs = { 0, 0, 0, 0, 0, 0 };
static int s_armedHave = 0;

static unsigned s_trorSeen = 0, s_trorArmed = 0, s_trorRefused = 0;
static unsigned s_trorSeenFrame = 0, s_trorArmedFrame = 0, s_trorRefusedFrame = 0;
static unsigned s_trorSeenMax = 0, s_trorArmedMax = 0, s_trorRefusedMax = 0;
static unsigned s_trorSkipped = 0, s_trorSkippedFrame = 0, s_trorSkippedMax = 0;

static unsigned s_frameNumber = 0;
unsigned camera_frame_number(void) { return s_frameNumber; }

static int s_pfIn = 0;
static int s_pfDepth = 0;
static long long s_pfFirstIn = 0, s_pfLastOut = 0, s_pfDraws = 0;
static int s_pfCalls = 0;

static int s_q16On = 0;
static int s_q16Seeded = 0;
static int s_q16Drawn = 0;
static int s_q16N = 0;
static long long s_q16In = 0, s_q16Alls = 0, s_q16Sweep = 0;
static EkPerfQ16Split s_q16Split = { 0, 0, 0, 0, 0, EK_PERF_GAP_NONE, 0 };
static long long s_q16SweepPre = 0;

static void cam_q16_open(long long in) {
  s_q16On = 1;
  s_q16Seeded = s_q16Drawn = 0;
  s_q16N = 0;
  s_q16In = in;
  s_q16Alls = perf_zone_ticks(EK_PZ_CUT_DRAWALLS);
  s_q16Sweep = perf_zone_ticks(EK_PZ_SWEEP_POLY);
  s_q16Split = ekm_perf_q16_split_zero();
  s_q16SweepPre = 0;
}

static void cam_q16_close(long long out) {
  if (!s_q16On) return;
  s_q16On = 0;
  if (out <= 0 || out < s_q16In) return;
  const long long dA = perf_zone_ticks(EK_PZ_CUT_DRAWALLS) - s_q16Alls;
  const long long dS = perf_zone_ticks(EK_PZ_SWEEP_POLY) - s_q16Sweep;
  perf_q16_bank(s_q16N, ekm_perf_q16_residual(out - s_q16In, dA, dS));
  ekm_perf_q16_split_close(&s_q16Split, s_q16In, out, dS);
  perf_q16_bank_split(s_q16N, &s_q16Split,
                      ekm_perf_q16_residual_raw(out - s_q16In, dA, dS));
}

static void cam_q16_note(void) {
  const int n = nocull_bunches_now();
  if (n > s_q16N) s_q16N = n;
}

void camera_q16_seed_entry(void) {
  if (!s_q16On) return;
  if (s_q16Seeded) {
    const long long at = perf_stamp();
    cam_q16_close(at);
    if (at <= 0) return;
    cam_q16_open(at);
  }
  s_q16Seeded = 1;
}

void camera_q16_seed_exit(void) {
  if (s_q16On) cam_q16_note();
}

void camera_q16_drawalls(void) {
  if (!s_q16On || s_q16Drawn) return;
  s_q16Drawn = 1;
  s_q16SweepPre = perf_zone_ticks(EK_PZ_SWEEP_POLY) - s_q16Sweep;
  cam_q16_note();
}

void camera_q16_alls(long long in, long long out) {
  if (!s_q16On || in <= 0 || out <= 0) return;
  ekm_perf_q16_split_alls(&s_q16Split, s_q16In, in, out, s_q16SweepPre);
}

struct Q16Stamp {
  long long in;
  Q16Stamp() : in(0) {
    if (!s_pfIn || s_pfDepth++ != 0) return;
    in = perf_stamp();
    if (in > 0 && s_pfCalls == 0) s_pfFirstIn = in;
    s_q16On = 0;
    if (in > 0 && g_cfg.perfzones != 0) cam_q16_open(in);
  }
  ~Q16Stamp() {
    if (!s_pfIn) return;
    if (--s_pfDepth != 0 || in <= 0) return;
    const long long out = perf_stamp();
    cam_q16_close(out);
    if (out <= 0) return;
    if (out > in) s_pfDraws += out - in;
    s_pfLastOut = out;
    s_pfCalls++;
  }
};

static void cam_partition_bank(long long in, long long out) {
  const EkPerfPartition p =
      ekm_perf_partition(in, s_pfFirstIn, s_pfLastOut, out, s_pfDraws, s_pfCalls);
  if (!p.ok) return;
  perf_zone_add(EK_PZ_ENGINE_HEAD, p.head, 1);
  perf_zone_add(EK_PZ_ENGINE_DRAWS, p.draws, s_pfCalls);
  perf_zone_add(EK_PZ_ENGINE_TAIL, p.tail, 1);
  perf_zone_add(EK_PZ_ENGINE_FRAME, p.whole, 1);
}

static void cam_tror_frame_begin(void) {
  s_frameNumber++;
  s_armedHave = 0;
  storeyflush_frame_begin();
  if (s_trorSeenFrame > s_trorSeenMax) s_trorSeenMax = s_trorSeenFrame;
  if (s_trorArmedFrame > s_trorArmedMax) s_trorArmedMax = s_trorArmedFrame;
  if (s_trorRefusedFrame > s_trorRefusedMax) s_trorRefusedMax = s_trorRefusedFrame;
  if (s_trorSkippedFrame > s_trorSkippedMax) s_trorSkippedMax = s_trorSkippedFrame;
  s_trorSeenFrame = s_trorArmedFrame = s_trorRefusedFrame = 0;
  s_trorSkippedFrame = 0;
}

static void EK_DrawRoomsDetour(int playerNum, int smoothRatio) {
  DrawRoomsFn const orig = s_drawOrig;
  if (!orig) return;
  norecoil_before_draw();
  s_smoothRatio = smoothRatio;
  s_smoothFresh = true;
  cam_tror_frame_begin();
  camera_step_on_engine_tic();
  const bool fbArmed = cam_fullbright_arm();
  s_pfFirstIn = s_pfLastOut = s_pfDraws = 0;
  s_pfCalls = 0;
  s_pfDepth = 0;
  const long long pfIn = perf_stamp();
  s_pfIn = pfIn > 0 ? 1 : 0;
  orig(playerNum, smoothRatio);
  const long long pfOut = s_pfIn ? perf_stamp() : 0;
  s_pfIn = 0;
  cam_partition_bank(pfIn, pfOut);
  if (fbArmed) cam_fullbright_release();
  s_smoothFresh = false;
  s_armedHave = 0;
}

// Which of the ten calls this is: renderDrawRoomsQ16 draws mirrors, the three TROR passes and a viewscreen sprite as
// well as the player's view, and the pose may only go into the last (freecam.h owns the rule; this supplies the
// return address as an rva and the engine's ud.camera*). Under Polymost all are reachable.
static bool detour_is_player_view(const void* ret, int x, int y, int z,
                                  int angFix, int horizFix, int sect) {
  const unsigned char* const base = s_base;
  if (!base) return false;
  EkCamera cam;
  if (!ud_camera_guarded(&cam)) return false;
  const FreecamViewArgs args = { x, y, z, angFix, horizFix, sect };
  const FreecamViewArgs eng = { cam.x, cam.y, cam.z, cam.angFix, cam.horizFix,
                                cam.sect };
  const unsigned retRva =
      (unsigned)(size_t)((const unsigned char*)ret - base);
  return freecam_is_player_view(retRva, (unsigned)s_drawBegin,
                                (unsigned)s_drawEnd, args, eng);
}

static EkEngineView s_engView;
static int s_engViewHave = 0;
static unsigned s_engViewCallsFrame = 0;
static int s_engViewCalls = 0;

bool camera_engine_view(EkEngineView* out) {
  if (!out || !s_engViewHave) return false;
  *out = s_engView;
  return true;
}

static void cam_engine_view_store(int x, int y, int z, int angFix, int horizFix,
                                  int sect, int poseOk) {
  EkEngineView v;
  v.frame = s_frameNumber;
  v.x = x; v.y = y; v.z = z;
  v.angFix = angFix; v.horizFix = horizFix; v.sect = sect;
  v.poseOk = poseOk;
  v.vr = v.yx = v.gotVr = v.gotYx = 0;
  v.calls = 0;
  if (g_cfg.debuglog) {
    if (s_engViewCallsFrame != s_frameNumber) {
      s_engViewCallsFrame = s_frameNumber;
      s_engViewCalls = 0;
    }
    s_engViewCalls++;
    int vr = 0, yx = 0;
    const bool gotVr = gamevars_viewingrange(&vr);
    const bool gotYx = gamevars_yxaspect(&yx);
    v.vr = vr; v.yx = yx;
    v.gotVr = gotVr ? 1 : 0;
    v.gotYx = gotYx ? 1 : 0;
    v.calls = s_engViewCalls;
  }
  v.vpX = v.vpY = v.vpW = v.vpH = v.xdim = v.ydim = 0;
  v.gotVp = v.gotDim = 0;
  s_engView = v;
  s_engViewHave = 1;
}

static bool cam_engine_view_note(const void* ret, int x, int y, int z,
                                 int angFix, int horizFix, int sect) {
  const unsigned char* const base = s_base;
  if (!base) return false;
  const unsigned retRva = (unsigned)(size_t)((const unsigned char*)ret - base);
  if (!freecam_rva_in_span(retRva, (unsigned)s_drawBegin, (unsigned)s_drawEnd))
    return false;
  if (!detour_is_player_view(ret, x, y, z, angFix, horizFix, sect)) return false;
  cam_engine_view_store(x, y, z, angFix, horizFix, sect, 1);
  return true;
}

static bool s_saidEngVpUnread = false;

static void cam_engine_view_vp_note(void) {
  EkViewRead r;
  project_read_view(&r);
  s_engView.vpX = r.vp.x;
  s_engView.vpY = r.vp.y;
  s_engView.vpW = r.vp.w;
  s_engView.vpH = r.vp.h;
  s_engView.xdim = r.xdim;
  s_engView.ydim = r.ydim;
  s_engView.gotVp = r.vp.fromGL ? 1 : 0;
  s_engView.gotDim = 1;
  if (!r.vp.fromGL && !s_saidEngVpUnread) {
    s_saidEngVpUnread = true;
    ek_log("camera: D37 GL_VIEWPORT did not answer after the player-view draw "
           "(no glGetIntegerv, or a zero-sized viewport); the viewport goes "
           "uncompared, xdim/ydim %d/%d still are", r.xdim, r.ydim);
  }
}

// The sky redirect: one call site, widened while we draw. polymer_drawrooms sends a z outside its sector's band down
// the external arm, which returns before the sky is drawn, and far above the roofs no sector's band holds the camera.

// The address the patched site used to reach, captured at install, not read from s_getzRva (cam_band_fill may zero it
// after a fault of its own; the engine keeps calling this address). The thunk must forward as long as the patch is
// in.
static GetZsOfSlopePtrFn s_getzOrig = NULL;
static EkCallPatch s_getzPatch;

static int s_widenActive = 0;

// The out-of-bounds smear and the one glClear: polymost_drawrooms clears the colour buffer only under `editstatus`.
// In play the world covers every column; out of bounds the free camera leaves columns no surface covers and they keep
// the previous frame's pixels. Mapster32 clears for the same reason.
typedef void(WINAPI* GlClearFn)(unsigned int);
typedef void(WINAPI* GlClearColorFn)(float, float, float, float);
static GlClearFn s_glClear = NULL;
static GlClearColorFn s_glClearColor = NULL;
static bool s_glClearTried = false;

static bool cam_clear_ready(void) {
  if (s_glClearTried) return s_glClear != NULL && s_glClearColor != NULL;
  s_glClearTried = true;
  const HMODULE gl = GetModuleHandleA("opengl32.dll");
  if (!gl) {
    ek_log("camera: opengl32.dll is not in the process, so the out-of-bounds "
           "clear cannot run; everything else keeps working");
    return false;
  }
  s_glClear = (GlClearFn)GetProcAddress(gl, "glClear");
  s_glClearColor = (GlClearColorFn)GetProcAddress(gl, "glClearColor");
  if (!s_glClear || !s_glClearColor) {
    ek_log("camera: opengl32.dll has no glClear/glClearColor, so the "
           "out-of-bounds clear cannot run; everything else keeps working");
    return false;
  }
  ek_log("camera: the out-of-bounds colour clear is armed -- the engine clears "
         "colour only under editstatus (polymost.cpp:6854), so on the "
         "substituted path we clear it ourselves");
  return true;
}

static void cam_clear_backdrop(void) {
  if (!cam_clear_ready()) return;
  s_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  s_glClear(0x00004000u);
}

// The extra pass draws sectors one pass never submits, far-first. Cumulative, never sampled. Index 0 is the
// substituted player view on a non-TROR map, 1 the TROR base level.
static unsigned s_xpWanted[2] = { 0, 0 };
static unsigned s_xpMade[2] = { 0, 0 };
static unsigned s_xpRefused[2][FREECAM_EXTRA_REASONS];
static unsigned s_xpSuspendFailed[2] = { 0, 0 };
static unsigned s_2pLastMs = 0;
static bool s_2pFirstTick = true;

static bool cam_2p_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_2pFirstTick && (now - s_2pLastMs) < 1000u) return false;
  s_2pFirstTick = false;
  s_2pLastMs = now;
  return true;
}

struct ZClearHold {
  int saved;
  bool held;
  ZClearHold() : saved(1), held(false) {}
  ~ZClearHold() { if (held) cutaway_zclear_restore(saved); }
};

static int s_extraPassActive = 0;
bool camera_extra_pass_active(void) { return s_extraPassActive != 0; }

// spritesortcnt around the player-view extra pass: Polymost does not reset it per pass, so the extra pass's tsprites
// would reach renderDrawMasks (near sprites doubled, far ones eating MAXSPRITESONSCREEN). The count is vouched for
// before and written back after, discarding only the appends.
static unsigned s_xpCountRestoreFailed = 0;
static bool s_saidCountRestore = false;

static bool cam_sprite_count_read(int** outCnt, int* outVal) {
  *outCnt = NULL;
  *outVal = 0;
  const unsigned char* const base = s_base;
  const unsigned cntSlot =
      (unsigned)InterlockedCompareExchange(&s_ssCntSlotRva, 0, 0);
  if (!base || !cntSlot) return false;
  if (!InterlockedCompareExchange(&s_spritesOk, 0, 0)) return false;
  int* cnt = NULL;
  int n = -1;
  __try {
    cnt = *(int* const*)(base + cntSlot);
    n = *cnt;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (!freecam_sprite_count_ok(n, EK_MAXSPRITESONSCREEN)) return false;
  *outCnt = cnt;
  *outVal = n;
  return true;
}

static bool cam_sprite_count_write(int* cnt, int value) {
  if (!cnt) return false;
  __try {
    *cnt = value;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

struct SpriteCountHold {
  int* cnt;
  int saved;
  SpriteCountHold(int* c, int v) : cnt(c), saved(v) {}
  ~SpriteCountHold() {
    if (cam_sprite_count_write(cnt, saved)) return;
    s_xpCountRestoreFailed++;
    if (!s_saidCountRestore) {
      s_saidCountRestore = true;
      ek_log("camera 2pass: writing spritesortcnt back to %d after the extra "
             "pass FAILED; that frame may draw the extra pass's sprites too. "
             "Counted in the camera 2pass line", saved);
    }
  }
};

static int cam_extra_decide(bool trorBase, bool spriteCountOk) {
  const int site = trorBase ? 1 : 0;
  const int xp = freecam_extra_pass_wanted(g_cfg.freecamsweeppasses,
                                           nocull_last_main_stopped(), trorBase,
                                           cutaway_nodraw_value(),
                                           cutaway_zclear_rva() != 0,
                                           g_cfg.freecamwholemap != 0,
                                           spriteCountOk);
  if (xp == FREECAM_EXTRA_YES) s_xpWanted[site]++;
  else if (xp > 0 && xp < FREECAM_EXTRA_REASONS) s_xpRefused[site][xp]++;
  return xp;
}

// Same trampoline and arguments as the main pass after it; `orig` resumes past the patched entry so it cannot
// re-enter our detour. Own zone EK_PZ_ENGINE_EXTRA; its result is discarded.
static void cam_extra_pass(DrawRoomsQ16Fn orig, int x, int y, int z, int ang,
                           int horiz, short sect) {
  if (!orig) return;
  s_extraPassActive = 1;
  perf_zone_begin(EK_PZ_ENGINE_EXTRA);
  orig(x, y, z, ang, horiz, sect);
  perf_zone_end(EK_PZ_ENGINE_EXTRA, 1);
  s_extraPassActive = 0;
}

static void cam_xp_report(void) {
  if (g_cfg.debuglog == 0 || !cam_2p_due()) return;
  ek_log("camera 2pass: Sweep passes = %s. Extra pass, player view (non-TROR) "
         "/ TROR base level: wanted %u / %u, made %u / %u; refused by the "
         "setting %u / %u, no main-pass budget stop within the last %d main "
         "passes %u / %u, TROR "
         "prepare pass or g_nodraw unknown %u / %u, depth-clear flag not "
         "located %u / %u, whole-map row off %u / %u, spritesortcnt not "
         "vouched for (player view only) %u; flag suspend FAILED after the "
         "pass (the normal pass then cleared depth and drew over it) %u / %u; "
         "spritesortcnt restore FAILED %u",
         freecam_passes_name(g_cfg.freecamsweeppasses), s_xpWanted[0],
         s_xpWanted[1], s_xpMade[0], s_xpMade[1],
         s_xpRefused[0][FREECAM_EXTRA_NO_SETTING],
         s_xpRefused[1][FREECAM_EXTRA_NO_SETTING],
         EK_NOCULL_EXTRA_HOLD,
         s_xpRefused[0][FREECAM_EXTRA_NO_STOP],
         s_xpRefused[1][FREECAM_EXTRA_NO_STOP],
         s_xpRefused[0][FREECAM_EXTRA_NO_PREPARE],
         s_xpRefused[1][FREECAM_EXTRA_NO_PREPARE],
         s_xpRefused[0][FREECAM_EXTRA_NO_FLAG],
         s_xpRefused[1][FREECAM_EXTRA_NO_FLAG],
         s_xpRefused[0][FREECAM_EXTRA_NO_WHOLEMAP],
         s_xpRefused[1][FREECAM_EXTRA_NO_WHOLEMAP],
         s_xpRefused[0][FREECAM_EXTRA_NO_SPRITECOUNT], s_xpSuspendFailed[0],
         s_xpSuspendFailed[1], s_xpCountRestoreFailed);
}

// Measurement only: is the camera's z inside the sector we hand the engine? ek_updatesector is a 2D query, so out of
// bounds it can return a sector the camera is over but far from vertically. Counts how often; changes nothing.
// Build's z grows downward. Cumulative, never sampled.
static unsigned s_zvsInside = 0;
static unsigned s_zvsAboveCeil = 0;
static unsigned s_zvsBelowFloor = 0;
static unsigned s_zvsUnreadable = 0;
static unsigned s_zvsFramesAtSeam = 0;
static unsigned s_zvsInsideAtSeam = 0;
static unsigned s_zvsAboveCeilAtSeam = 0;
static unsigned s_zvsBelowFloorAtSeam = 0;
static unsigned s_zvsUnreadableAtSeam = 0;
static unsigned s_zvsFrames = 0;
static unsigned s_zvsLastMs = 0;
static bool s_zvsFirstTick = true;

static bool cam_zvs_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_zvsFirstTick && (now - s_zvsLastMs) < 1000u) return false;
  s_zvsFirstTick = false;
  s_zvsLastMs = now;
  return true;
}

static void cam_zvs_measure(int sect, int camZ) {
  s_zvsFrames++;
  EkSectorInfo si = { 0, 0, 0, 0, 0 };
  if (sect < 0 || sect >= EK_MAXSECTORS || !sectorstate_info(sect, &si)) {
    s_zvsUnreadable++;
  } else if (camZ < si.ceilz) {
    s_zvsAboveCeil++;
  } else if (camZ > si.florz) {
    s_zvsBelowFloor++;
  } else {
    s_zvsInside++;
  }

  if (!cam_zvs_due()) return;
  const unsigned dFrames = s_zvsFrames - s_zvsFramesAtSeam;
  const unsigned dInside = s_zvsInside - s_zvsInsideAtSeam;
  const unsigned dAboveCeil = s_zvsAboveCeil - s_zvsAboveCeilAtSeam;
  const unsigned dBelowFloor = s_zvsBelowFloor - s_zvsBelowFloorAtSeam;
  const unsigned dUnreadable = s_zvsUnreadable - s_zvsUnreadableAtSeam;
  s_zvsFramesAtSeam = s_zvsFrames;
  s_zvsInsideAtSeam = s_zvsInside;
  s_zvsAboveCeilAtSeam = s_zvsAboveCeil;
  s_zvsBelowFloorAtSeam = s_zvsBelowFloor;
  s_zvsUnreadableAtSeam = s_zvsUnreadable;
  if (dFrames == 0) return;
  ek_log("camera zvs: sector %d ceilz %d florz %d camz %d (z increases "
         "DOWNWARD: above-ceiling is z<ceilz) -- this window %u frame(s): "
         "%u inside, %u above ceiling, %u below floor, %u unreadable",
         sect, si.ceilz, si.florz, camZ, dFrames, dInside, dAboveCeil,
         dBelowFloor, dUnreadable);
}

static unsigned s_substLogMs = 0;
static bool s_substLogFirst = true;
static bool cam_subst_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_substLogFirst && (now - s_substLogMs) < 1000u) return false;
  s_substLogFirst = false;
  s_substLogMs = now;
  return true;
}

static const char* cam_subst_label(int state) {
  switch (freecam_subst_clamped(state)) {
    case FREECAM_SUBST_KEEP_SECT:  return "Keep game sect";
    case FREECAM_SUBST_KEEP_Z:     return "Keep game z";
    case FREECAM_SUBST_KEEP_HORIZ: return "Keep game horiz";
    case FREECAM_SUBST_KEEP_ANG:   return "Keep game ang";
    default:                       return "All";
  }
}

static const char* cam_ud_label(int state) {
  switch (freecam_ud_clamped(state)) {
    case FREECAM_UD_KEEP_SECT:  return "Game sect in ud";
    case FREECAM_UD_KEEP_Z:     return "Game z in ud";
    case FREECAM_UD_KEEP_HORIZ: return "Game horiz in ud";
    case FREECAM_UD_KEEP_ANG:   return "Game ang in ud";
    default:                    return "All";
  }
}

static int s_widenZ = 0;

// Whether our own substituted draw is in flight, distinct from the armed flag above. nocull.cpp's `bracketArmed`
// reads it from inside that call. Also raised around the TROR base-level draw (cam_tror_base_draw).
static int s_substActive = 0;

bool camera_render_armed(void) { return s_substActive != 0; }

static int s_zClass = FREECAM_Z_INSIDE;
static unsigned s_zFrames[FREECAM_Z_CLASSES] = { 0, 0, 0, 0 };
static unsigned s_zUnreadable = 0;
static unsigned s_zUsLast = 0;
static unsigned s_zUsMax = 0;
bool camera_above_map(void) {
  return s_substActive != 0 && s_zClass == FREECAM_Z_ABOVE;
}
unsigned camera_zclass_frames(int cls) {
  return (cls >= 0 && cls < FREECAM_Z_CLASSES) ? s_zFrames[cls] : 0;
}
unsigned camera_zclass_unreadable(void) { return s_zUnreadable; }
unsigned camera_zclass_us_last(void) { return s_zUsLast; }
unsigned camera_zclass_us_max(void) { return s_zUsMax; }

static long long cam_now_us(void) {
  static long long freqHz = 0;
  static bool tried = false;
  static bool broken = false;
  if (broken) return 0;
  if (!tried) {
    tried = true;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
      broken = true;
      ek_log("camera: QueryPerformanceFrequency failed, the ceiling "
             "classification's timing reads 0");
      return 0;
    }
    freqHz = (long long)f.QuadPart;
  }
  LARGE_INTEGER now;
  if (!QueryPerformanceCounter(&now)) {
    broken = true;
    ek_log("camera: QueryPerformanceCounter failed, the ceiling "
           "classification's timing reads 0");
    return 0;
  }
  return (long long)((double)now.QuadPart * 1000000.0 / (double)freqHz);
}

static FreecamEdge s_zEdges[EK_WALL_SLOTS];

static int cam_sector_holds(const EkSectorInfo& si, int x, int y) {
  if (!ekm_sector_chain_record_ok(si.wallptr, si.wallnum)) return -1;
  for (int k = 0; k < si.wallnum; k++) {
    const int w = si.wallptr + k;
    EkWallCorner a, b;
    int p2 = -1;
    if (!wallstate_corner(w, &a)) return -1;
    if (!wallstate_point2(w, &p2)) return -1;
    if (!ekm_point2_ok(p2, w, si.wallptr, si.wallnum)) return -1;
    if (!wallstate_corner(p2, &b)) return -1;
    s_zEdges[k].x1 = a.x;
    s_zEdges[k].y1 = a.y;
    s_zEdges[k].x2 = b.x;
    s_zEdges[k].y2 = b.y;
  }
  return freecam_inside(x, y, s_zEdges, si.wallnum);
}

// Every live sector whose footprint holds (x, y), each against its own slope-exact band. Unknown numsectors, an
// unreadable sector or an inexact band all count as inside: the engine's picture is kept.
static int cam_zclass_decide(int x, int y, int z) {
  const int ns = mapextent_numsectors();
  if (ns <= 0 || ns > EK_MAXSECTORS) return FREECAM_Z_INSIDE;
  FreecamZTally t = { 0, 0, 0, 0 };
  for (int s = 0; s < ns; s++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0 };
    FreecamZSample smp = { true, 0, 0, false };
    const int holds = sectorstate_info(s, &si) ? cam_sector_holds(si, x, y) : -1;
    if (holds < 0) {
      s_zUnreadable++;
      freecam_z_note(&t, smp, z);
      continue;
    }
    if (holds == 0) continue;
    FreecamBand band = { 0, 0, 0, 0 };
    EkSectorInfo bi = { 0, 0, 0, 0, 0 };
    if (cam_band_fill(s, x, y, &band, &bi)) {
      smp.ceilz = band.ceilz;
      smp.florz = band.florz;
      smp.exact = band.exact != 0;
    }
    freecam_z_note(&t, smp, z);
    if (t.inside > 0) break;
  }
  return freecam_z_class(t);
}

static bool s_saidNoGetzOrig = false;

static void EK_GetZsOfSlopeThunk(const void* sec, int dax, int day,
                                 int* ceilz, int* florz) {
  GetZsOfSlopePtrFn const orig = s_getzOrig;
  if (!orig) {
    // Unreachable: the address is captured before the patch goes in and cleared after it comes out. The slots are
    // still written so the renderer's locals are not garbage; a zero band takes the external arm, the behaviour
    // without this.
    if (ceilz) *ceilz = 0;
    if (florz) *florz = 0;
    if (!s_saidNoGetzOrig) {
      s_saidNoGetzOrig = true;
      ek_log("camera: the band-test thunk ran with no original to forward to, "
             "which nothing in this build can produce -- please report it. The "
             "frame draws the external arm, as it would without the mod.");
    }
    return;
  }

  // Asked first and unguarded on purpose: these are the engine's own arguments on its own call, so a fault is one the
  // game would take without the mod, and a __try would turn a reportable crash into a band nobody computed.
  orig(sec, dax, day, ceilz, florz);

  if (!freecam_widen_now(s_widenActive != 0)) return;
  if (!ceilz || !florz) return;

  const FreecamWidened w = freecam_band_widen(*ceilz, *florz, s_widenZ);
  *ceilz = w.ceilz;
  *florz = w.florz;
}

// Called once per session after both entry hooks are in: the site is reached only from inside the function whose
// entry the detour owns, so earlier would be a redirect with no scope.
static bool cam_install_redirect(const unsigned char* base) {
  const unsigned target = (unsigned)InterlockedCompareExchange(&s_getzRva, 0, 0);
  const unsigned site = (unsigned)InterlockedCompareExchange(&s_getzSiteRva, 0, 0);
  const bool sweepOk = InterlockedCompareExchange(&s_sweepOk, 0, 0) != 0;
  const bool spritesOk = InterlockedCompareExchange(&s_spritesOk, 0, 0) != 0;

  if (!freecam_redirect_install_ok(target != 0 && site != 0, sweepOk)) {
    ek_log("camera: the sky redirect stays OUT -- call site %s, whole-map sweep "
           "%s. The camera flies and the sky comes back only where a reachable "
           "sector's band already contains it, which is what this build did "
           "before the redirect existed",
           (target && site) ? "claimed" : "REFUSED",
           sweepOk ? "available" : "REFUSED");
    return false;
  }
  if (!spritesOk)
    ek_log("camera: and it goes in WITHOUT the sprite pass -- the sky and the "
           "whole map, with no actors in the sectors only the sweep reaches. "
           "That is the deliberate trade: losing distant actors is a smaller "
           "loss than losing the sky");

  s_getzOrig = (GetZsOfSlopePtrFn)(const void*)(base + target);
  if (!callsite_install((void*)(base + site), (void*)(base + target),
                        (void*)&EK_GetZsOfSlopeThunk, &s_getzPatch)) {
    s_getzOrig = NULL;
    ek_log("camera: the band test's call site at rva 0x%07X was NOT redirected; "
           "the game keeps its own call and the sky comes back only where a "
           "reachable sector's band contains the camera", site);
    return false;
  }

  ek_log("camera: the sky redirect is IN -- the band test's call at rva 0x%07X "
         "now reaches our thunk, which asks getzsofslopeptr at 0x%07X first and "
         "widens its answer to contain the drawn z while, and only while, the "
         "substituted draw is in flight", site, target);
  return true;
}

// The sky's answer for the menu: s_getzPatch.site is non-NULL exactly while the redirect is in. A sprite refusal
// costs actors, not the sky.
bool camera_sky_available(void) {
  return camera_wholemap_available() && s_getzPatch.site != NULL;
}

// yax_drawrooms is where a TROR map's real draw happens (the call this bracket arms is yax_preparedrawrooms' no-draw
// pass). Two independent facts must name that function or nothing new is armed. Published by the locate, latched by
// the render thread: end is written last and read first.
static volatile LONG s_yaxBegin = 0;
static volatile LONG s_yaxEnd = 0;
static volatile LONG s_yaxAnswered = 0;
static unsigned s_yaxB = 0, s_yaxE = 0;

bool camera_tror_base_answered(void) {
  return InterlockedCompareExchange(&s_yaxAnswered, 0, 0) != 0;
}
bool camera_tror_base_located(void) { return s_yaxEnd != 0; }
bool camera_yax_span(unsigned* begin, unsigned* end) {
  const unsigned e = (unsigned)InterlockedCompareExchange(&s_yaxEnd, 0, 0);
  const unsigned b = (unsigned)InterlockedCompareExchange(&s_yaxBegin, 0, 0);
  if (e == 0 || !begin || !end) return false;
  *begin = b;
  *end = e;
  return true;
}

static const unsigned EK_YAX_NEXT_CALL_WINDOW = 32;
static const int EK_YAX_MAX_SITES = 8;

static int cam_calls_in(const unsigned char* base, unsigned b, unsigned e,
                        unsigned target, unsigned* sites, int max,
                        int* faulted) {
  int n = 0;
  __try {
    for (unsigned q = b; q + 5 <= e; q++) {
      if (base[q] != 0xE8) continue;
      if (ekm_us_rel32_target(q, *(const int*)(base + q + 1)) != target) continue;
      if (n < max) sites[n] = q;
      n++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
  return n;
}

static unsigned cam_next_callee(const unsigned char* base, unsigned site,
                                int* faulted) {
  __try {
    for (unsigned q = site + 5; q < site + 5 + EK_YAX_NEXT_CALL_WINDOW; q++) {
      if (base[q] != 0xE8) continue;
      const unsigned t = ekm_us_rel32_target(q, *(const int*)(base + q + 1));
      if (t != 0 && ek_image_entry_containing(base, t) == t) return t;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
  return 0;
}

static void cam_yax_refuse(const char* why) {
  InterlockedExchange(&s_yaxAnswered, 1);
  ek_log("camera yax: REFUSED -- %s. TROR support is off (the row reads "
         "Unavailable): no base-level arming AND no no-draw forward, so TROR "
         "maps draw as they did before D11", why);
}

static void cam_locate_yax(const unsigned char* base) {
  const unsigned rooms = camera_roomsQ16_rva();
  const unsigned drawB = (unsigned)InterlockedCompareExchange(&s_drawBegin, 0, 0);
  const unsigned drawE = (unsigned)InterlockedCompareExchange(&s_drawEnd, 0, 0);
  if (!base || !rooms || drawE <= drawB) {
    cam_yax_refuse("G_DrawRooms or renderDrawRoomsQ16 is not located");
    return;
  }
  int faulted = 0;

  unsigned aSites[EK_YAX_MAX_SITES];
  const int aN = cam_calls_in(base, drawB, drawE, rooms, aSites, EK_YAX_MAX_SITES,
                              &faulted);
  unsigned factA = 0;
  int aDisagree = 0;
  for (int i = 0; i < aN && i < EK_YAX_MAX_SITES; i++) {
    const unsigned t = cam_next_callee(base, aSites[i], &faulted);
    if (t == 0 || (factA != 0 && t != factA)) { aDisagree = 1; break; }
    factA = t;
  }
  if (aDisagree || aN == 0 || aN > EK_YAX_MAX_SITES) factA = 0;

  EkCallCount cc;
  ek_image_count_calls(base, rooms, &cc);
  unsigned factB = 0, bSites[3] = { 0, 0, 0 };
  int bCandidates = 0;
  if (!cc.faulted && !cc.capped) {
    for (int k = 0; k < cc.owners; k++) {
      unsigned ob = 0, oe = 0;
      if (!ek_image_extent_containing(base, cc.owner[k], &ob, &oe)) continue;
      unsigned s[3];
      if (cam_calls_in(base, ob, oe, rooms, s, 3, &faulted) != 3) continue;
      bCandidates++;
      factB = cc.owner[k];
      bSites[0] = s[0]; bSites[1] = s[1]; bSites[2] = s[2];
    }
  }
  if (bCandidates != 1) factB = 0;

  if (faulted || cc.faulted) {
    cam_yax_refuse("a read of the image faulted mid-scan");
    return;
  }
  const unsigned yax = freecam_yax_agree(factA, factB);
  unsigned yb = 0, ye = 0;
  if (yax == 0 || !ek_image_extent_containing(base, yax, &yb, &ye) || yb != yax) {
    InterlockedExchange(&s_yaxAnswered, 1);
    ek_log("camera yax: REFUSED -- fact A (the callee after G_DrawRooms' %d "
           "drawrooms call(s)) names 0x%07X%s, fact B (the one function of %d "
           "with exactly three drawrooms calls) names 0x%07X with %d "
           "candidate(s); they must agree on one function with its own .pdata "
           "entry. TROR support is off (the row reads Unavailable): no "
           "base-level arming AND no no-draw forward, so TROR maps draw as they "
           "did before D11", aN, factA, aDisagree ? " (the sites DISAGREE)" : "",
           cc.owners, factB, bCandidates);
    return;
  }
  InterlockedExchange(&s_yaxBegin, (LONG)yb);
  InterlockedExchange(&s_yaxEnd, (LONG)ye);
  InterlockedExchange(&s_yaxAnswered, 1);
  ek_log("camera yax: yax_drawrooms at rva 0x%07X..0x%07X -- fact A: all %d "
         "drawrooms call(s) in G_DrawRooms (first at 0x%07X) are followed by a "
         "call to it; fact B: it is the one function of %d holding exactly "
         "three drawrooms calls (0x%07X, 0x%07X, 0x%07X; the last is the base "
         "level)", yb, ye, aN, aSites[0], cc.owners, bSites[0], bSites[1],
         bSites[2]);
}

static void cam_yax_latch(void) {
  if (s_yaxE != 0) return;
  const unsigned e = (unsigned)InterlockedCompareExchange(&s_yaxEnd, 0, 0);
  if (e == 0) return;
  s_yaxB = (unsigned)InterlockedCompareExchange(&s_yaxBegin, 0, 0);
  s_yaxE = e;
}

// A call whose return address is inside yax_drawrooms. True having armed and drawn it (or, in mode 2, skipped it);
// false having drawn nothing. The bracket is the player-view one minus what would destroy the other TROR levels: no
// backdrop clear, no ud.camera* write, no bisection rows. *result is the main pass's answer, or 0 for a mode 2 skip.
static bool cam_tror_base_draw(DrawRoomsQ16Fn orig, const void* ret, int x,
                               int y, int z, int angFix, int horizFix,
                               short sect, int32_t* result) {
  *result = 0;
  const unsigned char* const base = s_base;
  if (!base) return false;
  cam_yax_latch();
  const unsigned retRva = (unsigned)(size_t)((const unsigned char*)ret - base);
  if (!freecam_rva_in_span(retRva, s_yaxB, s_yaxE)) return false;
  s_trorSeen++;
  s_trorSeenFrame++;
  const FreecamViewArgs args = { x, y, z, angFix, horizFix, (int)sect };
  const bool located =
      ekm_tror_support_located(cutaway_nodraw_rva() != 0, s_yaxE != 0);
  const int mode = g_cfg.freecamtrornodraw;
  if (!freecam_is_tror_base(located, freecam_tror_on(mode),
                            s_armedHave != 0, retRva, s_yaxB, s_yaxE, args,
                            s_armedArgs)) {
    // Mode 2, a diagnostic: an other-level draw pass returns without the engine, to test whether those passes cause
    // the residual breakage. g_nodraw is read only in this mode.
    if (freecam_tror_mode_clamped(mode) == FREECAM_TROR_SKIP_LEVELS &&
        freecam_tror_skip_level(mode, located, s_armedHave != 0,
                                freecam_rva_in_span(retRva, s_yaxB, s_yaxE),
                                cutaway_nodraw_value() == 0, args.sect)) {
      s_trorSkipped++;
      s_trorSkippedFrame++;
      storeyflush_pass_skipped();
      return true;
    }
    s_trorRefused++;
    s_trorRefusedFrame++;
    return false;
  }
  s_armedHave = 0;
  s_trorArmed++;
  s_trorArmedFrame++;

  FogHold fog;
  const bool fogHeld =
      freecam_fog_substitute(true, g_cfg.freecamfog != 0 || s_fbArmed != 0) &&
      cam_fog_arm(&fog);
  s_widenZ = z;
  s_widenActive =
      freecam_redirect_arm(g_cfg.freecamwholemap != 0,
                           InterlockedCompareExchange(&s_sweepOk, 0, 0) != 0)
          ? 1 : 0;
  s_substActive = 1;

  // Extra pass first: the engine already holds yax_polymostclearzbuffer at 0 here, so the two passes merge by depth.
  // No spritesortcnt hold: the base level collects no sprites.
  if (cam_extra_decide(true, false) == FREECAM_EXTRA_YES) {
    cam_extra_pass(orig, x, y, z, angFix, horizFix, sect);
    s_xpMade[1]++;
  }

  perf_zone_begin(EK_PZ_ENGINE);
  *result = orig(x, y, z, angFix, horizFix, sect);
  perf_zone_end(EK_PZ_ENGINE, 1);
  cutpaint_flip_pass_end();

  s_substActive = 0;
  s_widenActive = 0;
  if (fogHeld) cam_fog_release(&fog);
  return true;
}

static int32_t EK_RoomsQ16Detour(int x, int y, int z, int angFix,
                                 int horizFix, short sect) {
  const void* const ret = _ReturnAddress();
  const Q16Stamp pfStamp;

  DrawRoomsQ16Fn const orig = s_roomsQ16Orig;
  if (!orig) return 0;

  storeyflush_on_q16(ret, (int)sect);
  // Before the camera's fast path: the defect is on foot. project_begin projects from this record; with the camera up
  // the substituted draw records below instead.
  const bool camUp = s_active != 0;
  const bool engNoted =
      !camUp && cam_engine_view_note(ret, x, y, z, angFix, horizFix, (int)sect);

  s_widenActive = 0;
  s_substActive = 0;
  s_extraPassActive = 0;

  // Re-proved every frame, not latched: a kept verdict would leave the row enabled over the automap or after a
  // renderer change and lock out keyboard and mouse with no change to the view. camera_frame counts idle frames and
  // revokes.
  if (s_roomsQ16Idle) InterlockedExchange(&s_roomsQ16Idle, 0);
  if (!s_drawSeenOk) {
    InterlockedExchange(&s_drawSeenOk, 1);
    ek_log("camera: the hooked drawrooms was reached, so the world is being "
           "drawn and the free camera row is available (this does NOT prove "
           "the renderer is Polymer -- see camera.cpp)");
  }

  if (!camUp) {
    perf_zone_begin(EK_PZ_ENGINE_PLAIN);
    const int32_t r = orig(x, y, z, angFix, horizFix, sect);
    perf_zone_end(EK_PZ_ENGINE_PLAIN, 1);
    if (engNoted && g_cfg.debuglog) cam_engine_view_vp_note();
    return r;
  }

  if (!detour_is_player_view(ret, x, y, z, angFix, horizFix, (int)sect)) {
    int32_t trorResult = 0;
    const bool trorDrawn = cam_tror_base_draw(orig, ret, x, y, z, angFix,
                                              horizFix, sect, &trorResult);
    if (trorDrawn) return trorResult;
    if (!s_saidNotView) {
      s_saidNotView = true;
      ek_log("camera: a drawrooms call that is NOT the player's own view -- a "
             "mirror, a TROR level pass or a viewscreen (ud.camerasprite) -- "
             "draws unsubstituted, as it must. This is not the camera declining "
             "to fly.");
    }
    perf_zone_begin(EK_PZ_ENGINE_PLAIN);
    const int32_t r = orig(x, y, z, angFix, horizFix, sect);
    perf_zone_end(EK_PZ_ENGINE_PLAIN, 1);
    return r;
  }

  s_armedHave = 0;
  s_zClass = FREECAM_Z_INSIDE;

  FreecamPose prevPose, curPose;
  if (!camera_pose_read(&prevPose, &curPose)) {
    if (!s_saidNoPose) {
      s_saidNoPose = true;
      ek_log("camera: the pose would not read under the seqlock; this frame "
             "draws the player's own view");
    }
    perf_zone_begin(EK_PZ_ENGINE_PLAIN);
    const int32_t r = orig(x, y, z, angFix, horizFix, sect);
    perf_zone_end(EK_PZ_ENGINE_PLAIN, 1);
    return r;
  }

  // The drawn pose is between the two tics, not at the later one: the camera steps at 30 Hz, one new pose every eight
  // frames at 240 fps. The player's own view blends opos/oq16ang to pos/q16ang by this ratio in G_DrawRooms; the
  // camera matches it (same sampling and interpolation, up to one tic of latency).
  const bool fresh = s_smoothFresh;
  if (!fresh && !s_saidNoRatio) {
    s_saidNoRatio = true;
    ek_log("camera: no smoothRatio was captured for this frame -- no path in "
           "this build can produce that, so please report it. The camera draws "
           "at the tic pose unblended, which is safe but will look stepped at "
           "high frame rates.");
  }
  const FreecamPose pose =
      fresh ? freecam_pose_lerp(prevPose, curPose, s_smoothRatio) : curPose;

  // The camera's sector, seeded from the camera's own last one. The camera does not clip, so crossing a boundary wall
  // its point is in no sector and the resolve fails; the pick rule keeps the last sector it was legitimately in
  // (refusing snapped the view back to the player at every wall).
  int resolved = -1;
  const bool resolvedOk = ek_updatesector(pose.x, pose.y, s_camSect, &resolved);
  const FreecamSectorPick pick =
      freecam_sector_pick(resolvedOk, resolved, s_camSect, EK_MAXSECTORS);
  s_camSect = pick.last;

  cam_zvs_measure(pick.sect, pose.z);

  {
    const long long t0 = cam_now_us();
    s_zClass = cam_zclass_decide(pose.x, pose.y, pose.z);
    const long long dt = cam_now_us() - t0;
    s_zUsLast = (dt > 0 && dt < 0x7FFFFFFF) ? (unsigned)dt : 0u;
    if (s_zUsLast > s_zUsMax) s_zUsMax = s_zUsLast;
    s_zFrames[s_zClass]++;
  }

  if (pick.sect < 0) {
    if (!s_saidNoSector) {
      s_saidNoSector = true;
      ek_log("camera: no sector to draw from at (%d,%d) and none resolved yet "
             "this flight -- the frame draws the player's own view", pose.x,
             pose.y);
    }
    perf_zone_begin(EK_PZ_ENGINE_PLAIN);
    const int32_t r = orig(x, y, z, angFix, horizFix, sect);
    perf_zone_end(EK_PZ_ENGINE_PLAIN, 1);
    return r;
  }

  // The sector whose band contains the camera brings the sky back: polymer sends a z outside its sector's band down
  // the external arm before the sky is drawn; a sector whose band contains the z makes the normal arm run. Nothing
  // qualifying returns pick.sect unchanged. Bands come from the engine's getzsofslopeptr.
  const int drawSect = cam_band_sect(pick.sect, pose.x, pose.y, pose.z);

  const int subAng = ekm_f16(pose.ang);
  const int subHoriz = ekm_f16(pose.horiz);
  const short subSect = (short)drawSect;

  // ud.camera* is written and left written (see the section header); no termination handler is needed since a failed
  // write draws the player's own view. The ud.camera field bisection: a pure selection holding back at most one of
  // z/horiz/ang/sect, passing the engine's own argument instead (x and y never).
  const FreecamUdArgs udArgs =
      freecam_ud_pick(g_cfg.freecamud, pose.z, subAng, subHoriz, drawSect, z,
                      angFix, horizFix, sect);

  if (!ud_camera_write(pose.x, pose.y, udArgs.z, udArgs.ang, udArgs.horiz,
                       udArgs.sect)) {
    if (!s_saidNoWrite) {
      s_saidNoWrite = true;
      ek_log("camera: the camera view could not be written into ud.camera*; "
             "the frame draws the player's own view and nothing was left "
             "changed");
    }
    perf_zone_begin(EK_PZ_ENGINE_PLAIN);
    const int32_t r = orig(x, y, z, angFix, horizFix, sect);
    perf_zone_end(EK_PZ_ENGINE_PLAIN, 1);
    return r;
  }

  // Remove fog, armed here and only here (write/call/restore, one scalar). Every early return is upstream, so a frame
  // handed back to the game never touches it. It covers the sweep too, or the added sectors would be fogged and the
  // rest not. Fullbright arms it here as well: g_visibility is rewritten at G_DrawRooms' head.
  FogHold fog;
  const bool fogHeld =
      freecam_fog_substitute(true, g_cfg.freecamfog != 0 || s_fbArmed != 0) &&
      cam_fog_arm(&fog);

  // The band widening, raised here only, around this one call: every early return is upstream, and no other caller of
  // that site sees it. The z is the one polymer compares the band against. Armed by the sweep, not the camera
  // (freecam_redirect_arm).
  s_sweptEyeX = pose.x;
  s_sweptEyeY = pose.y;
  s_sweptEyeZ = pose.z;

  s_widenZ = pose.z;
  s_widenActive =
      freecam_redirect_arm(g_cfg.freecamwholemap != 0,
                           InterlockedCompareExchange(&s_sweepOk, 0, 0) != 0)
          ? 1 : 0;

  s_substActive = 1;

  cam_clear_backdrop();

  // The view-substitution bisection: holds back at most one of z/horiz/ang/sect, passing this detour's own parameter.
  // x and y are never held back. Applied only at the one substituting call site. No cause asserted.
  const FreecamSubstArgs substArgs =
      freecam_subst_pick(g_cfg.freecamsubst, pose.z, subAng, subHoriz,
                         subSect, z, angFix, horizFix, sect);
  if (cam_subst_due()) {
    ek_log("camera subst: state=%s z=%d ang=%d horiz=%d sect=%d",
           cam_subst_label(g_cfg.freecamsubst), substArgs.z, substArgs.ang,
           substArgs.horiz, (int)substArgs.sect);
    ek_log("camera ud: state=%s z=%d ang=%d horiz=%d sect=%d",
           cam_ud_label(g_cfg.freecamud), udArgs.z, udArgs.ang, udArgs.horiz,
           udArgs.sect);
    if (g_cfg.debuglog)
      ek_log("camera tror: %u yax_drawrooms draw(s) seen (most in one frame "
             "%u) = %u armed as the base level (most %u) + %u other-level "
             "pass(es) SKIPPED (most %u) + %u refused by the rule (most %u); "
             "TROR support %d = %s, yax_drawrooms %s%s",
             s_trorSeen, s_trorSeenMax, s_trorArmed, s_trorArmedMax,
             s_trorSkipped, s_trorSkippedMax, s_trorRefused, s_trorRefusedMax,
             freecam_tror_mode_clamped(g_cfg.freecamtrornodraw),
             freecam_tror_mode_name(g_cfg.freecamtrornodraw),
             s_yaxE != 0 ? "located" : "NOT LOCATED",
             freecam_tror_mode_clamped(g_cfg.freecamtrornodraw) ==
                     FREECAM_TROR_SKIP_LEVELS
                 ? ". DIAGNOSTIC, NOT A FIX: other TROR levels are not drawn, "
                   "and after the first skipped pass renderDrawMasks redraws "
                   "the masked walls the last no-draw pass collected (stale)"
                 : "");
  }

  {
    const FreecamViewArgs drawn = { pose.x, pose.y, substArgs.z, substArgs.ang,
                                    substArgs.horiz, (int)substArgs.sect };
    const FreecamViewArgs written = { pose.x, pose.y, udArgs.z, udArgs.ang,
                                      udArgs.horiz, udArgs.sect };
    cam_engine_view_store(drawn.x, drawn.y, drawn.z, drawn.angFix,
                          drawn.horizFix, drawn.sect,
                          freecam_same_view(drawn, written) ? 1 : 0);
  }

  int32_t mainResult = 0;
  {
    int* sprCnt = NULL;
    int sprSaved = 0;
    const bool sprOk = cam_sprite_count_read(&sprCnt, &sprSaved);
    const int xp = cam_extra_decide(false, sprOk);
    ZClearHold zhold;
    if (xp == FREECAM_EXTRA_YES) {
      {
        SpriteCountHold shold(sprCnt, sprSaved);
        cam_extra_pass(orig, pose.x, pose.y, substArgs.z, substArgs.ang,
                       substArgs.horiz, substArgs.sect);
      }
      s_xpMade[0]++;
      if (cutaway_zclear_suspend(&zhold.saved)) zhold.held = true;
      else s_xpSuspendFailed[0]++;
    }

    perf_zone_begin(EK_PZ_ENGINE);
    mainResult = orig(pose.x, pose.y, substArgs.z, substArgs.ang,
                      substArgs.horiz, substArgs.sect);
    perf_zone_end(EK_PZ_ENGINE, 1);
    cutpaint_flip_pass_end();
  }

  s_armedArgs.x = pose.x;
  s_armedArgs.y = pose.y;
  s_armedArgs.z = substArgs.z;
  s_armedArgs.angFix = substArgs.ang;
  s_armedArgs.horizFix = substArgs.horiz;
  s_armedArgs.sect = (int)substArgs.sect;
  s_armedHave = 1;

  cam_xp_report();

  s_substActive = 0;

  // Gotsector save/restore seam (2 of 2), deliberately empty: if room-over-room passes must keep working, the 512
  // bytes nocull.cpp snapshotted before its sweep are copied back here, after the draw (polymost_drawalls re-reads
  // gotsector later, so earlier would make it re-scan the map).

  s_widenActive = 0;

  // The rest of the map on top of the sky the call above drew, on the substituted path only. With the sky redirect
  // in, that call took the normal arm, so this adds exactly what its portal walk skipped; without it the original may
  // have gone external and some geometry draws twice (depth-correct).
  if (g_cfg.freecamwholemap) cam_draw_whole_map();

  if (fogHeld) cam_fog_release(&fog);
  return mainResult;
}

static_assert(std::is_same<decltype(EK_RoomsQ16Detour),
                           int32_t(int, int, int, int, int, short)>::value,
              "EK_RoomsQ16Detour must return renderDrawRoomsQ16's int32_t");
static_assert(std::is_same<DrawRoomsQ16Fn,
                           int32_t (*)(int, int, int, int, int, short)>::value,
              "DrawRoomsQ16Fn must return renderDrawRoomsQ16's int32_t");


static void cam_start(const PlayerSnap& snap) {
  s_cam.x = snap.x;
  s_cam.y = snap.y;
  s_cam.z = snap.z;
  s_cam.ang = (snap.ang >> 16) & (FREECAM_ANG_UNITS - 1);
  s_cam.pitch = freecam_horiz_to_pitch((snap.horiz + snap.horizoff) >> 16);
  s_cam.yawRem = 0;
  s_cam.pitchRem = 0;
  s_camSect = (snap.sect >= 0 && snap.sect < EK_MAXSECTORS) ? snap.sect : -1;

  freecam_pose_carry(&s_cam);
  s_transportHold = 0;
  s_cam.active = true;
  ek_log("camera: flying from (%d,%d,%d) ang %d pitch %d",
         s_cam.x, s_cam.y, s_cam.z, s_cam.ang, s_cam.pitch);
}

static void cam_stop(const char* why) {
  s_cam.active = false;
  ek_log("camera: down (%s)", why);
}

// Two records, not one: the live plan moves every frame the marker's row is on, the said outcome only when the key
// fired. One record would pair the live plan's words with the last press's clock and overwrite a refused press
// unread. Each has its own counter (different publishers). Game thread writes, the draw reads.
static volatile LONG s_tpPlanSeq = 0;
static volatile LONG s_tpPlanHave = 0;
static EkTeleportPlan s_tpPlan;

static volatile LONG s_tpSaidSeq = 0;
static volatile LONG s_tpHave = 0;
static EkTeleportPlan s_tpSaid;
static unsigned s_tpMs = 0;

static void cam_teleport_publish_plan(const EkTeleportPlan& p) {
  const unsigned begin =
      freecam_seq_write_begin((unsigned)InterlockedCompareExchange(&s_tpPlanSeq, 0, 0));
  InterlockedExchange(&s_tpPlanSeq, (LONG)begin);
  s_tpPlan = p;
  InterlockedExchange(&s_tpPlanSeq, (LONG)freecam_seq_write_end(begin));
  InterlockedExchange(&s_tpPlanHave, 1);
}

static void cam_teleport_say(const EkTeleportPlan& p) {
  const unsigned begin =
      freecam_seq_write_begin((unsigned)InterlockedCompareExchange(&s_tpSaidSeq, 0, 0));
  InterlockedExchange(&s_tpSaidSeq, (LONG)begin);
  s_tpSaid = p;
  s_tpMs = wallclock_ms();
  InterlockedExchange(&s_tpSaidSeq, (LONG)freecam_seq_write_end(begin));
  InterlockedExchange(&s_tpHave, 1);
}

int camera_teleport_plan_read(EkTeleportPlan* plan, int* have) {
  if (!plan || !have) return 0;
  for (int i = 0; i < EK_CAM_POSE_TRIES; i++) {
    const unsigned before = (unsigned)InterlockedCompareExchange(&s_tpPlanSeq, 0, 0);
    const EkTeleportPlan p = s_tpPlan;
    const int hv = InterlockedCompareExchange(&s_tpPlanHave, 0, 0) ? 1 : 0;
    const unsigned after = (unsigned)InterlockedCompareExchange(&s_tpPlanSeq, 0, 0);
    if (freecam_seq_read_ok(before, after)) {
      *plan = p;
      *have = hv;
      return 1;
    }
  }
  return 0;
}

int camera_teleport_outcome(EkTeleportPlan* plan, unsigned* setMs, int* have) {
  if (!plan || !setMs || !have) return 0;
  for (int i = 0; i < EK_CAM_POSE_TRIES; i++) {
    const unsigned before = (unsigned)InterlockedCompareExchange(&s_tpSaidSeq, 0, 0);
    const EkTeleportPlan p = s_tpSaid;
    const unsigned ms = s_tpMs;
    const int hv = InterlockedCompareExchange(&s_tpHave, 0, 0) ? 1 : 0;
    const unsigned after = (unsigned)InterlockedCompareExchange(&s_tpSaidSeq, 0, 0);
    if (freecam_seq_read_ok(before, after)) {
      *plan = p;
      *setMs = ms;
      *have = hv;
      return 1;
    }
  }
  return 0;
}

// The current landing plan from the camera's live pose, shared by the key and the marker so they cannot disagree. A
// refused plan answers with its reason. outBand is for the key's log line: the floor and ceiling the refusal was
// judged against; sect stays -1 when the walk refused before the band was read.
static EkTeleportPlan cam_teleport_plan_now(FreecamBand* outBand) {
  EkTeleportPlan refused = { 0, 0, 0, 0, -1, EK_TP_NOTHING };
  if (outBand) { outBand->sect = -1; outBand->ceilz = 0; outBand->florz = 0; outBand->exact = 0; }

  FreecamPose pose;
  const int havePose = camera_viewpoint_read(&pose);
  if (!havePose) return refused;

  const EkHitscanDir dir = ekm_tp_direction(pose.ang, pose.horiz);

  // The ray must start from the sector that holds its own start point: pose is the tic pose and s_camSect was
  // resolved from the blended drawn point, up to a tic apart, so near a portal Build would walk from a sector the
  // point is not in.
  int startSect = s_camSect;
  const bool haveStart = ek_updatesector(pose.x, pose.y, s_camSect, &startSect);
  if (!haveStart || startSect < 0) return refused;

  EkHitData hit = { 0, 0, 0, -1, -1, -1 };
  const bool cast = ek_hitscan(pose.x, pose.y, pose.z, startSect,
                               dir.vx, dir.vy, dir.vz, &hit);
  if (!cast) return refused;

  const EkTeleportAim aim = ekm_tp_aim(hit.sect, hit.wall, hit.sprite,
                                       hit.x, hit.y, hit.z, dir.vx, dir.vy);
  if (!aim.ok) { refused.reason = aim.reason; return refused; }

  int sect = -1;
  const bool inMap = ek_updatesector(aim.x, aim.y, s_camSect, &sect);
  if (!inMap) sect = -1;

  if (sect < 0) {
    EkTeleportPlan noSect = { 0, 0, 0, 0, -1, EK_TP_NO_SECTOR };
    return noSect;
  }

  // The band must be exact: when a sloped sector's slope call failed, cam_band_fill sets `exact` 0 and a flat
  // fallback would land the player at the wrong height, the one error this plan exists to avoid. Its own words: the
  // point IS in the map, so "outside the map" would mislead; what failed is the floor read.
  FreecamBand band = { -1, 0, 0, 0 };
  EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
  const bool haveBand = cam_band_fill(sect, aim.x, aim.y, &band, &si);
  if (haveBand && outBand) *outBand = band;
  if (!haveBand || !band.exact) {
    EkTeleportPlan noFloor = { 0, 0, 0, 0, -1, EK_TP_NO_FLOOR };
    return noFloor;
  }

  return ekm_tp_plan(aim.x, aim.y, aim.z, sect, band.florz, band.ceilz, EK_TP_PHEIGHT);
}

// The teleport, from camera_frame only: the game thread after the tic, outside the G_DrawRooms bracket (camera_tic's
// call site is inside it). menuOpen is camera_frame's own answer, so the teleport and the camera agree on whether the
// panel is up.
static void camera_teleport_frame(bool menuOpen) {
  const int edge = input_teleport_take();
  const int fire = ekm_tp_fire_rule(camera_active() ? 1 : 0, edge, menuOpen ? 1 : 0);

  // The marker needs a plan every frame, and it must be the same plan the key acts on. The key's path pays nothing
  // while the row is off: a plan is a hitscan, an ek_updatesector and a cstat round trip (ek_hitscan clears and
  // restores the player sprite's blocking bits per cast).
  const int wantMark =
      (camera_active() && g_cfg.freecamteleportmarker != 0 && !menuOpen) ? 1 : 0;
  if (!fire && !wantMark) return;

  FreecamBand band = { -1, 0, 0, 0 };
  const EkTeleportPlan plan = cam_teleport_plan_now(&band);
  cam_teleport_publish_plan(plan);
  if (!fire) return;

  EkTeleportPlan outcome = plan;
  if (plan.ok && !player_write_pos(plan)) {
    outcome.ok = 0;
    outcome.reason = EK_TP_NO_WRITE;
  }
  // One line per press, inside the key's branch like the say: a refusal on hardware is otherwise four words under a
  // crosshair. The reason, the chosen point and the floor/ceiling tell "too tight" from a bad height. Flying never
  // logs.
  if (g_cfg.debuglog)
    ek_log("teleport: %s -- point %d,%d,%d sector %d, floor %d ceiling %d%s",
           ekm_tp_has_notice(outcome) ? outcome.reason : "done",
           plan.x, plan.y, plan.z, plan.sect, band.florz, band.ceilz,
           band.sect < 0 ? " (band not read)" : "");
  cam_teleport_say(outcome);
}

void camera_frame(void) {
  const bool on = g_cfg.freecamon != 0;

  // Drained before the early-out: input.cpp records no toggle edge while freecamon is clear, but the row is written
  // on the render thread and read on the window thread, so an edge can be banked in the frame the row goes to zero
  // and would start the camera unbidden later.
  const bool keyEdge = input_freecam_toggle_take() > 0;

  // The paste latch, drained for the same reason; the early-out drops it, which is right.
  FreecamPose want;
  const bool wantPose = cam_take_request(&want);

  // Belt for a path this function cannot reach with a hold in flight: the row going off lands the camera through
  // freecam_force_off_rule and shot_frame at the end of that frame has already given the hud back.
  if (!on && !s_cam.active) { shot_frame(false, false); return; }

  PlayerSnap snap;
  int hp = 0, pal = 0, gm = 0, newowner = 0, overShoulder = 0;
  const bool snapOk = player_snap(&snap);
  const bool bodyOk = player_read_body(&hp, &pal);
  const bool gmOk   = player_read_gm(&gm);
  const bool modeOk = player_read_draw_mode(&newowner, &overShoulder);

  const bool dead = bodyOk && hp <= 0;

  // Not esp_level_loaded, deliberately: that is exact equality with MODE_GAME, so it is false over the game's pause
  // menu, and being paused is no reason to drop a camera already up. ekm_level_loading (a level being built) is what
  // strands it.
  bool mapOrHelp = false;
  const bool drawn = camera_installed() && snapOk && gmOk &&
                     !ekm_level_loading(gm, gmOk) &&
                     !(ud_map_or_help(&mapOrHelp) && mapOrHelp);
  if (drawn) {
    const FreecamDrawSeenVerdict verdict =
        freecam_draw_seen_verdict((int)s_roomsQ16Idle, FREECAM_DRAW_IDLE_FRAMES);
    if (verdict.idle != (int)s_roomsQ16Idle)
      InterlockedExchange(&s_roomsQ16Idle, (LONG)verdict.idle);
    if (verdict.revoke) {
      const bool was = s_drawSeenOk != 0;
      if (was) InterlockedExchange(&s_drawSeenOk, 0);
      ek_log("camera: the hooked drawrooms has %s been reached in %d drawn "
             "frames, so no frame appears to have been drawn there. The free "
             "camera has no pose to substitute: the row now reads Unavailable "
             "and the camera key is the game's again.",
             was ? "not" : "never", FREECAM_DRAW_IDLE_FRAMES);
    }
  }

  const bool valid = camera_available() && snapOk && bodyOk && gmOk && modeOk &&
                     !ekm_level_loading(gm, gmOk) &&
                     freecam_draw_reachable(newowner, overShoulder);

  const bool gmMenuOpen = gmOk && (gm & EK_MODE_MENU) != 0;
  const bool menuOpen = input_panel_open() || gmMenuOpen || console_capturing();

  if (freecam_force_off_rule(s_cam.active, on, dead, valid)) {
    cam_stop(!on ? "the row was switched off"
                 : dead ? "the player died"
                        : "the player, the level or the draw path stopped reading");
  } else if (s_cam.active) {
    if (freecam_exit_rule(true, keyEdge, menuOpen)) cam_stop("the key");
  } else if (freecam_enter_rule(on, keyEdge, menuOpen, dead, valid)) {
    cam_start(snap);
  } else if (keyEdge) {
    if (g_cfg.debuglog)
      ek_log("camera: entry refused -- row %s, draw hook %s, menu %s, dead %s, "
             "snap %s, body %s, "
             "gm %s, draw mode %s, level loading %s, viewscreen %s, third person %s",
             on ? "on" : "OFF",
             !camera_installed() ? "NOT INSTALLED"
                                 : (s_drawSeenOk ? "installed"
                                                : "installed, drawrooms not reached"),
             menuOpen ? "OPEN" : "shut", dead ? "YES" : "no",
             snapOk ? "ok" : "FAILED", bodyOk ? "ok" : "FAILED",
             gmOk ? "ok" : "FAILED", modeOk ? "ok" : "FAILED",
             ekm_level_loading(gm, gmOk) ? "YES" : "no",
             (modeOk && newowner >= 0) ? "YES" : "no",
             (modeOk && overShoulder != 0) ? "YES" : "no");
  }

  if (wantPose) {
    if (!s_cam.active) {
      // Refused, and said so: banking it until the next entry would act minutes later, and starting the camera from
      // here would go behind freecam_enter_rule. Not behind g_cfg.debuglog: it answers a key the player just pressed.
      ek_log("camera: a viewpoint was pasted while the camera is down -- press "
             "the camera key first. Nothing moved.");
    } else {
      s_cam.x = want.x;
      s_cam.y = want.y;
      s_cam.z = want.z;
      s_cam.ang = want.ang & (FREECAM_ANG_UNITS - 1);
      s_cam.pitch = freecam_horiz_to_pitch(want.horiz);
      s_cam.yawRem = 0;
      s_cam.pitchRem = 0;
      freecam_pose_carry(&s_cam);
      // s_camSect is deliberately left alone: the one refusal freecam_sector_pick still has (no valid sector) draws
      // the player's view. A stale sector is re-resolved next frame.
      ek_log("camera: flown to a pasted viewpoint (%d,%d,%d) ang %d pitch %d",
             s_cam.x, s_cam.y, s_cam.z, s_cam.ang, s_cam.pitch);
    }
  }

  // What a copy would take, published every frame the row is on: the camera's pose while it flies and the player's
  // while it does not. A frame whose player read failed publishes nothing, so the last good viewpoint stands.
  if (s_cam.active) {
    FreecamPose vp;
    vp.x = s_cam.x;
    vp.y = s_cam.y;
    vp.z = s_cam.z;
    vp.ang = s_cam.ang & (FREECAM_ANG_UNITS - 1);
    vp.horiz = freecam_pitch_to_horiz(s_cam.pitch);
    cam_publish_viewpoint(vp);
  } else if (snapOk) {
    FreecamPose vp;
    vp.x = snap.x;
    vp.y = snap.y;
    vp.z = snap.z;
    vp.ang = (snap.ang >> 16) & (FREECAM_ANG_UNITS - 1);
    vp.horiz = (snap.horiz + snap.horizoff) >> 16;
    cam_publish_viewpoint(vp);
  }

  // Screenshot mode on this frame's camera state, below every entry, exit and force-off so the hud returns on the
  // frame the camera comes down. shot_menu_term(gmMenuOpen), not menuOpen: opening our panel mid-flight must not give
  // the hud back.
  shot_frame(s_cam.active != 0, shot_menu_term(gmMenuOpen));

  camera_tic();

  camera_teleport_frame(menuOpen);
}

bool camera_installed(void) { return s_roomsQ16Orig != NULL; }

bool camera_available(void) {
  return s_roomsQ16Orig != NULL && s_drawSeenOk != 0 &&
         InterlockedCompareExchange64(&s_otclkAddr, 0, 0) != 0;
}

// Follow teleporters needs the two sprite-chain arrays, decoded out of the inlined polymer_scansprites
// (camerascan.cpp). The scans it also needs (sprite[], sector[], actor[]) are not terms: they come up per level, and
// a row Unavailable in the main menu would be saying something about the map.
bool camera_transport_available(void) {
  return camera_available() && s_base != NULL &&
         InterlockedCompareExchange(&s_headSlotRva, 0, 0) != 0 &&
         InterlockedCompareExchange(&s_nextSlotRva, 0, 0) != 0;
}

// The install-time unwind is the opposite order to remove. camera_remove clears the trampoline pointer before
// hook_remove (the process is going away). Here it keeps running, and hook_remove can fail leaving the entry patched,
// so clearing first would leave a NULL original and the detour would draw no world.
static void unwind_draw_hook(void) {
  if (hook_remove(&s_hook)) {
    s_drawOrig = NULL;
    return;
  }
  ek_log("camera: G_DrawRooms could not be unpatched after a failed install -- "
         "the entry still reaches this module, so the trampoline pointer is "
         "KEPT and the detour forwards every frame to the engine's own draw. "
         "The free camera stays unavailable and the picture is the game's");
}

bool camera_install(void) {
  if (s_attempted) return s_drawOrig != NULL;

  if (!camera_scan()) return false;

  s_attempted = true;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) {
    ek_log("camera: no host module; G_DrawRooms is not hooked and the free "
           "camera stays unavailable");
    return false;
  }

  const unsigned entry = camera_rva();
  if (!hook_install((void*)(base + entry), (void*)&EK_DrawRoomsDetour, &s_hook)) {
    ek_log("camera: G_DrawRooms at rva 0x%07X was NOT hooked; the game keeps "
           "its own draw and the free camera stays unavailable", entry);
    return false;
  }

  DrawRoomsFn const tramp = (DrawRoomsFn)hook_trampoline(&s_hook);
  if (!tramp) {
    // Cannot happen after a successful install; being wrong leaves a patched draw call with nowhere to return to, so
    // it is unpatched again.
    if (!hook_remove(&s_hook))
      ek_log("camera: G_DrawRooms has no trampoline AND could not be unpatched "
             "-- the entry still jumps into this module with nowhere to "
             "return to. The free camera stays unavailable");
    ek_log("camera: G_DrawRooms was patched but has no trampoline; unpatched "
           "again and the free camera stays unavailable");
    return false;
  }
  s_drawOrig = tramp;

  // Both or neither: with its substitution retired a G_DrawRooms patch alone just passes arguments through, a live
  // five-byte rewrite doing nothing. The first comes out if the second will not go in.
  const unsigned poly = camera_roomsQ16_rva();
  if (!hook_install((void*)(base + poly), (void*)&EK_RoomsQ16Detour,
                    &s_roomsQ16Hook)) {
    unwind_draw_hook();
    ek_log("camera: polymer_drawrooms at rva 0x%07X was NOT hooked; G_DrawRooms "
           "was unpatched again and the free camera stays unavailable", poly);
    return false;
  }

  DrawRoomsQ16Fn const polyTramp = (DrawRoomsQ16Fn)hook_trampoline(&s_roomsQ16Hook);
  if (!polyTramp) {
    if (!hook_remove(&s_roomsQ16Hook))
      ek_log("camera: polymer_drawrooms has no trampoline AND could not be "
             "unpatched -- its entry still jumps into this module with nowhere "
             "to return to");
    unwind_draw_hook();
    ek_log("camera: polymer_drawrooms was patched but has no trampoline; both "
           "hooks were taken out again and the free camera stays unavailable");
    return false;
  }

  s_roomsQ16Orig = polyTramp;
  ek_log("camera: G_DrawRooms hooked at rva 0x%07X (no longer substituting) and "
         "the hooked drawrooms at rva 0x%07X (the pose goes in here); the row "
         "reads Unavailable until a drawn frame reaches the second -- which "
         "proves a frame was drawn there, not which renderer drew it (see "
         "s_drawSeenOk's declaration)", entry, poly);

  cam_install_redirect(base);

  cam_locate_yax(base);

  // One-time diagnostic. polymer.cpp:5359's operator-precedence bug (`>` binds tighter than `?:` in the
  // uniform_visibility ternary) makes the art-tile fog term zero correctly only at the compiled default
  // r_usenewshading == 4; the hightile uniform_linearFog assignment (:5461) has the same dependency.
  cvars_scan();
  int usns = 4;
  if (cvar_get_int("r_usenewshading", &usns) && usns != 4) {
    ek_log("camera: r_usenewshading is %d, not the compiled default 4 -- "
           "Remove Fog may leave the art-tile path's fog term partly on "
           "(polymer.cpp:5359, :5461). Set r_usenewshading back to 4 if the "
           "fog only half goes away.", usns);
  }
  return true;
}

void camera_remove(void) {
  // The call-site redirect comes out first, and the order is load-bearing: the site is only reached from inside the
  // function whose entry the detour owns, so unpatching the entry first would let the engine reach a call whose
  // operand points at a stub page about to be freed.
  if (s_getzPatch.site) {
    if (callsite_remove(&s_getzPatch)) {
      s_getzOrig = NULL;
    } else {
      ek_log("camera: the band test's call site could not be put back; it "
             "still reaches our thunk, which forwards to the engine unchanged");
    }
  }
  s_widenActive = 0;
  s_substActive = 0;

  // The pointer goes first, the opposite of unwind_draw_hook on purpose: a thread already inside the original holds a
  // return address into the block hook_remove is about to free (gamehooks.cpp says the same), and a frame arriving in
  // between draws nothing, which at process teardown is nothing.
  s_roomsQ16Orig = NULL;
  s_drawOrig = NULL;
  if (s_roomsQ16Hook.target && !hook_remove(&s_roomsQ16Hook))
    ek_log("camera: polymer_drawrooms could not be unpatched");
  if (s_hook.target && !hook_remove(&s_hook))
    ek_log("camera: G_DrawRooms could not be unpatched");
}
