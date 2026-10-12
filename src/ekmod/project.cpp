// World-to-screen for Polymer, from polymer.cpp. horiz is a slope: pitch = atan2(horiz - 100, 128) (polymer.cpp:1121).
// viewingrange is DERIVED, not read: G_DrawRooms restores it (game.cpp:1250-1254), so on stock it reads 65536 at
// SwapBuffers and under AA it alternates (zoom_trace). Expression: game.cpp:886-895.

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "config.h"
#include "gamestate.h"
#include "gamevars.h"
#include "aazoom.h"
#include "projmath.h"
#include "project.h"
#include "cvars.h"
#include "ekm_seen2.h"
#include "camera.h"
#include "sectorstate.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// GL_VIEWPORT is asked of OpenGL, not rebuilt from windowxy1/xdimen/ydimen, which depend on status bar size.
typedef void(WINAPI* GetIntegervFn)(unsigned int, int*);
static GetIntegervFn s_glGetIntegerv = NULL;

static void load_gl(void) {
  if (s_glGetIntegerv) return;
  const HMODULE gl = GetModuleHandleA("opengl32.dll");
  if (gl) s_glGetIntegerv = (GetIntegervFn)GetProcAddress(gl, "glGetIntegerv");
}

// The player's sprite carries the shrink/grow state that scales the field of view; found by picnum because g_player is
// `playerdata_t *const` (global.h:152) and may be folded away.
static const int EK_APLAYER  = 1405;
static const int EK_STAT_PLAYER = 10;

static int player_yrepeat(void) {
  if (!gamestate_ready()) return 36;
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    if (*(const short*)(r + EK_SPR_PICNUM) != EK_APLAYER) continue;
    if (*(const short*)(r + EK_SPR_STATNUM) != EK_STAT_PLAYER) continue;
    const unsigned char yr = r[EK_SPR_YREPEAT];
    return yr ? yr : 36;
  }
  return 36;
}

// For the compare line only (at most once a second); -1 if none.
static int player_sectnum(void) {
  if (!gamestate_ready()) return -1;
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    if (*(const short*)(r + EK_SPR_PICNUM) != EK_APLAYER) continue;
    if (*(const short*)(r + EK_SPR_STATNUM) != EK_STAT_PLAYER) continue;
    return *(const short*)(r + EK_SPR_SECTNUM);
  }
  return -1;
}

// AA's scope zoom, measured here and corrected in project_begin. AA never touches fov: it writes the range with
// setaspect vr yxaspect (AAGAME.CON:16996), and the engine's global alternates within a frame, so the correction reads
// AA's vr. project_begin runs up to six times a frame, so the change key is the values, not a frame counter.
struct EkZoomState {
  int got, zoomin, zoomlevel, vr, vrhi, vrlo, fov, yrep;
  // The engine's own two, read through the registry's pointer arm; in the change key because an engine range that moves
  // while every var stands still is the answer.
  int engVr, engYx;
  // In the key because the correction divides by a term built from the window pair: a resize moves the range with every
  // var still.
  int winW, winH;

  // Field-by-field, the set ekm_seen2_admit's T requires.
  bool operator==(const EkZoomState& o) const {
    return got == o.got && zoomin == o.zoomin && zoomlevel == o.zoomlevel &&
           vr == o.vr && vrhi == o.vrhi && vrlo == o.vrlo && fov == o.fov &&
           yrep == o.yrep && engVr == o.engVr && engYx == o.engYx &&
           winW == o.winW && winH == o.winH;
  }
};

// Player 0 is the one whose scope is on screen (playerstate.cpp anchors on g_player[0].ps).
static const int EK_PROJ_LOCAL_PLAYER = 0;

// A failed read must not print like a real value (gamevars.cpp's value_text rule). _TRUNCATE, never sprintf_s.
static void zoom_slot(bool got, int v, char* outText, size_t bytes) {
  if (got) _snprintf_s(outText, bytes, _TRUNCATE, "%d", v);
  else     _snprintf_s(outText, bytes, _TRUNCATE, "unread");
}

