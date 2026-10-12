// The locates behind modelbounds.h: the engine's `models` array (found by the 'IDP3' magic) and its `tile2model`
// table. Every non-voxel entry is an md3model_t (mdsprite.cpp:1158-1279, mdload 2571). md3postload_common (:1516)
// rebuilds the frame box from [10:6] fixed-point vertices, so reads here are RAW.
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "modelbounds.h"
#include "gamestate.h"
#include "camerascan.h"
#include "imagescan.h"
#include "log.h"

// Counts and field offsets, not addresses: where `models` lives, where `head` starts in a block, where tile2model
// begins and its record width are all searched for. Only declaration order and two array dimensions are written down.

static const int EK_MDL_MAXTILES = 30720;
static const int EK_MDL_T2M_COUNT = 32512;

// tile2model_t, build.h:1616-1626: int16 modelid, framenum, nexttile; uint16 smoothduration; hudtyp *hudmem[2]; int8
// skinnum; char pal. The record size is derived (padding), never assumed.
static const int EK_MDL_T2M_MODELID  = 0;
static const int EK_MDL_T2M_FRAMENUM = 2;
static const int EK_MDL_T2M_NEXTTILE = 4;
static const int EK_MDL_T2M_HUDMEM   = 8;
static const int EK_MDL_T2M_HUDBYTES = 16;
static const int EK_MDL_T2M_PAL      = 25;
static const int EK_MDL_T2M_MINREC   = 26;

// SHARED_MODEL_DATA (mdsprite.h:18-21): int32 mdnum, shadeoff; float scale, bscale, zadd, yoffset; GLuint *texid;
// int32 flags; its total size varies and the magic search derives it. bscale is the def file's scale, not `scale`
// (md_setmisc, mdsprite.cpp:279); the box needs their product.
static const int EK_MDL_SM_MDNUM   = 0;
static const int EK_MDL_SM_SCALE   = 8;
static const int EK_MDL_SM_BSCALE  = 12;
static const int EK_MDL_SM_ZADD    = 16;
static const int EK_MDL_SM_YOFFSET = 20;

// md3head_t, mdsprite.h:143-158, from the magic: int32 id, vers; char nam[64]; int32 flags, numframes, numtags,
// numsurfs, numskins, ofsframes, ofstags, ofssurfs, eof; then three pointers. `frames` is derived and confirms head's
// base.
static const int EK_MDL_HD_NAM       = 8;
static const int EK_MDL_HD_NUMFRAMES = 76;
static const int EK_MDL_HD_NUMSURFS  = 84;
static const int EK_MDL_HD_FRAMES_LO = 104;
static const int EK_MDL_HD_FRAMES_HI = 168;

// md3frame_t, mdsprite.h:108-113: vec3f_t min, max, cen; float r; char nam[16]. 56 bytes.
static const int EK_MDL_FRAME_STRIDE = 56;

static const int EK_MDL_MIN_AGREE   = 4;
static const int EK_MDL_MAX_MODELS  = 4096;
static const int EK_MDL_STRIDE_RUN  = 64;
static const int EK_MDL_STRIDE_MIN  = 24;
static const int EK_MDL_STRIDE_MAX  = 256;
static const int EK_MDL_HUD_WINDOW  = 64;
static const int EK_MDL_HUD_MIN     = 56;
static const int EK_MDL_PAL_HOPS    = 64;
static const int EK_MDL_MAX_WINDOWS = 128;
static const int EK_MDL_SEEN       = 8;
static const int EK_MDL_HEAD_SEARCH = 512;
static const int EK_MDL_MAX_PROBE  = 16;
static const int EK_MDL_MODELID_CEILING = 4096;
static const int EK_MDL_ATTEMPTS = 60;

enum {
  EK_MDL_STATE_LOOKING = 0,
  EK_MDL_STATE_READY   = 1,
  EK_MDL_STATE_REFUSED = 2
};

// Written by the scan thread, read by the render thread; the state is stored last with an interlocked write, so READY
// means every field below is final. After READY only a refusal rewrites it, shutting the gate.
static volatile LONG s_state = EK_MDL_STATE_LOOKING;
static volatile LONG s_attempts = 0;

static const unsigned char* s_imageBase = NULL;
static const unsigned char* s_models = NULL;
static unsigned s_modelsSlot = 0;
static int s_modelCount = 0;
static int s_headOff = 0;
static int s_framesOff = 0;
static const unsigned char* s_t2m = NULL;
static int s_t2mStride = 0;

static volatile LONG s_movedLogged = 0;

bool modelbounds_ready(void) {
  return InterlockedCompareExchange(&s_state, 0, 0) == EK_MDL_STATE_READY;
}

// Not !ready: ready collapses LOOKING and REFUSED, and the scan rides the sampler for up to EK_MDL_ATTEMPTS ticks, so
// "not ready" for the first seconds means still looking, not a session-wide failure.
bool modelbounds_refused(void) {
  return InterlockedCompareExchange(&s_state, 0, 0) == EK_MDL_STATE_REFUSED;
}

