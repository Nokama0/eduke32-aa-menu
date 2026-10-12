#pragma once

// Shot tracers: our predicted ray (to where ek_hitscan stops) beside the engine's impact (the spawned spark,
// Proj_InsertShotspark, player.cpp:428); divergence is the bug. Pairing is a heuristic, so the actual line needs a
// pair.

const int EKM_TRACER_MAX = 64;

const unsigned EKM_TRACER_FADE_MS = 10000;

// The spark spawns in the firing tic and the scan sees it a frame or two later; a quarter second cannot steal the next
// pull's.
const unsigned EKM_TRACER_PAIR_MS = 250;

// Ints, not bool: MSVC /O2 loses a bool member of a struct returned by an inline function.
struct EkTracer {
  int ox, oy, oz;
  int px, py, pz;
  int ax, ay, az;
  int haveActual;
  unsigned ms;
};

// `next` is where the next insert lands; the newest is at next - 1.
struct EkTracerRing {
  EkTracer slot[EKM_TRACER_MAX];
  int count;
  int next;
};

inline void ekm_tracer_reset(EkTracerRing* r) {
  if (!r) return;
  r->count = 0;
  r->next = 0;
}

// Overwrite, never refuse: a full ring that dropped new shots would show last minute's burst.
inline void ekm_tracer_add(EkTracerRing* r, const EkTracer& t) {
  if (!r) return;
  r->slot[r->next] = t;
  r->next = (r->next + 1) % EKM_TRACER_MAX;
  if (r->count < EKM_TRACER_MAX) r->count++;
}

inline int ekm_tracer_index(const EkTracerRing* r, int i) {
  if (!r || i < 0 || i >= r->count) return -1;
  int k = r->next - 1 - i;
  while (k < 0) k += EKM_TRACER_MAX;
  return k;
}

inline int ekm_tracer_alpha(unsigned ageMs) {
  if (ageMs >= EKM_TRACER_FADE_MS) return 0;
  return 255 - (int)((ageMs * 255u) / EKM_TRACER_FADE_MS);
}

// Unsigned difference: exact across the wallclock wrap, and an earlier-stamped impact is refused, not paired backwards.
inline bool ekm_tracer_pair_ok(unsigned shotMs, unsigned impactMs, unsigned windowMs) {
  return (unsigned)(impactMs - shotMs) <= windowMs;
}

// --- tracer.cpp (DLL only) ---

bool tracer_impacts_wanted(void);

// One round the engine is about to fire, from the P_PreFireHitscan detour with its exact origin, angle and pitch.
// Nothing is cast here: ek_hitscan writes clipsectorlist, so it runs only in the SwapBuffers hook (tracer_frame).
void tracer_note_shot(int x, int y, int z, int shootAng, int zvel);

// `ours` is 0 for a round somebody else fired (the turret); those never pair.
void tracer_note_impact(int x, int y, int z, int ours);

// SwapBuffers hook only, before the impact scan, so this frame's shots precede its sparks.
void tracer_frame(void);

int tracer_draw(void);

// One world-space segment, clipped at the near planes (gdepth: ekm_build_clip_near; tilted oz2: ekm_build_clip_tilt)
// and the viewport (ekm_clip_segment_2d), then drawn; the ESP's secret-trigger line also calls it. False when nothing
// survives. `aFront`/`bFront` say which ends are real: a clipped end is not the thing that was there.
struct EkProjection;
struct EkWorldLine { float ax, ay, bx, by; int aFront, bFront; };

// Refusal counters are split by caller so the tracer's numbers are not diluted by the ESP's secret lines.
enum EkWorldlineCaller { EKM_WL_CALLER_TRACER = 0, EKM_WL_CALLER_OTHER = 1 };

// `lineW`/`keyline` (esp_strokes, esp_layout.h): defaults draw one canvas_line; wider uses canvas_line_w.
bool worldline_draw(const EkProjection& p, int x0, int y0, int z0,
                    int x1, int y1, int z1, unsigned colour, EkWorldLine* out,
                    EkWorldlineCaller caller, int lineW = 1, int keyline = 0);