// On change, never per frame. The key is the five values and read flags plus `fov` and the player's yrepeat, the whole
// input to ekm_viewingrange. `applied` and `appliedRange` are not in it: they are pure functions of terms already
// keyed. zoomin and vr arrive already read, so the printed number is the one the fix used.
static void zoom_trace(int fovDegrees, int yrep, float derivedRange,
                       bool gotZoomin, int zoomin, bool gotVr, int vr,
                       bool applied, float appliedRange, int winW, int winH) {
  if (!g_cfg.debuglog) return;

  // Each read is a statement of its own: reading an out-param in the argument list that fills it is indeterminately
  // sequenced and MSVC gets it wrong.
  int zoomlevel = 0, vrhi = 0, vrlo = 0;
  const bool gotLevel = gamevars_zoomlevel(EK_PROJ_LOCAL_PLAYER, &zoomlevel);
  const bool gotVrhi = gamevars_vrhi(&vrhi);
  const bool gotVrlo = gamevars_vrlo(&vrlo);

  // The engine's pair alternates within one frame between AA's value and 65536 (calls straddle something that resets
  // the aspect), so it is not a source; the fix takes AA's `vr`. Kept as instrumentation.
  int engVr = 0, engYx = 0;
  const bool gotEngVr = gamevars_viewingrange(&engVr);
  const bool gotEngYx = gamevars_yxaspect(&engYx);

  const int got = (gotZoomin ? 1 : 0) | (gotLevel ? 2 : 0) | (gotVr ? 4 : 0) |
                  (gotVrhi ? 8 : 0) | (gotVrlo ? 16 : 0) |
                  (gotEngVr ? 32 : 0) | (gotEngYx ? 64 : 0);

  // Two slots hold the last two DISTINCT states logged: a single record passed on every call once the engine's pair
  // alternated A,B,A,B across a frame's calls.
  static EkSeen2<EkZoomState> seen2 = { false, false };
  const EkZoomState state = { got, zoomin, zoomlevel, vr, vrhi, vrlo,
                              fovDegrees, yrep, engVr, engYx, winW, winH };
  if (!ekm_seen2_admit(seen2, state)) return;

  char zinText[24], zlvText[24], vrText[24], vrhiText[24], vrloText[24];
  zoom_slot(gotZoomin, zoomin, zinText, sizeof(zinText));
  zoom_slot(gotLevel, zoomlevel, zlvText, sizeof(zlvText));
  zoom_slot(gotVr, vr, vrText, sizeof(vrText));
  zoom_slot(gotVrhi, vrhi, vrhiText, sizeof(vrhiText));
  zoom_slot(gotVrlo, vrlo, vrloText, sizeof(vrloText));

  // Two unread terms are not a "no": print the unknown, not 0.
  char activeText[24];
  if (gotZoomin && gotVr)
    _snprintf_s(activeText, sizeof(activeText), _TRUNCATE, "%d",
                ekm_aa_zoom_active(zoomin, vr) ? 1 : 0);
  else
    _snprintf_s(activeText, sizeof(activeText), _TRUNCATE, "unread");

  float ratio = 0.0f;
  bool gotRatio = false;
  if (gotVr && gotVrhi) gotRatio = ekm_aa_zoom_ratio(vr, vrhi, &ratio);
  char ratioText[24];
  if (gotRatio) _snprintf_s(ratioText, sizeof(ratioText), _TRUNCATE, "%.4f", ratio);
  else          _snprintf_s(ratioText, sizeof(ratioText), _TRUNCATE, "none");

  char engVrText[24], engYxText[24];
  zoom_slot(gotEngVr, engVr, engVrText, sizeof(engVrText));
  zoom_slot(gotEngYx, engYx, engYxText, sizeof(engYxText));

  // mulscale16(a,b) is a*b/65536: each factor is its argument over 65536, one from the engine's frame, one from AA.
  float engFactor = 0.0f, vrFactor = 0.0f;
  bool gotEngFactor = false, gotVrFactor = false;
  if (gotEngVr) gotEngFactor = ekm_aa_range_factor(engVr, &engFactor);
  if (gotVr) gotVrFactor = ekm_aa_range_factor(vr, &vrFactor);
  char engFactorText[24], vrFactorText[24];
  if (gotEngFactor)
    _snprintf_s(engFactorText, sizeof(engFactorText), _TRUNCATE, "%.4f", engFactor);
  else
    _snprintf_s(engFactorText, sizeof(engFactorText), _TRUNCATE, "none");
  if (gotVrFactor)
    _snprintf_s(vrFactorText, sizeof(vrFactorText), _TRUNCATE, "%.4f", vrFactor);
  else
    _snprintf_s(vrFactorText, sizeof(vrFactorText), _TRUNCATE, "none");

  // What the fix did: an applied correction versus a gate refusal are the two outcomes to grep for. The range printed
  // is the one the projection used.
  char fixText[48];
  if (applied)
    _snprintf_s(fixText, sizeof(fixText), _TRUNCATE, "APPLIED -> %.0f",
                appliedRange);
  else
    _snprintf_s(fixText, sizeof(fixText), _TRUNCATE, "not applied");

  // Predicted drawn range against AA's `vr`: ekm_build_proj multiplies our range by the default-global term and
  // setaspect ASSIGNS that global, so the product must come back to `vr` exactly. A broken divisor shows up here, not
  // on screen.
  const double term = ekm_default_viewingrange_term(winW, winH);
  char drawnText[32], aaAssignedText[32];
  if (applied && term > 0.0)
    _snprintf_s(drawnText, sizeof(drawnText), _TRUNCATE, "%.0f",
                (double)appliedRange * term);
  else
    _snprintf_s(drawnText, sizeof(drawnText), _TRUNCATE, "none");
  if (applied && gotVr)
    _snprintf_s(aaAssignedText, sizeof(aaAssignedText), _TRUNCATE, "%d", vr);
  else
    _snprintf_s(aaAssignedText, sizeof(aaAssignedText), _TRUNCATE, "none");

  // IDLE is its own answer: with the scope down an engine 65536 proves nothing.
  const char* verdict;
  if (!gotEngVr || !gotVr)
    verdict = "UNREAD -- one of the two numbers did not read";
  else if (!gotZoomin || !ekm_aa_zoom_active(zoomin, vr))
    verdict = "IDLE -- no zoom in force, so this frame proves nothing";
  // AMBIGUOUS comes first, or TRACKS would be true for the wrong reason: the first zoomed tic still has vr at 65536
  // (aazoom.h:8-11).
  else if (vr == EK_AA_VR_SCALE)
    verdict = "AMBIGUOUS -- vr is still at 65536, so tracking and parked look "
              "identical on this tic; the next one decides";
  else if (engVr == vr)
    verdict = "TRACKS -- the engine holds AA's vr at our read, which is the "
              "value it draws with; the fix REPLACES our range with it, and "
              "does not multiply by it";
  else if (engVr == EK_AA_VR_SCALE)
    verdict = "PARKED at 65536 -- AA's write does not survive to our read, so "
              "the gamevar is the only source";
  else
    verdict = "DISAGREES -- the engine holds neither vr nor 65536";

  // Must fit ek_log's 1024-byte buffer; a longer line was truncated with an embedded NUL (ek_log_keep_len). Keep the
  // format string short.
  ek_log("zoom: zoomin=%s zoomlevel=%s vr=%s vrhi=%s vrlo=%s -- active %s, "
         "vr/vrhi %s; engine viewingrange=%s yxaspect=%s -- engine/65536 %s, "
         "vr/65536 %s, verdict %s; ours fov %d yrepeat %d derived viewingrange "
         "%.0f, correction %s; window %dx%d, predicted drawn %s vs AA's "
         "assigned vr %s -- THESE TWO MUST BE EQUAL (setaspect assigns the "
         "drawn range, it does not scale it); %d of %d zoom vars and %d of %d "
         "engine vars resolved. yxaspect standing still proves nothing: "
         "game.cpp:1253 recomputes it rather than restoring it",
         zinText, zlvText, vrText, vrhiText, vrloText, activeText, ratioText,
         engVrText, engYxText, engFactorText, vrFactorText, verdict,
         fovDegrees, yrep, derivedRange, fixText, winW, winH, drawnText,
         aaAssignedText, gamevars_zoom_resolved(),
         EK_GV_ZOOM_COUNT, gamevars_engine_resolved(), EK_GV_ENGINE_COUNT);
}

