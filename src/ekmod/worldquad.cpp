// The world-quad layer's storage and its one GL draw; the rules are in worldquad.h (suite-reachable). Engine-facing,
// not run.

#include <windows.h>
#include <string.h>

#include "worldquad.h"
#include "gamestate.h"
#include "frustum.h"
#include "project.h"
#include "log.h"
#include "perf.h"
#include "config.h"

// One frame's list, not kept across frames: producers rebuild it from live geometry, so a carried quad would tint where
// a thing used to be.
static EkWorldQuad s_quads[EK_WORLD_QUAD_MAX];
static unsigned char s_kinds[EK_WORLD_QUAD_MAX];
static short s_sectors[EK_WORLD_QUAD_MAX];
static unsigned char s_drawn[EK_WORLD_QUAD_MAX];
static unsigned s_levelDrawn = 0, s_finalUndrawn = 0, s_finalTotal = 0;
static int s_count = 0;
static int s_refused = 0;
// The most any one collect pass was refused, and it outlives the reset: overlay.cpp prints it from the SwapBuffers hook
// after the flush emptied the list. A pass, not a frame: the producer resets at the head of its collect (worldquad.h).
static int s_refusedPeak = 0;
// Interlocked: the two transitions that set it run on the game thread and on DLL detach. A session total, not cleared
// by the reset: with no draw point there is no flush and no reset.
static volatile LONG s_haveDrawPoint = 0;
static int s_noDrawPoint = 0;

void worldquad_reset(void) {
  if (s_refused > s_refusedPeak) s_refusedPeak = s_refused;
  s_count = 0;
  s_refused = 0;
}

void worldquad_set_drawpoint(bool ok) {
  const LONG was = InterlockedExchange(&s_haveDrawPoint, ok ? 1 : 0);
  if (ok && was == 0) s_noDrawPoint = 0;
}

int worldquad_no_drawpoint(void) { return s_noDrawPoint; }

void worldquad_push_at(const EkWorldQuad& q, int kind, int sector) {
  if (InterlockedCompareExchange(&s_haveDrawPoint, 0, 0) == 0) {
    s_noDrawPoint++;
    return;
  }
  const int after = ekm_world_quad_append(s_quads, s_count, EK_WORLD_QUAD_MAX, q);
  if (after == s_count) {
    s_refused++;
  } else {
    s_kinds[s_count] = (unsigned char)kind;
    s_sectors[s_count] = (short)(sector >= 0 && sector < 0x8000 ? sector : -1);
    s_drawn[s_count] = 0;
  }
  s_count = after;
}

void worldquad_push_kind(const EkWorldQuad& q, int kind) { worldquad_push_at(q, kind, -1); }

void worldquad_push(const EkWorldQuad& q) { worldquad_push_at(q, EK_WQ_KIND_OTHER, -1); }

int worldquad_count(void) { return s_count; }

int worldquad_candidate_sectors(short* out, int max) {
  unsigned char seen[EK_MAXSECTORS / 8];
  memset(seen, 0, sizeof seen);
  return ekm_world_quad_candidates(s_sectors, s_drawn, s_count, seen,
                                   (int)sizeof seen, out, max);
}

unsigned worldquad_level_drawn(void) { return s_levelDrawn; }
unsigned worldquad_final_undrawn(void) { return s_finalUndrawn; }
unsigned worldquad_final_total(void) { return s_finalTotal; }

int worldquad_refused(void) { return s_refusedPeak; }