static bool safe_read(const void* at, void* dst, size_t n) {
  __try {
    memcpy(dst, at, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool read_ptr(const void* at, const unsigned char** out) {
  const unsigned char* v = NULL;
  if (!safe_read(at, &v, sizeof(v))) return false;
  *out = v;
  return true;
}

static bool read_i32(const void* at, int* out) {
  int v = 0;
  if (!safe_read(at, &v, sizeof(v))) return false;
  *out = v;
  return true;
}

static bool read_f32(const void* at, float* out) {
  float v = 0.0f;
  if (!safe_read(at, &v, sizeof(v))) return false;
  *out = v;
  return true;
}

// An address the host allocator could have handed out: non-null, aligned, user space, NOT in the host image. Cheap:
// it runs against every 8-byte slot in the image's data.
static bool heap_plausible(const unsigned char* p, const unsigned char* base,
                           size_t imageSize) {
  const size_t v = (size_t)p;
  if (v == 0 || (v & 7u) != 0) return false;
  if (v < 0x10000u || v >= 0x00007FFFFFFFFFFFull) return false;
  if (p >= base && p < base + imageSize) return false;
  return true;
}

// The.bss hazard: tile2model is a ~1 MB static array, so its section's SizeOfRawData is zero and ek_image_sections
// cannot see it (imagescan.h:44-49). ek_image_data_extent walks VirtualSize. Intersected with gamestate's committed
// regions.
struct EkMdlWindow { const unsigned char* b; size_t n; };

// Whole pages: VirtualSize is not page-aligned, so the cursor drifted and.bss's window began 2996 bytes in, four
// bytes off the 8-byte record grid (the section before it ends at rva 0x005E4BB4); the sweep never saw a record's
// first byte.
static const unsigned char* page_up(const unsigned char* p) {
  return (const unsigned char*)(((size_t)p + 0xFFFu) & ~(size_t)0xFFFu);
}

static const unsigned char* align_up8(const unsigned char* p) {
  return (const unsigned char*)(((size_t)p + 7u) & ~(size_t)7u);
}

static int data_windows(const unsigned char* base, size_t imageSize,
                        EkMdlWindow* w, int max, int* capped) {
  *capped = 0;
  int n = 0;
  const int regions = gamestate_region_count();
  for (int i = 0; i < regions; i++) {
    const unsigned char* rb = NULL;
    size_t rn = 0;
    if (!gamestate_region_at(i, &rb, &rn)) continue;
    if (rb < base || rb >= base + imageSize) continue;
    const size_t avail = (size_t)(base + imageSize - rb);
    if (rn > avail) rn = avail;
    const unsigned char* const re = rb + rn;
    const unsigned char* p = rb;
    while (p < re) {
      unsigned db = 0, de = 0;
      if (!ek_image_data_extent(base, ek_image_rva(base, p), &db, &de)) {
        p = page_up(p + 1);
        continue;
      }
      const unsigned char* const sb = base + db;
      const unsigned char* const se = base + de;
      const unsigned char* const wb = p > sb ? p : sb;
      const unsigned char* const we = re < se ? re : se;
      if (we > wb) {
        if (n >= max) { *capped = 1; return n; }
        w[n].b = wb;
        w[n].n = (size_t)(we - wb);
        n++;
      }
      p = se > p ? page_up(se) : page_up(p + 1);
    }
  }
  return n;
}

// Whether `off` is md3head_t.frames: it must reach a plausible box and the LAST frame must read too. `strict` demands
// a real box only when deriving the offset; walking asks only readable, finite, not inverted, since a model FLAT on
// an axis (decal, plate) has min == max and refusing it truncated modelCount.
static bool frames_off_ok(const unsigned char* head, int off, int numframes,
                          bool strict, const unsigned char** framesOut) {
  const unsigned char* f = NULL;
  if (!read_ptr(head + off, &f)) return false;
  if (f == NULL || ((size_t)f & 3u) != 0) return false;
  if ((size_t)f < 0x10000u || (size_t)f >= 0x00007FFFFFFFFFFFull) return false;
  float v[6];
  if (!safe_read(f, v, sizeof(v))) return false;
  if (strict) {
    if (!ekm_mdl_frame_plausible(v[0], v[1], v[2], v[3], v[4], v[5]))
      return false;
  } else {
    for (int i = 0; i < 6; i++)
      if (!isfinite(v[i])) return false;
    if (v[0] > v[3] || v[1] > v[4] || v[2] > v[5]) return false;
  }
  float tail[6];
  const unsigned char* const last = f + (size_t)(numframes - 1) * EK_MDL_FRAME_STRIDE;
  if (!safe_read(last, tail, sizeof(tail))) return false;
  *framesOut = f;
  return true;
}

// Voxel models (mdnum 1, mdsprite.cpp:2544) carry no magic and are reported separately, not as the walk's end.
static bool entry_is_md3(const unsigned char* blk, int headOff) {
  unsigned id = 0;
  if (!safe_read(blk + headOff, &id, sizeof(id))) return false;
  return id == EK_MDL_IDP3_MAGIC;
}

// The terminator must be stronger than the tail: md_loadmodel grows the array with an Xrealloc that does not zero
// (mdsprite.cpp:262), so the tail is uninitialised heap where "first int32 is 1" is ordinary. `scale` is set by both
// loaders (:1058,:1332) and never zero or absurd.
static bool shared_scale_ok(const unsigned char* blk) {
  float scale = 0.0f;
  if (!read_f32(blk + EK_MDL_SM_SCALE, &scale)) return false;
  return isfinite(scale) && scale > 0.0f && scale < 1.0e6f;
}

static bool entry_is_voxel(const unsigned char* blk) {
  int mdnum = 0;
  if (!read_i32(blk + EK_MDL_SM_MDNUM, &mdnum)) return false;
  if (mdnum != 1) return false;
  return shared_scale_ok(blk);
}

struct ModelsFunnel {
  int windows;
  int windowsCapped;
  size_t imageBytes;
  int sweepFaulted;
  int sweepComplete;
  int t2mHoles;
  int t2mStrides;
  int t2mBases;
  int t2mTables;
  int t2mStride;
  int t2mDefined;
  int probeIds;
  int probeMax;
  int slots;
  int candidates;
  int probed;
  int shaped;
  int headOff, headVotes, headRunner;
  int framesOff, framesVotes, framesRunner;
  int arrays;
  int bestCount;
  int bestMagic;
  int globalHits;
};

// Targeted search: sweeping image data for pointer-shaped values (42418 candidates, no magic) and the process for the
// magic (9.0 GB) looked in the wrong place. `models` and `tile2model` are globals in the 27 MB image; heap blocks are
// only CHECKED. So tile2model is located first and its modelids target the `models` search.

// The magic's position is head's base, found not computed: SHARED_MODEL_DATA and IDMODEL_SHARED_DATA
// (mdsprite.h:18-27) sizes depend on the build. `readFailed` separates -1 for an unreadable block from -1 for no
// magic.
static int block_head_off(const unsigned char* blk, int* readFailed) {
  unsigned char buf[EK_MDL_HEAD_SEARCH];
  *readFailed = 0;
  if (!safe_read(blk, buf, sizeof(buf))) { *readFailed = 1; return -1; }
  for (int off = 0; off + EK_MDL_HD_NUMSURFS + 4 <= (int)sizeof(buf); off += 8) {
    unsigned id = 0;
    int numframes = 0, numsurfs = 0;
    memcpy(&id, buf + off, 4);
    if (id != EK_MDL_IDP3_MAGIC) continue;
    memcpy(&numframes, buf + off + EK_MDL_HD_NUMFRAMES, 4);
    memcpy(&numsurfs, buf + off + EK_MDL_HD_NUMSURFS, 4);
    // md3surf_t.id carries the same fourcc (mdsprite.cpp:1197); there a surface has numshaders and numtris, and its
    // triangle count is far above 256.
    if (numframes < 1 || numframes > 4096) continue;
    if (numsurfs < 1 || numsurfs > 256) continue;
    return off;
  }
  return -1;
}

struct MdlShape { int headOff; int framesOff; };

// A models[] entry at an agreed shape: md3 with the magic, a voxel, or the array's end. Frames test is the relaxed
// one (frames_off_ok).
static bool entry_is_model(const unsigned char* blk, const MdlShape* sh,
                           const unsigned char* base, size_t imageSize) {
  if (!heap_plausible(blk, base, imageSize)) return false;
  if (entry_is_md3(blk, sh->headOff)) {
    if (!shared_scale_ok(blk)) return false;
    int numframes = 0;
    if (!read_i32(blk + sh->headOff + EK_MDL_HD_NUMFRAMES, &numframes))
      return false;
    if (numframes < 1 || numframes > 4096) return false;
    const unsigned char* f = NULL;
    return frames_off_ok(blk + sh->headOff, sh->framesOff, numframes, false, &f);
  }
  return entry_is_voxel(blk);
}

static int array_walk(const unsigned char* arr, const MdlShape* sh,
                      const unsigned char* base, size_t imageSize, int* magic) {
  int count = 0;
  *magic = 0;
  for (int i = 0; i < EK_MDL_MAX_MODELS; i++) {
    const unsigned char* blk = NULL;
    if (!read_ptr(arr + (size_t)i * sizeof(void*), &blk)) break;
    if (!entry_is_model(blk, sh, base, imageSize)) break;
    if (entry_is_md3(blk, sh->headOff)) (*magic)++;
    count++;
  }
  return count;
}

struct ModelsFound {
  const unsigned char* array;
  unsigned slotRva;
  int count;
  int magic;
};

// A candidate array tested at the indices tile2model named: each is an engine-recorded modelid, so on the real array
// every one leads to a model. The shape is voted on and the winner must be unique (head and frames offsets are the
// same for every model); a tie refuses.
static bool array_probe(const unsigned char* arr, const int* ids, int nprobe,
                        int need, const unsigned char* base, size_t imageSize,
                        ModelsFunnel* fn, MdlShape* out) {
  const int nb = EK_MDL_HEAD_SEARCH / 8 + 1;
  const int nf = (EK_MDL_HD_FRAMES_HI - EK_MDL_HD_FRAMES_LO) / 8 + 1;
  int bvote[EK_MDL_HEAD_SEARCH / 8 + 1];
  int fvote[(EK_MDL_HD_FRAMES_HI - EK_MDL_HD_FRAMES_LO) / 8 + 1];
  memset(bvote, 0, sizeof(bvote));
  memset(fvote, 0, sizeof(fvote));
  int shaped = 0;

  for (int i = 0; i < nprobe; i++) {
    const unsigned char* blk = NULL;
    if (!read_ptr(arr + (size_t)ids[i] * sizeof(void*), &blk)) return false;
    if (!heap_plausible(blk, base, imageSize)) return false;
    if (i == 0) fn->probed++;
    int readFailed = 0;
    const int off = block_head_off(blk, &readFailed);
    if (off < 0) continue;
    int numframes = 0;
    if (!read_i32(blk + off + EK_MDL_HD_NUMFRAMES, &numframes)) continue;
    if (!shared_scale_ok(blk)) continue;
    bvote[off / 8]++;
    int gotFrames = 0;
    for (int fo = EK_MDL_HD_FRAMES_LO; fo <= EK_MDL_HD_FRAMES_HI; fo += 8) {
      const unsigned char* f = NULL;
      if (!frames_off_ok(blk + off, fo, numframes, true, &f)) continue;
      fvote[(fo - EK_MDL_HD_FRAMES_LO) / 8]++;
      gotFrames = 1;
    }
    if (gotFrames) shaped++;
  }
  if (shaped == 0) return false;

  int bi = -1, bv = 0, br = 0;
  for (int i = 0; i < nb; i++) {
    if (bvote[i] > bv) { br = bv; bv = bvote[i]; bi = i; }
    else if (bvote[i] > br) { br = bvote[i]; }
  }
  int fi = -1, fv = 0, fr = 0;
  for (int i = 0; i < nf; i++) {
    if (fvote[i] > fv) { fr = fv; fv = fvote[i]; fi = i; }
    else if (fvote[i] > fr) { fr = fvote[i]; }
  }
  if (bv > fn->headVotes) {
    fn->headVotes = bv;
    fn->headRunner = br;
    fn->headOff = bi < 0 ? 0 : bi * 8;
    fn->framesVotes = fv;
    fn->framesRunner = fr;
    fn->framesOff = fi < 0 ? 0 : EK_MDL_HD_FRAMES_LO + fi * 8;
  }
  if (bi < 0 || fi < 0) return false;
  if (bv < need || fv < need) return false;
  if (bv <= br || fv <= fr) return false;
  out->headOff = bi * 8;
  out->framesOff = EK_MDL_HD_FRAMES_LO + fi * 8;
  return true;
}

// Every eight-aligned slot tested as a model pointer array; the answering slot IS the `models` global.
static int locate_models(const unsigned char* base, size_t imageSize,
                         const EkMdlWindow* w, int nw, const int* ids,
                         int nprobe, int need, ModelsFunnel* fn,
                         MdlShape* shape, ModelsFound* best) {
  const unsigned char* seen[EK_MDL_SEEN];
  int nseen = 0;
  int found = 0;
  if (nprobe <= 0) return 0;
  for (int i = 0; i < nw; i++) {
    const unsigned char* p = align_up8(w[i].b);
    const unsigned char* const e = w[i].b + w[i].n;
    while (p + sizeof(void*) <= e) {
      const unsigned char* arr = NULL;
      fn->slots++;
      if (!safe_read(p, &arr, sizeof(arr))) { fn->sweepFaulted++; break; }
      if (!heap_plausible(arr, base, imageSize)) { p += sizeof(void*); continue; }
      fn->candidates++;
      MdlShape sh;
      memset(&sh, 0, sizeof(sh));
      if (!array_probe(arr, ids, nprobe, need, base, imageSize, fn, &sh)) {
        p += sizeof(void*);
        continue;
      }
      fn->shaped++;
      int magic = 0;
      const int count = array_walk(arr, &sh, base, imageSize, &magic);
      if (count > fn->bestCount) {
        fn->bestCount = count;
        fn->bestMagic = magic;
      }
      if (magic < need) { p += sizeof(void*); continue; }

      fn->globalHits++;
      bool known = false;
      for (int k = 0; k < nseen; k++)
        if (seen[k] == arr) { known = true; break; }
      if (!known) {
        if (nseen < EK_MDL_SEEN) seen[nseen++] = arr;
        if (found == 0) {
          best->array = arr;
          best->count = count;
          best->magic = magic;
          best->slotRva = ek_image_rva(base, p);
          *shape = sh;
        }
        found++;
      }
      p += sizeof(void*);
    }
  }
  fn->arrays = found;
  return found;
}


// A virgin record: freeallmodels (mdsprite.cpp:133-134) memsets the table to -1 and zeroes only hudmem, only below
// MAXTILES, so an untouched record is 8 bytes of 0xFF then 16 zero bytes. That repetition gives the stride.
static bool rec_is_virgin_hole(const unsigned char* r) {
  for (int i = 0; i < 8; i++)
    if (r[i] != 0xFF) return false;
  for (int i = 0; i < EK_MDL_T2M_HUDBYTES; i++)
    if (r[EK_MDL_T2M_HUDMEM + i] != 0x00) return false;
  return true;
}

static bool rec_hudmem_zero(const unsigned char* r) {
  for (int i = 0; i < EK_MDL_T2M_HUDBYTES; i++)
    if (r[EK_MDL_T2M_HUDMEM + i] != 0x00) return false;
  return true;
}

static bool t2m_read_i16(const unsigned char* rec, int off, int* out) {
  short v = 0;
  if (!gamestate_region_contains(rec + off, sizeof(v))) return false;
  if (!safe_read(rec + off, &v, sizeof(v))) return false;
  *out = v;
  return true;
}

// Stride from the repetition: the SMALLEST step turning one virgin record into the next, then required to explain a
// long run (the reverse accepts a multiple of the stride).
static int t2m_derive_stride(const unsigned char* r, const unsigned char* end) {
  int stride = 0;
  for (int d = EK_MDL_STRIDE_MIN; d <= EK_MDL_STRIDE_MAX; d += 8) {
    if (r + d + EK_MDL_T2M_MINREC > end) return 0;
    if (rec_is_virgin_hole(r + d)) { stride = d; break; }
  }
  if (stride == 0) return 0;
  if (r + (size_t)EK_MDL_STRIDE_RUN * stride > end) return 0;
  for (int k = 0; k < EK_MDL_STRIDE_RUN; k++)
    if (!rec_is_virgin_hole(r + (size_t)k * stride)) return 0;
  return stride;
}

static const unsigned char* t2m_base_from(const unsigned char* r, int stride) {
  for (int j = 0; j < EK_MDL_T2M_COUNT; j++) {
    const unsigned char* const rec = r + (size_t)j * stride;
    if (!gamestate_region_contains(rec, EK_MDL_T2M_MINREC)) return NULL;
    if (rec_hudmem_zero(rec)) continue;
    int k = 1;
    for (; k < EK_MDL_HUD_WINDOW; k++) {
      const unsigned char* const q = rec + (size_t)k * stride;
      if (!gamestate_region_contains(q, EK_MDL_T2M_MINREC)) return NULL;
      if (rec_hudmem_zero(q)) break;
    }
    if (k < EK_MDL_HUD_WINDOW) continue;
    const size_t span = (size_t)EK_MDL_MAXTILES * (size_t)stride;
    if ((size_t)rec <= span) return NULL;
    return rec - span;
  }
  return NULL;
}

static bool t2m_validate(const unsigned char* b, int stride, int ceiling,
                         int* definedOut) {
  int defined = 0;
  for (int i = 0; i < EK_MDL_T2M_COUNT; i++) {
    int modelid = 0;
    if (!t2m_read_i16(b + (size_t)i * stride, EK_MDL_T2M_MODELID, &modelid))
      return false;
    if (!ekm_mdl_modelid_plausible(modelid, ceiling)) return false;
    if (modelid >= 0) defined++;
  }
  if (defined < 1) return false;

  int zeroed = 0, kept = 0;
  for (int k = 1; k <= EK_MDL_HUD_WINDOW; k++) {
    const unsigned char* const below = b + (size_t)(EK_MDL_MAXTILES - k) * stride;
    if (!gamestate_region_contains(below, EK_MDL_T2M_MINREC)) return false;
    if (rec_hudmem_zero(below)) zeroed++;
  }
  for (int k = 0; k < EK_MDL_HUD_WINDOW; k++) {
    const unsigned char* const above = b + (size_t)(EK_MDL_MAXTILES + k) * stride;
    if (!gamestate_region_contains(above, EK_MDL_T2M_MINREC)) return false;
    if (!rec_hudmem_zero(above)) kept++;
  }
  if (zeroed < EK_MDL_HUD_MIN || kept < EK_MDL_HUD_MIN) return false;

  *definedOut = defined;
  return true;
}

struct T2mHit {
  const unsigned char* base;
  int stride;
  int defined;
};

// tile2model is located FIRST: a static array in.bss, a 27 MB scan of mapped bytes rather than a 9 GB heap sweep. Its
// modelids make the `models` search targeted.
static int locate_t2m(const unsigned char* base, const EkMdlWindow* w, int nw,
                      int ceiling, ModelsFunnel* fn, T2mHit* best,
                      int* faulted) {
  int found = 0;
  *faulted = 0;
  for (int i = 0; i < nw; i++) {
    const unsigned char* p = align_up8(w[i].b);
    const unsigned char* const e = w[i].b + w[i].n;
    while (p + EK_MDL_STRIDE_MAX <= e) {
      bool hole = false;
      __try {
        hole = rec_is_virgin_hole(p);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        break;
      }
      if (!hole) { p += 8; continue; }
      fn->t2mHoles++;

      int stride = 0;
      __try {
        stride = t2m_derive_stride(p, e);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        break;
      }
      if (stride == 0) { p += 8; continue; }
      fn->t2mStrides++;

      const unsigned char* cand = NULL;
      int defined = 0;
      bool validated = false;
      __try {
        cand = t2m_base_from(p, stride);
        if (cand != NULL) fn->t2mBases++;
        validated = cand != NULL &&
                    t2m_validate(cand, stride, ceiling, &defined);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        break;
      }
      if (validated) {
        fn->t2mTables++;
        if (found == 0) {
          best->base = cand;
          best->stride = stride;
          best->defined = defined;
        }
        found++;
        p = cand + (size_t)EK_MDL_T2M_COUNT * stride;
        continue;
      }
      p += (size_t)EK_MDL_STRIDE_RUN * stride;
    }
  }
  return found;
}

static int t2m_probe_ids(const unsigned char* b, int stride, int* ids, int max,
                         int* maxId) {
  int n = 0;
  *maxId = -1;
  for (int i = 0; i < EK_MDL_T2M_COUNT; i++) {
    int modelid = 0;
    if (!t2m_read_i16(b + (size_t)i * stride, EK_MDL_T2M_MODELID, &modelid))
      break;
    if (modelid < 0) continue;
    if (modelid > *maxId) *maxId = modelid;
    if (n >= max) continue;
    bool known = false;
    for (int k = 0; k < n; k++)
      if (ids[k] == modelid) { known = true; break; }
    if (!known) ids[n++] = modelid;
  }
  return n;
}

static void head_name(const unsigned char* head, char* out, int cap) {
  char raw[64];
  out[0] = '?';
  out[1] = 0;
  if (!safe_read(head + EK_MDL_HD_NAM, raw, sizeof(raw))) return;
  int n = 0;
  for (int i = 0; i < (int)sizeof(raw) && raw[i] != 0 && n < cap - 1; i++)
    out[n++] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? raw[i] : '.';
  out[n] = 0;
  if (n == 0) { out[0] = '?'; out[1] = 0; }
}

static void log_funnel(int attempt, const ModelsFunnel* fn) {
  ek_log("modelbounds: no model system yet (attempt %d of %d) -- %d window(s) "
         "of the image's non-executable data, %llu byte(s), %d sweep(s) "
         "faulted. THE SWEEP WAS %s",
         attempt, EK_MDL_ATTEMPTS, fn->windows,
         (unsigned long long)fn->imageBytes, fn->sweepFaulted,
         fn->sweepComplete ? "COMPLETE -- every eligible byte was read"
                           : "PARTIAL -- some eligible bytes were never read");
  ek_log("modelbounds:   tile2model: %d virgin record(s) found, %d yielding a "
         "stride, %d a candidate base, %d passing all %d records; stride %d, "
         "%d record(s) naming a model, %d distinct modelid(s) with the largest "
         "%d",
         fn->t2mHoles, fn->t2mStrides, fn->t2mBases, fn->t2mTables,
         EK_MDL_T2M_COUNT, fn->t2mStride, fn->t2mDefined, fn->probeIds,
         fn->probeMax);
  ek_log("modelbounds:   models: %d slot(s), %d looked like a heap pointer, %d "
         "led somewhere at the first index tile2model named, %d agreed on a "
         "shape; vote block+%d (%d, runner-up %d) frames+%d (%d, runner-up %d); "
         "%d array(s) validated, longest walk %d entry(s) of which %d carried "
         "the magic, %d image slot(s) hold it",
         fn->slots, fn->candidates, fn->probed, fn->shaped, fn->headOff,
         fn->headVotes, fn->headRunner, fn->framesOff, fn->framesVotes,
         fn->framesRunner, fn->arrays, fn->bestCount, fn->bestMagic,
         fn->globalHits);
}

static void log_hex(const unsigned char* base, const unsigned char* at,
                    const char* what) {
  unsigned char buf[64];
  if (!safe_read(at, buf, sizeof(buf))) {
    ek_log("modelbounds:   %s at rva 0x%07X: UNREADABLE", what,
           ek_image_rva(base, at));
    return;
  }
  char hex[3 * 64 + 1];
  static const char digits[] = "0123456789ABCDEF";
  for (int i = 0; i < 64; i++) {
    hex[i * 3] = digits[buf[i] >> 4];
    hex[i * 3 + 1] = digits[buf[i] & 0xF];
    hex[i * 3 + 2] = ' ';
  }
  hex[3 * 64] = 0;
  ek_log("modelbounds:   %s at rva 0x%07X: %s", what, ek_image_rva(base, at),
         hex);
}

static void log_byte_census(const unsigned char* base, const EkMdlWindow* w,
                            int nw) {
  size_t total = 0, ffBytes = 0;
  size_t ffRuns = 0;
  size_t virginAligned = 0;
  const unsigned char* firstRun = NULL;
  const EkMdlWindow* big = NULL;

  for (int i = 0; i < nw; i++) {
    if (!big || w[i].n > big->n) big = &w[i];
    total += w[i].n;
    __try {
      const unsigned char* const b = w[i].b;
      const size_t n = w[i].n;
      size_t run = 0;
      for (size_t k = 0; k < n; k++) {
        if (b[k] == 0xFF) {
          run++;
          ffBytes++;
          continue;
        }
        if (run >= 8) {
          ffRuns++;
          if (!firstRun) firstRun = b + k - run;
        }
        run = 0;
      }
      if (run >= 8) {
        ffRuns++;
        if (!firstRun) firstRun = b + n - run;
      }
      const unsigned char* p = align_up8(b);
      const unsigned char* const e = b + n;
      for (; p + 32 <= e; p += 8)
        if (rec_is_virgin_hole(p)) virginAligned++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ek_log("modelbounds:   census: window %d faulted mid-count", i + 1);
    }
  }

  ek_log("modelbounds: BYTE CENSUS -- %llu byte(s) in %d window(s): %llu are "
         "0xFF, in %llu run(s) of eight or more; %llu eight-aligned position(s) "
         "match the virgin record. THE PREDICATE REQUIRES bytes 0..7 == FF and "
         "bytes 8..23 == 00, and DOES NOT EXAMINE bytes 24..31",
         (unsigned long long)total, nw, (unsigned long long)ffBytes,
         (unsigned long long)ffRuns, (unsigned long long)virginAligned);

  if (!big) return;
  ek_log("modelbounds:   the largest window is rva 0x%07X..0x%07X, %llu bytes, "
         "start %s eight-aligned",
         ek_image_rva(base, big->b), ek_image_rva(base, big->b + big->n),
         (unsigned long long)big->n,
         ((size_t)big->b & 7u) ? "*** NOT ***" : "is");
  if (firstRun) log_hex(base, firstRun, "first run of eight 0xFF");
  log_hex(base, align_up8(big->b), "largest window, at its start");
  log_hex(base, align_up8(big->b + big->n / 4), "largest window, one quarter in");
  log_hex(base, align_up8(big->b + big->n / 2), "largest window, halfway in");
  log_hex(base, align_up8(big->b + (big->n / 4) * 3),
          "largest window, three quarters in");
}

static void refuse(const char* why) {
  // No fallback to name: the flat quad was deleted with the volume, so a lock without bounds draws nothing and its
  // marker and Flip/Leave label remain.
  ek_log("modelbounds: REFUSED -- %s. No model bounds this session; combination "
         "locks keep their marker and label but draw no volume", why);
  InterlockedExchange(&s_state, EK_MDL_STATE_REFUSED);
}

bool modelbounds_scan(void) {
  const LONG state = InterlockedCompareExchange(&s_state, 0, 0);
  if (state == EK_MDL_STATE_READY) return true;
  if (state == EK_MDL_STATE_REFUSED) return false;

  const LONG attempt = InterlockedIncrement(&s_attempts);

  // s_base is camerascan's, NULL until the overlay draws; gamestate's is up from the loader. Same image; take
  // whichever exists.
  const unsigned char* const base = s_base ? s_base : gamestate_module_base();
  const size_t imageSize = base ? host_image_size(base) : 0;
  if (!base || imageSize == 0) return false;

  gamestate_regions_refresh();

  EkMdlWindow w[EK_MDL_MAX_WINDOWS];
  int capped = 0;
  const int nw = data_windows(base, imageSize, w, EK_MDL_MAX_WINDOWS, &capped);
  if (capped) {
    refuse("the host image's committed non-executable data does not fit in "
           "128 windows, so a sweep of it would be partial and a partial sweep "
           "must not be reported as a whole one");
    return false;
  }

  if (attempt == 1) {
    for (int i = 0; i < nw; i++)
      ek_log("modelbounds:   window %d of %d: rva 0x%07X..0x%07X, %llu bytes%s",
             i + 1, nw, ek_image_rva(base, w[i].b),
             ek_image_rva(base, w[i].b + w[i].n), (unsigned long long)w[i].n,
             ((size_t)w[i].b & 0xFFFu) ? " *** START NOT PAGE-ALIGNED ***" : "");
    log_byte_census(base, w, nw);
  }

  ModelsFound mh;
  memset(&mh, 0, sizeof(mh));
  MdlShape shape;
  memset(&shape, 0, sizeof(shape));
  ModelsFunnel fn;
  memset(&fn, 0, sizeof(fn));
  fn.windows = nw;
  for (int i = 0; i < nw; i++) fn.imageBytes += w[i].n;

  // tile2model first, in image data: a static array memset at startup, so there from mdinit on. Loose ceiling
  // (nextmodelid unknown); the real bound is applied below.
  T2mHit th;
  memset(&th, 0, sizeof(th));
  int faulted = 0;
  const int tables = locate_t2m(base, w, nw, EK_MDL_MODELID_CEILING, &fn, &th,
                                &faulted);
  fn.sweepFaulted += faulted;
  fn.t2mStride = th.stride;
  fn.t2mDefined = th.defined;
  fn.sweepComplete = (!fn.sweepFaulted && !capped) ? 1 : 0;
  if (tables > 1) {
    char why[320];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "%d tile2model table(s) were found in %d window(s) of the host "
                "image's non-executable data and exactly 1 is required -- a "
                "table of %d records whose every modelid is -1 or below %d, "
                "with the hud-pointer step at tile %d",
                tables, nw, EK_MDL_T2M_COUNT, EK_MDL_MODELID_CEILING,
                EK_MDL_MAXTILES);
    refuse(why);
    return false;
  }

  int ids[EK_MDL_MAX_PROBE];
  int nprobe = 0;
  int maxId = -1;
  if (tables == 1) {
    nprobe = t2m_probe_ids(th.base, th.stride, ids, EK_MDL_MAX_PROBE, &maxId);
    fn.probeIds = nprobe;
    fn.probeMax = maxId;
  }
  // Fewer models than the usual quorum is legal, so the bar is the quorum or everything the table names.
  const int need = nprobe < EK_MDL_MIN_AGREE ? nprobe : EK_MDL_MIN_AGREE;

  const int models = tables == 1
                         ? locate_models(base, imageSize, w, nw, ids, nprobe,
                                         need, &fn, &shape, &mh)
                         : 0;
  if (tables != 1 || models == 0) {
    // Absence, not refusal, until the budget runs out (models load while def files parse). Printed on attempt 1,
    // every tenth, and at expiry; the first sample alone was the worst one.
    log_funnel((int)attempt, &fn);
    if (attempt >= EK_MDL_ATTEMPTS) {
      char why[640];
      _snprintf_s(why, sizeof(why), _TRUNCATE,
                  "%d attempt(s) and the looking is over, over %d window(s) of "
                  "the image's non-executable data (%llu bytes, sweep %s). "
                  "tile2model: %d virgin record(s), %d stride(s), %d base(s), "
                  "%d validated table(s) -- stride %d, %d record(s) naming a "
                  "model, largest modelid %d. models: %d slot(s), %d heap-"
                  "shaped, %d leading somewhere at the first named index, %d "
                  "agreeing on a shape (block+%d, %d votes vs %d; frames+%d, "
                  "%d vs %d), %d array(s) validated, longest walk %d entry(s) "
                  "with %d carrying the magic where %d were required",
                  (int)attempt, fn.windows, (unsigned long long)fn.imageBytes,
                  fn.sweepComplete ? "COMPLETE" : "PARTIAL", fn.t2mHoles,
                  fn.t2mStrides, fn.t2mBases, fn.t2mTables, fn.t2mStride,
                  fn.t2mDefined, fn.probeMax, fn.slots, fn.candidates,
                  fn.probed, fn.shaped, fn.headOff, fn.headVotes, fn.headRunner,
                  fn.framesOff, fn.framesVotes, fn.framesRunner, fn.arrays,
                  fn.bestCount, fn.bestMagic, need);
      refuse(why);
    }
    return false;
  }
  if (models > 1) {
    char why[256];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "%d distinct array(s) of model pointers were found and exactly "
                "1 is required; the first is at 0x%p with %d entry(s), named by "
                "%d image slot(s)",
                models, (const void*)mh.array, mh.count, fn.globalHits);
    refuse(why);
    return false;
  }

  // The real bound: now nextmodelid exists, all 32512 records are put to: -1, or an index below the array's count.
  int defined = 0;
  bool bounded = false;
  __try {
    bounded = t2m_validate(th.base, th.stride, mh.count, &defined);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    bounded = false;
  }
  if (!bounded) {
    char why[320];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "the table at rva 0x%07X passed with modelids below %d but not "
                "below %d, the count of the array at 0x%p that was then found "
                "-- so the two do not describe the same model system and "
                "neither can be trusted",
                ek_image_rva(base, th.base), EK_MDL_MODELID_CEILING, mh.count,
                (const void*)mh.array);
    refuse(why);
    return false;
  }
  th.defined = defined;

  s_imageBase = base;
  s_models = mh.array;
  s_modelsSlot = mh.slotRva;
  s_modelCount = mh.count;
  s_headOff = shape.headOff;
  s_framesOff = shape.framesOff;
  s_t2m = th.base;
  s_t2mStride = th.stride;
  InterlockedExchange(&s_state, EK_MDL_STATE_READY);

  ek_log("modelbounds: located on attempt %d. models[] at 0x%p: %d entry(s), %d "
         "of them md3 blocks carrying 'IDP3'. Found by testing %d image slot(s) "
         "at the %d modelid(s) tile2model names (largest %d), which %d array(s) "
         "answered",
         (int)attempt, (const void*)mh.array, mh.count, mh.magic, fn.candidates,
         fn.probeIds, fn.probeMax, fn.arrays);
  // Logged because not in the code: here the shared-data block is 96 bytes and frames sits 112 into head; a layout
  // change shows as a different line. Vote counts show a thin majority versus unanimity.
  ek_log("modelbounds:   head at block+%d (%d of %d probed model(s) agreed, "
         "runner-up %d), md3head_t.frames at head+%d (%d agreed, runner-up %d)",
         shape.headOff, fn.headVotes, fn.probeIds, fn.headRunner,
         shape.framesOff, fn.framesVotes, fn.framesRunner);
  ek_log("modelbounds:   the models global is at rva 0x%07X -- the slot the "
         "array was found in, and %d image slot(s) hold that address; a reload "
         "that moves the array will be noticed",
         mh.slotRva, fn.globalHits);
  ek_log("modelbounds: tile2model[] at rva 0x%07X, stride %d bytes derived from "
         "the table's own repetition, %d records, %d of them naming a model",
         ek_image_rva(base, th.base), th.stride, EK_MDL_T2M_COUNT, th.defined);

  {
    const unsigned char* blk = NULL;
    if (read_ptr(s_models, &blk) && entry_is_md3(blk, s_headOff)) {
      char nam[80];
      head_name(blk + s_headOff, nam, sizeof(nam));
      const unsigned char* f = NULL;
      float v[6] = { 0, 0, 0, 0, 0, 0 };
      float scale = 0.0f, bscale = 0.0f, zadd = 0.0f, yoffset = 0.0f;
      int numframes = 0;
      read_i32(blk + s_headOff + EK_MDL_HD_NUMFRAMES, &numframes);
      read_f32(blk + EK_MDL_SM_SCALE, &scale);
      read_f32(blk + EK_MDL_SM_BSCALE, &bscale);
      read_f32(blk + EK_MDL_SM_ZADD, &zadd);
      read_f32(blk + EK_MDL_SM_YOFFSET, &yoffset);
      if (read_ptr(blk + s_headOff + s_framesOff, &f)) safe_read(f, v, sizeof(v));
      ek_log("modelbounds:   models[0] \"%s\": %d frame(s), scale %.6f bscale "
             "%.6f zadd %.3f yoffset %.3f, frame 0 raw min (%.2f %.2f %.2f) max "
             "(%.2f %.2f %.2f)",
             nam, numframes, (double)scale, (double)bscale, (double)zadd,
             (double)yoffset,
             (double)v[0], (double)v[1], (double)v[2],
             (double)v[3], (double)v[4], (double)v[5]);
      const EkModelBox sample =
          ekm_mdl_scale_bbox(v[0], v[1], v[2], v[3], v[4], v[5],
                             scale, bscale, zadd, yoffset);
      ek_log("modelbounds:   models[0] in build units: x %.2f..%.2f y "
             "%.2f..%.2f z %.2f..%.2f (ok %d), sprite repeats excluded",
             sample.minx, sample.maxx, sample.miny, sample.maxy,
             sample.minz, sample.maxz, sample.ok);
    }
  }
  return true;
}