// On EVERY project_begin call, named by caller (round-1's first-call compare missed the drift; values alternate between
// calls). Cumulative counters, a total beside the parts every 5 s, a DIFFERS line at most once a second per set of
// fields. Render thread only; changes nothing drawn.
static unsigned s_vcCompared = 0, s_vcDiffer = 0, s_vcNoRecord = 0;
static unsigned s_vcUdMoved = 0;
static unsigned s_vcField[EKM_VD_FIELD_COUNT] = { 0 };
// Index EK_PC_COUNT collects an out-of-range tag. Columns: compared, differing, no record, ud moved after draw.
static unsigned s_vcByCaller[EK_PC_COUNT + 1][4] = { { 0 } };

// Which pose each call used. A frame counts as "record" if any call in it did, banked when the next frame's first call
// arrives.
static unsigned s_psCallsRec = 0, s_psCallsUd = 0;
static unsigned s_psFramesRec = 0, s_psFramesUd = 0;
static unsigned s_psFrame = 0;
static int s_psFrameOpen = 0, s_psFrameRec = 0;
static bool s_psSaidRec = false, s_psSaidUd = false;

static void pose_source_note(bool fromRecord, unsigned frame) {
  if (s_psFrameOpen && frame != s_psFrame) {
    if (s_psFrameRec) s_psFramesRec++;
    else s_psFramesUd++;
    s_psFrameOpen = 0;
  }
  if (!s_psFrameOpen) {
    s_psFrameOpen = 1;
    s_psFrame = frame;
    s_psFrameRec = 0;
  }
  if (fromRecord) {
    s_psCallsRec++;
    s_psFrameRec = 1;
  } else {
    s_psCallsUd++;
  }
  if (fromRecord && !s_psSaidRec) {
    s_psSaidRec = true;
    ek_log("project: D37 pose source -- the engine's own player-view draw "
           "(frame %u); ud.camera* now supplies only fov, and the pose on a "
           "frame without this frame's complete record", frame);
  } else if (!fromRecord && !s_psSaidUd) {
    s_psSaidUd = true;
    ek_log("project: D37 pose source -- ud.camera* fallback in use (frame %u): "
           "no complete player-view record from this frame", frame);
  }
}
static unsigned s_vcTotalsMs = 0;
static unsigned s_vcTotalsKey = 0xFFFFFFFFu;
static unsigned s_vcFrame = 0, s_vcFrameCall = 0;