// GL 1.1 from opengl32.dll (as camera.cpp:356 cam_gl_entries): the context is compatibility and immediate mode works
// (spike: GL_VERSION "4.6.0 NVIDIA", GL_MODELVIEW_MATRIX carrying the world transform with the 1/1000 Build-to-GL
// scale). Polymer has unbound its shader by now (program 0), so that path saves none; Polymost leaves its own bound.
#define EK_GL_QUADS 0x0007u
#define EK_GL_LEQUAL 0x0203u
#define EK_GL_CURRENT_COLOR 0x0B00u
#define EK_GL_SRC_ALPHA 0x0302u
#define EK_GL_ONE_MINUS_SRC_ALPHA 0x0303u
#define EK_GL_CULL_FACE 0x0B44u
#define EK_GL_DEPTH_TEST 0x0B71u
#define EK_GL_DEPTH_WRITEMASK 0x0B72u
#define EK_GL_DEPTH_FUNC 0x0B74u
#define EK_GL_BLEND_DST 0x0BE0u
#define EK_GL_BLEND_SRC 0x0BE1u
#define EK_GL_BLEND 0x0BE2u
#define EK_GL_ALPHA_TEST 0x0BC0u
#define EK_GL_TEXTURE_2D 0x0DE1u
#define EK_GL_NO_ERROR 0x0000u
#define EK_GL_MODELVIEW_MATRIX 0x0BA6u
#define EK_GL_PROJECTION_MATRIX 0x0BA7u
#define EK_GL_VIEWPORT 0x0BA2u
#define EK_GL_CURRENT_PROGRAM 0x8B8Du
#define EK_GL_DEPTH_RANGE 0x0B70u
// No fog disable: fog off would make the volume the one surface that does not dim with distance. The polygon offset is
// needed: the secret volume's panels are exactly coplanar with the room's walls (same vertices, sector z). Values:
// worldquad.h.
#define EK_GL_POLYGON_OFFSET_FILL 0x8037u
#define EK_GL_POLYGON_OFFSET_FACTOR 0x8038u
#define EK_GL_POLYGON_OFFSET_UNITS 0x2A00u

typedef void(WINAPI* WqToggleFn)(unsigned int);
typedef unsigned char(WINAPI* WqIsEnabledFn)(unsigned int);
typedef void(WINAPI* WqBeginFn)(unsigned int);
typedef void(WINAPI* WqEndFn)(void);
typedef void(WINAPI* WqColor4ubFn)(unsigned char, unsigned char, unsigned char,
                                   unsigned char);
// Colour saved and restored in floats: GL_CURRENT_COLOR is a float vector, and glColor4ub would quantise it.
typedef void(WINAPI* WqColor4fFn)(float, float, float, float);
typedef void(WINAPI* WqGetFloatvFn)(unsigned int, float*);
typedef void(WINAPI* WqVertex3dFn)(double, double, double);
typedef void(WINAPI* WqBlendFuncFn)(unsigned int, unsigned int);
typedef void(WINAPI* WqDepthMaskFn)(unsigned char);
typedef void(WINAPI* WqDepthFuncFn)(unsigned int);
typedef void(WINAPI* WqPolygonOffsetFn)(float, float);
typedef void(WINAPI* WqGetIntegervFn)(unsigned int, int*);
typedef void(WINAPI* WqGetBooleanvFn)(unsigned int, unsigned char*);
typedef unsigned int(WINAPI* WqGetErrorFn)(void);

static WqToggleFn s_glEnable = NULL;
static WqToggleFn s_glDisable = NULL;
static WqIsEnabledFn s_glIsEnabled = NULL;
static WqBeginFn s_glBegin = NULL;
static WqEndFn s_glEnd = NULL;
static WqColor4ubFn s_glColor4ub = NULL;
static WqColor4fFn s_glColor4f = NULL;
static WqGetFloatvFn s_glGetFloatv = NULL;
static WqVertex3dFn s_glVertex3d = NULL;
static WqBlendFuncFn s_glBlendFunc = NULL;
static WqDepthMaskFn s_glDepthMask = NULL;
static WqDepthFuncFn s_glDepthFunc = NULL;
static WqPolygonOffsetFn s_glPolygonOffset = NULL;
static WqGetIntegervFn s_glGetIntegerv = NULL;
static WqGetBooleanvFn s_glGetBooleanv = NULL;
static WqGetErrorFn s_glGetError = NULL;

static bool s_glTried = false;
static bool s_saidNoGl = false;
static bool s_saidError = false;
static bool s_saidState = false;