// Ptile2tile, mdsprite.cpp:61-71, hop-capped: the engine reassigns the tile then tests the NEW tile's pal. `pal` is a
// signed plain char, so palettes above 127 never match; mirrored on purpose.
static int t2m_ptile2tile(int tile, int pal) {
  int t = tile;
  int cur = tile;
  for (int hop = 0; hop < EK_MDL_PAL_HOPS; hop++) {
    int next = 0;
    if (!t2m_read_i16(s_t2m + (size_t)cur * s_t2mStride, EK_MDL_T2M_NEXTTILE,
                      &next))
      break;
    if (next == -1) break;
    if (next < 0 || next >= EK_MDL_T2M_COUNT) break;
    cur = next;
    signed char p = 0;
    const unsigned char* const at =
        s_t2m + (size_t)cur * s_t2mStride + EK_MDL_T2M_PAL;
    if (!gamestate_region_contains(at, sizeof(p))) break;
    if (!safe_read(at, &p, sizeof(p))) break;
    if ((int)p == pal) { t = cur; break; }
  }
  return t;
}

int modelbounds_for(int picnum, int pal, EkModelBox* out) {
  EkModelBox empty;
  memset(&empty, 0, sizeof(empty));
  *out = empty;

  if (!modelbounds_ready()) return EK_MDL_UNREADY;
  if (picnum < 0 || picnum >= EK_MDL_MAXTILES) return EK_MDL_NONE;

  // Re-read from its slot every call: freeallmodels (mdsprite.cpp:120-131) frees and nulls it on a def reload. A zero
  // slot means no image global was found (logged) and the check is skipped.
  const unsigned char* arr = s_models;
  if (s_modelsSlot && !read_ptr(s_imageBase + s_modelsSlot, &arr))
    return EK_MDL_UNREADY;
  if (arr != s_models) {
    // The gate shuts until the scan thread looks again; the render thread only resets the state to LOOKING and
    // refunds the budget, and the sampler re-locates within a second.
    if (InterlockedExchange(&s_movedLogged, 1) == 0)
      ek_log("modelbounds: the models array moved -- the slot at rva 0x%07X no "
             "longer holds 0x%p, so the models were freed and reloaded. No "
             "bounds are answered until the locate has run again", s_modelsSlot,
             (const void*)s_models);
    InterlockedExchange(&s_attempts, 0);
    InterlockedExchange(&s_state, EK_MDL_STATE_LOOKING);
    return EK_MDL_UNREADY;
  }

  const int tile = t2m_ptile2tile(picnum, pal);
  int modelid = 0, framenum = 0;
  if (!t2m_read_i16(s_t2m + (size_t)tile * s_t2mStride, EK_MDL_T2M_MODELID,
                    &modelid))
    return EK_MDL_NONE;
  if (modelid < 0 || modelid >= s_modelCount) return EK_MDL_NONE;
  if (!t2m_read_i16(s_t2m + (size_t)tile * s_t2mStride, EK_MDL_T2M_FRAMENUM,
                    &framenum))
    return EK_MDL_NONE;
  if (framenum < 0) return EK_MDL_NONE;

  const unsigned char* blk = NULL;
  if (!read_ptr(arr + (size_t)modelid * sizeof(void*), &blk)) return EK_MDL_NONE;
  if (blk == NULL) return EK_MDL_NONE;
  if (!entry_is_md3(blk, s_headOff)) return EK_MDL_NONE;

  int numframes = 0;
  if (!read_i32(blk + s_headOff + EK_MDL_HD_NUMFRAMES, &numframes))
    return EK_MDL_NONE;
  if (framenum >= numframes) return EK_MDL_NONE;

  const unsigned char* frames = NULL;
  if (!read_ptr(blk + s_headOff + s_framesOff, &frames)) return EK_MDL_NONE;
  float v[6];
  if (!safe_read(frames + (size_t)framenum * EK_MDL_FRAME_STRIDE, v, sizeof(v)))
    return EK_MDL_NONE;

  float scale = 0.0f, bscale = 0.0f, zadd = 0.0f, yoffset = 0.0f;
  if (!read_f32(blk + EK_MDL_SM_SCALE, &scale)) return EK_MDL_NONE;
  if (!read_f32(blk + EK_MDL_SM_BSCALE, &bscale)) return EK_MDL_NONE;
  if (!read_f32(blk + EK_MDL_SM_ZADD, &zadd)) return EK_MDL_NONE;
  if (!read_f32(blk + EK_MDL_SM_YOFFSET, &yoffset)) return EK_MDL_NONE;

  const EkModelBox b = ekm_mdl_scale_bbox(v[0], v[1], v[2], v[3], v[4], v[5],
                                          scale, bscale, zadd, yoffset);
  if (!b.ok) return EK_MDL_NONE;
  *out = b;
  return EK_MDL_OK;
}