// A mask seen within the last second is held back; a new mask takes the stalest slot. 0 marks empty.
struct VcLineSlot { unsigned mask, ms; };
static VcLineSlot s_vcLines[16] = { { 0, 0 } };

static bool vc_line_due(unsigned mask, unsigned now) {
  int oldest = 0;
  for (int i = 0; i < 16; i++) {
    if (s_vcLines[i].mask == mask) {
      if ((unsigned)(now - s_vcLines[i].ms) < 1000u) return false;
      s_vcLines[i].ms = now;
      return true;
    }
    if ((unsigned)(now - s_vcLines[i].ms) >
        (unsigned)(now - s_vcLines[oldest].ms))
      oldest = i;
  }
  s_vcLines[oldest].mask = mask;
  s_vcLines[oldest].ms = now;
  return true;
}

// The ESP side is the record whenever it was usable, so its pose reads 0 differing by construction. `udCam` is
// ud.camera* now: record against it is "ud moved after draw", the AA HUD effect.
static void engine_compare(const EkProjection* out, int xdim, int ydim,
                           int caller, const EkCamera& udCam) {
  if (!g_cfg.debuglog) return;
  const unsigned frame = camera_frame_number();
  if (frame == 0) return;
  if (frame != s_vcFrame) {
    s_vcFrame = frame;
    s_vcFrameCall = 0;
  }
  s_vcFrameCall++;
  const int ci = (caller >= 0 && caller < EK_PC_COUNT) ? caller : EK_PC_COUNT;

  EkEngineView e;
  const bool have = camera_engine_view(&e);
  if (!have || e.frame != frame) {
    s_vcNoRecord++;
    s_vcByCaller[ci][2]++;
  } else {
    s_vcCompared++;
    s_vcByCaller[ci][0]++;
    const double term = ekm_default_viewingrange_term(xdim, ydim);
    const EkViewPose eng = { e.x, e.y, e.z, e.angFix, e.horizFix, e.sect,
                             (double)e.vr, e.yx, e.gotVr, e.gotYx,
                             e.vpX, e.vpY, e.vpW, e.vpH, e.xdim, e.ydim,
                             e.gotVp, e.gotDim };
    const EkViewPose esp = { out->cam.x, out->cam.y, out->cam.z,
                             out->cam.angFix, out->cam.horizFix, out->cam.sect,
                             (double)out->viewingRange * term,
                             (int)EKM_POLYMOST_YXASPECT, term > 0.0 ? 1 : 0, 1,
                             out->vp.x, out->vp.y, out->vp.w, out->vp.h,
                             xdim, ydim, 1, 1 };
    const unsigned d = ekm_view_diff(eng, esp);
    if (d) {
      s_vcDiffer++;
      s_vcByCaller[ci][1]++;
      for (int i = 0; i < EKM_VD_FIELD_COUNT; i++)
        if (d & (1u << i)) s_vcField[i]++;
      if (vc_line_due(d, GetTickCount())) {
        char which[128];
        which[0] = 0;
        for (int i = 0; i < EKM_VD_FIELD_COUNT; i++) {
          if (!(d & (1u << i))) continue;
          strncat_s(which, sizeof(which), which[0] ? "," : "", _TRUNCATE);
          strncat_s(which, sizeof(which), ekm_view_diff_name(i), _TRUNCATE);
        }
        char engVrText[24], engYxText[24], engXdText[24], engYdText[24];
        zoom_slot(e.gotVr != 0, e.vr, engVrText, sizeof(engVrText));
        zoom_slot(e.gotYx != 0, e.yx, engYxText, sizeof(engYxText));
        zoom_slot(e.gotDim != 0, e.xdim, engXdText, sizeof(engXdText));
        zoom_slot(e.gotDim != 0, e.ydim, engYdText, sizeof(engYdText));
        char engVpText[48];
        if (e.gotVp)
          _snprintf_s(engVpText, sizeof(engVpText), _TRUNCATE, "%d,%d %dx%d",
                      e.vpX, e.vpY, e.vpW, e.vpH);
        else
          _snprintf_s(engVpText, sizeof(engVpText), _TRUNCATE, "unread");
        const int psect = player_sectnum();
        EkSectorInfo si;
        const bool gotSi = psect >= 0 && sectorstate_info(psect, &si);
        ek_log("project vs engine: %s (call %u this frame) frame %u DIFFERS in "
               "%s (engine/esp) -- x %d/%d y %d/%d z %d/%d angFix %d/%d "
               "horizFix %d/%d sect %d/%d drawn range %s/%.1f (tol %.0f) "
               "yxaspect %s/%d viewport %s/%d,%d %dx%d%s xdim %s/%d ydim %s/%d; "
               "player sector %d ceilingstat 0x%X floorstat 0x%X%s; "
               "%d player-view call(s) this frame",
               ekm_proj_caller_name(ci), s_vcFrameCall, frame, which,
               e.x, esp.x, e.y, esp.y, e.z, esp.z,
               e.angFix, esp.angFix, e.horizFix, esp.horizFix, e.sect, esp.sect,
               engVrText, esp.range, EKM_VD_RANGE_TOL, engYxText, esp.yxaspect,
               engVpText, esp.vpX, esp.vpY, esp.vpW, esp.vpH,
               out->vp.fromGL ? "" : " (esp ASSUMED client rect)",
               engXdText, esp.xdim, engYdText, esp.ydim,
               psect, gotSi ? (unsigned)si.ceilStat : 0u,
               gotSi ? (unsigned)si.florStat : 0u, gotSi ? "" : " (unread)",
               e.calls);
      }
    }
    // Only a usable record: a freecam bisection row's record differs from ud by design.
    if (e.poseOk) {
      const EkViewPose drawn = { e.x, e.y, e.z, e.angFix, e.horizFix, e.sect,
                                 0.0, 0, 0, 0 };
      const EkViewPose udNow = { udCam.x, udCam.y, udCam.z, udCam.angFix,
                                 udCam.horizFix, udCam.sect, 0.0, 0, 0, 0 };
      const unsigned m = ekm_view_diff(drawn, udNow);
      // Bit 31 keeps this gate apart from the DIFFERS masks (bits 0-13).
      if (m) {
        s_vcUdMoved++;
        s_vcByCaller[ci][3]++;
        if (vc_line_due(0x80000000u | m, GetTickCount())) {
          char which[64];
          which[0] = 0;
          for (int i = 0; i < EKM_VD_FIELD_COUNT; i++) {
            if (!(m & (1u << i))) continue;
            strncat_s(which, sizeof(which), which[0] ? "," : "", _TRUNCATE);
            strncat_s(which, sizeof(which), ekm_view_diff_name(i), _TRUNCATE);
          }
          ek_log("project vs engine: %s (call %u this frame) frame %u ud MOVED "
                 "AFTER DRAW in %s (drawn/ud now) -- x %d/%d y %d/%d z %d/%d "
                 "angFix %d/%d horizFix %d/%d sect %d/%d; the projection uses "
                 "the drawn pose",
                 ekm_proj_caller_name(ci), s_vcFrameCall, frame, which,
                 e.x, udCam.x, e.y, udCam.y, e.z, udCam.z, e.angFix,
                 udCam.angFix, e.horizFix, udCam.horizFix, e.sect, udCam.sect);
        }
      }
    }
  }

  const unsigned now = GetTickCount();
  const unsigned key = s_vcCompared + s_vcNoRecord;
  if (key != s_vcTotalsKey && (unsigned)(now - s_vcTotalsMs) >= 5000u) {
    s_vcTotalsMs = now;
    s_vcTotalsKey = key;
    char fields[320], callers[512];
    fields[0] = 0;
    callers[0] = 0;
    for (int i = 0; i < EKM_VD_FIELD_COUNT; i++) {
      char one[40];
      _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s %u", i ? " " : "",
                  ekm_view_diff_name(i), s_vcField[i]);
      strncat_s(fields, sizeof(fields), one, _TRUNCATE);
    }
    for (int c = 0; c <= EK_PC_COUNT; c++) {
      char one[64];
      _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s %u/%u/%u/%u", c ? " " : "",
                  ekm_proj_caller_name(c), s_vcByCaller[c][0],
                  s_vcByCaller[c][1], s_vcByCaller[c][2], s_vcByCaller[c][3]);
      strncat_s(callers, sizeof(callers), one, _TRUNCATE);
    }
    ek_log("project vs engine totals: calls compared %u, differing %u, no "
           "engine record %u, ud moved after draw %u; pose source calls "
           "record %u / ud %u, frames record %u / ud only %u; by field (%s); "
           "by caller compared/differing/no record/ud moved (%s)",
           s_vcCompared, s_vcDiffer, s_vcNoRecord, s_vcUdMoved, s_psCallsRec,
           s_psCallsUd, s_psFramesRec, s_psFramesUd, fields, callers);
  }
}