// True with all of them in hand: without glIsEnabled/glGetIntegerv it could only assume the state it is about to
// change. Tried once.
static bool worldquad_gl_entries(void) {
  if (!s_glTried) {
    s_glTried = true;
    const HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) {
      s_glEnable = (WqToggleFn)GetProcAddress(gl, "glEnable");
      s_glDisable = (WqToggleFn)GetProcAddress(gl, "glDisable");
      s_glIsEnabled = (WqIsEnabledFn)GetProcAddress(gl, "glIsEnabled");
      s_glBegin = (WqBeginFn)GetProcAddress(gl, "glBegin");
      s_glEnd = (WqEndFn)GetProcAddress(gl, "glEnd");
      s_glColor4ub = (WqColor4ubFn)GetProcAddress(gl, "glColor4ub");
      s_glColor4f = (WqColor4fFn)GetProcAddress(gl, "glColor4f");
      s_glGetFloatv = (WqGetFloatvFn)GetProcAddress(gl, "glGetFloatv");
      s_glVertex3d = (WqVertex3dFn)GetProcAddress(gl, "glVertex3d");
      s_glBlendFunc = (WqBlendFuncFn)GetProcAddress(gl, "glBlendFunc");
      s_glDepthMask = (WqDepthMaskFn)GetProcAddress(gl, "glDepthMask");
      s_glDepthFunc = (WqDepthFuncFn)GetProcAddress(gl, "glDepthFunc");
      s_glPolygonOffset =
          (WqPolygonOffsetFn)GetProcAddress(gl, "glPolygonOffset");
      s_glGetIntegerv = (WqGetIntegervFn)GetProcAddress(gl, "glGetIntegerv");
      s_glGetBooleanv = (WqGetBooleanvFn)GetProcAddress(gl, "glGetBooleanv");
      s_glGetError = (WqGetErrorFn)GetProcAddress(gl, "glGetError");
    }
  }
  return s_glEnable && s_glDisable && s_glIsEnabled && s_glBegin && s_glEnd &&
         s_glColor4ub && s_glColor4f && s_glGetFloatv && s_glVertex3d &&
         s_glBlendFunc && s_glDepthMask && s_glDepthFunc && s_glPolygonOffset &&
         s_glGetIntegerv && s_glGetBooleanv && s_glGetError;
}

// Under Polymost the draw point inherits the engine's shader (program 4 in the AA log), which would run our vertices
// through polymost1Vert/Frag. So the flush unbinds it, submits vertices in Polymost's space (ekm_pm_world_vertex), then
// restores the same id so glbuild's cached program (glbuild.cpp:134) stays true.
#define EK_GL_ACTIVE_TEXTURE 0x84E0u
#define EK_GL_TEXTURE0 0x84C0u

typedef void(WINAPI* WqUseProgramFn)(unsigned int);
typedef void(WINAPI* WqActiveTextureFn)(unsigned int);
typedef PROC(WINAPI* WqWglGetProcFn)(LPCSTR);

static WqUseProgramFn s_glUseProgram = NULL;
static WqActiveTextureFn s_glActiveTexture = NULL;
static bool s_pmTried = false;
static bool s_pmRefused = false;
static bool s_saidPmNoProj = false;
static bool s_saidPmNear = false;
static bool s_saidPm = false;
static int s_pmTexUnits[3] = {-1, -1, -1};

static PROC wq_wgl_valid(PROC p) {
  const INT_PTR v = (INT_PTR)p;
  return (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) ? NULL : p;
}

// GL 1.3/2.0 entry points opengl32.dll does not export: wglGetProcAddress once, on the render thread.
static bool worldquad_pm_entries(void) {
  if (!s_pmTried) {
    s_pmTried = true;
    const HMODULE gl = GetModuleHandleA("opengl32.dll");
    const WqWglGetProcFn get =
        gl ? (WqWglGetProcFn)GetProcAddress(gl, "wglGetProcAddress") : NULL;
    if (get) {
      s_glUseProgram = (WqUseProgramFn)wq_wgl_valid(get("glUseProgram"));
      s_glActiveTexture = (WqActiveTextureFn)wq_wgl_valid(get("glActiveTexture"));
    }
  }
  return s_glUseProgram != NULL && s_glActiveTexture != NULL;
}

static bool worldquad_pm_begin(const float* pr, EkProjection* proj, double* k,
                               int* savedProg, int* savedUnit) {
  if (s_pmRefused) return false;
  if (!worldquad_pm_entries()) {
    s_pmRefused = true;
    ek_log("worldquad: glUseProgram/glActiveTexture did not resolve through "
           "wglGetProcAddress, so the quad layer stands down under Polymost for "
           "the session");
    return false;
  }
  const double nearclip = ekm_pm_near_from_m14((double)pr[14]);
  *k = ekm_pm_k_from_near(nearclip);
  if (!(*k > 0.0)) {
    if (!s_saidPmNear) {
      s_saidPmNear = true;
      ek_log("worldquad: the Polymost projection's m14 %.9g yields no near "
             "plane, so these frames draw no quads", (double)pr[14]);
    }
    return false;
  }
  if (!project_begin(proj, EK_PC_WORLDQUAD) || !proj->g.valid) {
    if (!s_saidPmNoProj) {
      s_saidPmNoProj = true;
      ek_log("worldquad: no projection at the draw point (&ud or the viewport "
             "would not read), so these frames draw no quads");
    }
    return false;
  }

  *savedProg = 0;
  *savedUnit = (int)EK_GL_TEXTURE0;
  s_glGetIntegerv(EK_GL_CURRENT_PROGRAM, savedProg);
  s_glGetIntegerv(EK_GL_ACTIVE_TEXTURE, savedUnit);
  if (!s_saidPm) {
    for (int u = 0; u < 3; u++) {
      s_glActiveTexture(EK_GL_TEXTURE0 + 1u + (unsigned)u);
      s_pmTexUnits[u] = s_glIsEnabled(EK_GL_TEXTURE_2D) ? 1 : 0;
    }
  }
  // Unit 0 before the shared save, so the GL_TEXTURE_2D read lands on the unit fixed-function samples first.
  s_glActiveTexture(EK_GL_TEXTURE0);
  s_glUseProgram(0);
  const unsigned int err = s_glGetError();
  if (err != EK_GL_NO_ERROR) {
    s_glUseProgram((unsigned int)*savedProg);
    s_glActiveTexture((unsigned int)*savedUnit);
    s_pmRefused = true;
    ek_log("worldquad: glGetError reads 0x%04X after unbinding program %d on "
           "texture unit 0, so the quad layer stands down under Polymost for "
           "the session", err, *savedProg);
    return false;
  }
  return true;
}