// Shared so camera.cpp samples the draw viewport with the code the ESP reads its own.
void project_read_view(EkViewRead* r) {
  load_gl();

  EkViewport vp = {0, 0, 0, 0, false};
  if (s_glGetIntegerv) {
    int v[4] = {0, 0, 0, 0};
    s_glGetIntegerv(0x0BA2, v);
    if (v[2] > 0 && v[3] > 0) {
      // GL's origin is bottom-left; ours is top-left like ImGui's.
      vp.x = v[0];
      vp.w = v[2];
      vp.h = v[3];
      vp.y = 0;
      vp.fromGL = true;
      vp.y = v[1];
    }
  }

  RECT rc = {0, 0, 0, 0};
  const HWND wnd = GetActiveWindow();
  if (wnd) GetClientRect(wnd, &rc);
  const int clientW = rc.right - rc.left;
  const int clientH = rc.bottom - rc.top;

  if (!vp.fromGL) {
    vp.x = 0; vp.y = 0; vp.w = clientW; vp.h = clientH;
  } else if (clientH > 0) {
    vp.y = clientH - (vp.y + vp.h);
  }
  r->vp = vp;
  r->clientW = clientW;
  r->clientH = clientH;
  r->xdim = clientW > 0 ? clientW : vp.w;
  r->ydim = clientH > 0 ? clientH : vp.h;
}