// An empty list costs nothing: no lookup, state read or GL call (one perf zone pair excepted). Read what it depends on,
// set what it needs, restore all: inherited state bit this port three times (GL_CULL_FACE off, camera.cpp:317;
// GL_DEPTH_TEST; a depth func). `level` draws undrawn quads of pending sectors; the final flush draws the rest.
static int wq_flush(const unsigned char* pending, int bytes, bool level) {
  // The zone is the whole function, closing on both returns: else empty-list frames would read as a win.
  perf_zone_begin(EK_PZ_OVERLAY_QUADS);
  if (s_count <= 0) { perf_zone_end(EK_PZ_OVERLAY_QUADS, 0); return 0; }

  const int noDepth = g_cfg.secvolnodepth != 0 ? 1 : 0;
  // Counted before any GL work: a level with nothing of ours costs a loop, not a state save and restore.
  int selected = 0;
  for (int i = 0; i < s_count; i++)
    selected += (level ? ekm_world_quad_at_level(s_sectors[i], s_drawn[i], s_kinds[i],
                                                 noDepth, pending, bytes)
                       : ekm_world_quad_at_final(s_drawn[i])) ? 1 : 0;
  if (level && selected == 0) { perf_zone_end(EK_PZ_OVERLAY_QUADS, 0); return 0; }
  if (!level) {
    s_finalTotal += (unsigned)s_count;
    s_finalUndrawn += (unsigned)selected;
    if (selected == 0) {
      worldquad_reset();
      perf_zone_end(EK_PZ_OVERLAY_QUADS, 0);
      return 0;
    }
  }

  if (!worldquad_gl_entries()) {
    if (!s_saidNoGl) {
      s_saidNoGl = true;
      ek_log("worldquad: opengl32.dll did not yield every GL 1.1 entry point "
             "this draw needs, so the quad layer stands down for the session");
    }
    if (!level) worldquad_reset();
    perf_zone_end(EK_PZ_OVERLAY_QUADS, 0);
    return 0;
  }

  // Drain first so the reading at the end is ours: glGetError clears ONE flag per call and the engine may have left
  // several. Bounded: GL 1.1 has five error enums, so a stuck driver gives up.
  for (int drain = 0; drain < 16; drain++)
    if (s_glGetError() == EK_GL_NO_ERROR) break;

  // One-shot state dump on the first flush with quads: a bound shader swallows immediate-mode vertices silently, and a
  // modelview that is not the world transform puts the quad off screen; both look like this bug.
  if (!s_saidState) {
    s_saidState = true;
    float mv[16] = {0.0f}, pr[16] = {0.0f};
    int vp[4] = {0, 0, 0, 0}, prog = -1;
    s_glGetFloatv(EK_GL_MODELVIEW_MATRIX, mv);
    s_glGetFloatv(EK_GL_PROJECTION_MATRIX, pr);
    s_glGetIntegerv(EK_GL_VIEWPORT, vp);
    s_glGetIntegerv(EK_GL_CURRENT_PROGRAM, &prog);
    int dfunc = 0;
    unsigned char dmask = 1;
    s_glGetIntegerv(EK_GL_DEPTH_FUNC, &dfunc);
    s_glGetBooleanv(EK_GL_DEPTH_WRITEMASK, &dmask);
    ek_log("worldquad: state at the draw point -- program %d, viewport "
           "%d,%d %dx%d, depth test %d func 0x%04X mask %d, blend %d, "
           "texture2d %d, alphatest %d, cull %d",
           prog, vp[0], vp[1], vp[2], vp[3],
           s_glIsEnabled(EK_GL_DEPTH_TEST) ? 1 : 0,
           (unsigned)dfunc, (int)dmask,
           s_glIsEnabled(EK_GL_BLEND) ? 1 : 0,
           s_glIsEnabled(EK_GL_TEXTURE_2D) ? 1 : 0,
           s_glIsEnabled(EK_GL_ALPHA_TEST) ? 1 : 0,
           s_glIsEnabled(EK_GL_CULL_FACE) ? 1 : 0);
    ek_log("worldquad: modelview diag %.6f %.6f %.6f %.6f, row3 %.2f %.2f %.2f",
           mv[0], mv[5], mv[10], mv[15], mv[12], mv[13], mv[14]);
    float dr[2] = {0.0f, 1.0f};
    s_glGetFloatv(EK_GL_DEPTH_RANGE, dr);
    ek_log("worldquad: projection diag %.6f %.6f %.6f %.6f, depth range %.6f..%.6f",
           pr[0], pr[5], pr[10], pr[15], dr[0], dr[1]);
    const EkWorldQuad& q0 = s_quads[0];
    ek_log("worldquad: %d quad(s); first argb 0x%08X corners "
           "(%.1f,%.1f,%d) (%.1f,%.1f,%d) (%.1f,%.1f,%d) (%.1f,%.1f,%d)",
           s_count, q0.argb,
           q0.x[0], q0.y[0], q0.z[0], q0.x[1], q0.y[1], q0.z[1],
           q0.x[2], q0.y[2], q0.z[2], q0.x[3], q0.y[3], q0.z[3]);
  }

  // Decided per flush from the projection these vertices go through; anything but resizeglcheck's shape keeps Polymer.
  float pmProj[16] = {0.0f};
  s_glGetFloatv(EK_GL_PROJECTION_MATRIX, pmProj);
  const bool polymost = ekm_pm_projection_is_polymost(pmProj);
  EkProjection pmCam = {};
  double pmK = 0.0;
  int pmProg = 0, pmUnit = (int)EK_GL_TEXTURE0;
  if (polymost && !worldquad_pm_begin(pmProj, &pmCam, &pmK, &pmProg, &pmUnit)) {
    if (!level) worldquad_reset();
    perf_zone_end(EK_PZ_OVERLAY_QUADS, 0);
    return 0;
  }

  const int wasDepth = s_glIsEnabled(EK_GL_DEPTH_TEST) ? 1 : 0;
  const int wasBlend = s_glIsEnabled(EK_GL_BLEND) ? 1 : 0;
  const int wasTex = s_glIsEnabled(EK_GL_TEXTURE_2D) ? 1 : 0;
  const int wasAlpha = s_glIsEnabled(EK_GL_ALPHA_TEST) ? 1 : 0;
  const int wasCull = s_glIsEnabled(EK_GL_CULL_FACE) ? 1 : 0;
  // Saves the enable AND the values: glPolygonOffset is global, and the engine biases per sprite (polymer.cpp:1583).
  const int wasPolyOff = s_glIsEnabled(EK_GL_POLYGON_OFFSET_FILL) ? 1 : 0;
  float wasOffset[2] = {0.0f, 0.0f};
  s_glGetFloatv(EK_GL_POLYGON_OFFSET_FACTOR, &wasOffset[0]);
  s_glGetFloatv(EK_GL_POLYGON_OFFSET_UNITS, &wasOffset[1]);
  unsigned char wasDepthMask = 1;
  s_glGetBooleanv(EK_GL_DEPTH_WRITEMASK, &wasDepthMask);
  int wasDepthFunc = (int)EK_GL_LEQUAL;
  int wasSrc = (int)EK_GL_SRC_ALPHA, wasDst = (int)EK_GL_ONE_MINUS_SRC_ALPHA;
  s_glGetIntegerv(EK_GL_DEPTH_FUNC, &wasDepthFunc);
  s_glGetIntegerv(EK_GL_BLEND_SRC, &wasSrc);
  s_glGetIntegerv(EK_GL_BLEND_DST, &wasDst);
  float wasColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  s_glGetFloatv(EK_GL_CURRENT_COLOR, wasColor);

  if (!wasDepth) s_glEnable(EK_GL_DEPTH_TEST);
  s_glDepthFunc(EK_GL_LEQUAL);
  if (wasDepthMask) s_glDepthMask(0);
  if (!wasBlend) s_glEnable(EK_GL_BLEND);
  s_glBlendFunc(EK_GL_SRC_ALPHA, EK_GL_ONE_MINUS_SRC_ALPHA);
  if (wasTex) s_glDisable(EK_GL_TEXTURE_2D);
  if (wasAlpha) s_glDisable(EK_GL_ALPHA_TEST);
  // Load-bearing: a box is seen from outside on one face and inside on the opposite, so culling halves the volume.
  if (wasCull) s_glDisable(EK_GL_CULL_FACE);
  // Toward the viewer for the whole flush: the secret panels tie with the room's walls on depth and z-fight. One value
  // serves both producers: only the panels are coplanar, the box rides along. Derivation: worldquad.h.
  if (!wasPolyOff) s_glEnable(EK_GL_POLYGON_OFFSET_FILL);
  s_glPolygonOffset(EK_WORLD_OFFSET_FACTOR, EK_WORLD_OFFSET_UNITS);

  // Two batches, not a per-quad toggle: batch 1 runs only with the diagnostic on and draws secret volumes with depth
  // test off. A level flush never has a batch-1 quad (ekm_world_quad_at_level refuses them).
  int marked = 0;
  for (int batch = 0; batch < 2; batch++) {
    if (batch == 1) {
      if (!noDepth || level) break;
      s_glDisable(EK_GL_DEPTH_TEST);
    }
    for (int i = 0; i < s_count; i++) {
      if (!ekm_world_quad_in_batch(s_kinds[i], noDepth, batch)) continue;
      if (level) {
        if (!ekm_world_quad_at_level(s_sectors[i], s_drawn[i], s_kinds[i], noDepth,
                                     pending, bytes))
          continue;
        s_drawn[i] = 1;
        marked++;
      } else if (!ekm_world_quad_at_final(s_drawn[i])) {
        continue;
      }
      const EkWorldQuad& q = s_quads[i];
      const unsigned c = q.argb;
      s_glColor4ub((unsigned char)((c >> 16) & 0xFFu),
                   (unsigned char)((c >> 8) & 0xFFu),
                   (unsigned char)(c & 0xFFu),
                   (unsigned char)((c >> 24) & 0xFFu));
      s_glBegin(EK_GL_QUADS);
      // Build units in, polymer's axes out: polymer_drawrooms fills pos[] as (daposy, -daposz/16, -daposx)
      // (polymer.cpp:1118), so all three axes permute and the vertical carries the 16:1 z unit; else the tint drew off
      // screen with no error.
      for (int v = 0; v < 4; v++) {
        if (polymost) {
          // Camera-relative deltas, dz not divided by 16: V carries that.
          const EkPmVertex p = ekm_pm_world_vertex(
              pmCam.g, q.x[v] - (double)pmCam.cam.x, q.y[v] - (double)pmCam.cam.y,
              (double)q.z[v] - (double)pmCam.cam.z, pmK);
          s_glVertex3d(p.x, p.y, p.z);
          continue;
        }
        const EkGlPointD g = ekm_build_to_gld(q.x[v], q.y[v], (double)q.z[v]);
        s_glVertex3d(g.x, g.y, g.z);
      }
      s_glEnd();
    }
    if (batch == 1) s_glEnable(EK_GL_DEPTH_TEST);
  }

  s_glColor4f(wasColor[0], wasColor[1], wasColor[2], wasColor[3]);
  if (wasCull) s_glEnable(EK_GL_CULL_FACE);
  if (!wasPolyOff) s_glDisable(EK_GL_POLYGON_OFFSET_FILL);
  s_glPolygonOffset(wasOffset[0], wasOffset[1]);
  if (wasAlpha) s_glEnable(EK_GL_ALPHA_TEST);
  if (wasTex) s_glEnable(EK_GL_TEXTURE_2D);
  s_glBlendFunc((unsigned int)wasSrc, (unsigned int)wasDst);
  if (!wasBlend) s_glDisable(EK_GL_BLEND);
  if (wasDepthMask) s_glDepthMask(1);
  s_glDepthFunc((unsigned int)wasDepthFunc);
  if (!wasDepth) s_glDisable(EK_GL_DEPTH_TEST);
  if (polymost) {
    s_glActiveTexture((unsigned int)pmUnit);
    s_glUseProgram((unsigned int)pmProg);
    if (!s_saidPm) {
      s_saidPm = true;
      int back = -1;
      s_glGetIntegerv(EK_GL_CURRENT_PROGRAM, &back);
      ek_log("worldquad: Polymost path -- program %d saved, %d read back after "
             "the restore; active texture 0x%04X; near %.9g from m14 %.9g, K "
             "%.9g; texture2d on units 1-3: %d %d %d",
             pmProg, back, (unsigned)pmUnit, pmK * 8.0, (double)pmProj[14], pmK,
             s_pmTexUnits[0], s_pmTexUnits[1], s_pmTexUnits[2]);
    }
  }

  const unsigned int err = s_glGetError();
  if (err != EK_GL_NO_ERROR && !s_saidError) {
    s_saidError = true;
    ek_log("worldquad: glGetError reads 0x%04X after a flush of %d quad(s) "
           "(%d refused by the cap of %d)",
           err, s_count, s_refused, EK_WORLD_QUAD_MAX);
  }

  if (level) {
    s_levelDrawn += (unsigned)marked;
    perf_zone_end(EK_PZ_OVERLAY_QUADS, marked);
    return marked;
  }
  worldquad_reset();
  perf_zone_end(EK_PZ_OVERLAY_QUADS, selected);
  return selected;
}

void worldquad_flush(void) { wq_flush(NULL, 0, false); }

int worldquad_flush_level(const unsigned char* pending, int bytes) {
  return wq_flush(pending, bytes, true);
}