bool project_begin(EkProjection* out, int caller) {
  if (!out) return false;
  out->valid = false;
  if (!ud_camera(&out->cam)) return false;

  // The pose the engine DREW with, not ud.camera* now: AA's HUD rewrites ud.camerax/y after the 3D view (project.h). ud
  // still supplies fov and the pose whenever the rule refuses.
  const EkCamera udCam = out->cam;
  EkEngineView rec = {};
  const bool haveRec = camera_engine_view(&rec);
  const unsigned frame = camera_frame_number();
  const bool fromRecord =
      ekm_pose_use_record(haveRec ? 1 : 0, rec.poseOk, rec.frame, frame,
                          rec.sect, EK_MAXSECTORS);
  if (fromRecord)
    ekm_pose_apply(&out->cam, rec.x, rec.y, rec.z, rec.angFix, rec.horizFix,
                   rec.sect);
  pose_source_note(fromRecord, frame);

  EkViewRead view;
  project_read_view(&view);
  const EkViewport vp = view.vp;
  const int clientW = view.clientW;
  const int clientH = view.clientH;
  if (vp.w <= 0 || vp.h <= 0) return false;
  out->vp = vp;

  // Read before the viewing range, which the correction needs. The engine derives viewingrange from the WINDOW
  // (engine.cpp:13727-13758), so this pair builds the default-global term in the correction and in ekm_build_proj
  // alike.
  const float xdim = (float)view.xdim;
  const float ydim = (float)view.ydim;

  const int yrep = player_yrepeat();
  const float derivedRange = ekm_viewingrange(out->cam.fov, yrep);
  out->viewingRange = derivedRange;

  // AA's scope zoom, applied. setaspect vr yxaspect runs after the engine's renderSetAspect (game.cpp:895) and before
  // renderDrawRoomsQ16 (:1142), and assigns viewingrange (gameexec.cpp:3876-3880): vr REPLACES the range. Dividing by
  // ekm_default_viewingrange_term composes to vr exactly; vr/65536 applied the term twice.
  int zoomin = 0, vr = 0;
  const bool gotZoomin = gamevars_zoomin(EK_PROJ_LOCAL_PLAYER, &zoomin);
  const bool gotVr = gamevars_vr(&vr);
  float correctedRange = 0.0f;
  const bool corrected = ekm_aa_corrected_range(gotZoomin, zoomin, gotVr, vr,
                                                (int)xdim, (int)ydim,
                                                &correctedRange);
  if (corrected) out->viewingRange = correctedRange;

  zoom_trace(out->cam.fov, yrep, derivedRange, gotZoomin, zoomin, gotVr, vr,
             corrected, out->viewingRange, (int)xdim, (int)ydim);
  engine_compare(out, (int)xdim, (int)ydim, caller, udCam);

  // Polymost-only: AA declares GAMEFLAG_NOPOLYMER, so there is no Polymer arm to protect. vp is the 3D viewport,
  // xdim/ydim the WINDOW (the engine derives its range from the window, the rest from the viewport); see
  // ekm_build_proj.
  out->g = ekm_build_proj((double)out->cam.angFix / 65536.0,
                          (float)out->cam.horizFix / 65536.0f,
                          out->viewingRange, vp.w, vp.h, (int)xdim, (int)ydim);
  if (!out->g.valid) return false;

  static bool logged = false;
  if (!logged) {
    logged = true;
    ek_log("project: viewport %d,%d %dx%d (%s), client %dx%d",
           vp.x, vp.y, vp.w, vp.h, vp.fromGL ? "from GL" : "ASSUMED client rect",
           clientW, clientH);
    // The camera's own horiz is in this one-shot line because it can fire before the camera is live (it once recorded
    // shang 0.51309, i.e. horiz 0). Two ranges: fov/yrepeat give the first, AA's zoom the second, which every later
    // number used.
    ek_log("project: fov %d, player yrepeat %d -> derived viewingrange %.0f, "
           "used %.0f; cam ang "
           "%.2f horiz %.2f; build proj halfx %.1f horiz %.1f yxscale %.4f "
           "gvr %.5f shang %.5f chang %.5f",
           out->cam.fov, yrep, derivedRange, out->viewingRange,
           (double)out->cam.angFix / 65536.0, (double)out->cam.horizFix / 65536.0,
           out->g.halfx, out->g.horiz, out->g.yxscale, out->g.gvr,
           out->g.shang, out->g.chang);
  }

  out->valid = true;
  return true;
}

EkmVec3 project_to_build(const EkProjection& p, int wx, int wy, int wz) {
  return ekm_build_rel(wx, wy, wz, p.cam.x, p.cam.y, p.cam.z);
}

bool project_build_point(const EkProjection& p, EkmVec3 rel, float* sx, float* sy) {
  if (!p.valid) return false;
  EkScreenPtF s = { 0.0f, 0.0f, false };
  if (!ekm_project_build(p.g, (double)rel.x, (double)rel.y, (double)rel.z,
                         p.vp.x, p.vp.y, &s))
    return false;
  if (sx) *sx = s.x;
  if (sy) *sy = s.y;
  return true;
}

// Instrumentation only: routed through ekm_project_build_diag so a refusal names its reason. p.valid false has no
// reason in EkBuildProjectReason and reports INVALID.
bool project_build_point_diag(const EkProjection& p, EkmVec3 rel, float* sx, float* sy,
                              EkBuildProjectReason* reason, double* outGdepth, double* outOz2) {
  if (!p.valid) {
    if (reason) *reason = EKM_BUILD_PROJECT_REFUSE_INVALID;
    if (outGdepth) *outGdepth = 0.0;
    if (outOz2) *outOz2 = 0.0;
    return false;
  }
  EkScreenPtF s = { 0.0f, 0.0f, false };
  if (!ekm_project_build_diag(p.g, (double)rel.x, (double)rel.y, (double)rel.z,
                              p.vp.x, p.vp.y, &s, reason, outGdepth, outOz2))
    return false;
  if (sx) *sx = s.x;
  if (sy) *sy = s.y;
  return true;
}

// Split in two so the segment path can clip between them; ekm_project_build applies the near clip itself.
bool project_point(const EkProjection& p, int wx, int wy, int wz,
                   float* sx, float* sy) {
  if (!p.valid) return false;
  return project_build_point(p, project_to_build(p, wx, wy, wz), sx, sy);
}

// `want` is unused for r_screenxy, which always logs its value.
static void check_one_assumption(const char* name, int want) {
  int have = 0;
  const bool readOk = cvar_get_int(name, &have);
  const EkProjAssumptionVerdict v = ekm_proj_assumption_verdict(readOk, have, want);
  if (v == EKM_PROJ_ASSUME_UNREADABLE) {
    ek_log("project: assumption unchecked -- %s could not be read", name);
  } else if (v == EKM_PROJ_ASSUME_MISMATCH) {
    ek_log("project: assumption broken -- %s is %d, the projection assumes %d; "
           "ESP and world quads will be misplaced", name, have, want);
  }
}

void project_check_assumptions(void) {
  check_one_assumption("r_projectionhack", 2);
  check_one_assumption("r_yshearing", 0);
  check_one_assumption("r_usenewaspect", 1);

  // No fixed expected value (project.h), so read and logged on its own.
  int screenxy = 0;
  if (cvar_get_int("r_screenxy", &screenxy)) {
    ek_log("project: r_screenxy is %d -- yxaspect is 81920 only while the "
           "render resolution matches this monitor's aspect", screenxy);
  } else {
    ek_log("project: assumption unchecked -- r_screenxy could not be read");
  }
}
