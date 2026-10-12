#pragma once
#include "cvars.h"

// Is EDuke32's console taking typing? Found without an offset: osd->flags & OSD_CAPTURE via the osdtextmode and
// osdhistorydepth cvar records, which point into the heap osdmain_t (osd.cpp:843, :846).

const unsigned EK_OSD_CAPTURE = 0x4;

// r10669 has cvars at +0x30 past draw.mode.
const int EK_OSD_SCAN_BYTES = 512;

const int EK_OSD_SAMPLE_MAX = 64;

const unsigned long long EK_OSD_ANCHOR_SPAN_MAX = 256;

inline int ekm_osd_sample(int n) {
  if (n <= 0) return 0;
  return n < EK_OSD_SAMPLE_MAX ? n : EK_OSD_SAMPLE_MAX;
}

inline bool ekm_osd_anchor_pair_ok(unsigned long long histMaxlines,
                                   unsigned long long drawMode) {
  if ((histMaxlines & 3) != 0 || (drawMode & 3) != 0) return false;
  return drawMode > histMaxlines && drawMode - histMaxlines <= EK_OSD_ANCHOR_SPAN_MAX;
}

// Every sampled entry must be a known record and both anchors among them.
inline bool ekm_osd_cvars_candidate(unsigned long long slot, int n, unsigned long long p,
                                    const unsigned long long* pData, const bool* known,
                                    int k, unsigned long long anchorA,
                                    unsigned long long anchorB) {
  if ((slot & 7) != 0 || p == 0 || (p & 7) != 0) return false;
  if (n < 2 || n > EK_CVAR_MAX) return false;
  if (!pData || !known || k != ekm_osd_sample(n)) return false;
  if (anchorA == 0 || anchorB == 0 || anchorA == anchorB) return false;
  bool sawA = false, sawB = false;
  for (int i = 0; i < k; i++) {
    if (pData[i] == 0 || (pData[i] & 7) != 0 || !known[i]) return false;
    if (pData[i] == anchorA) sawA = true;
    if (pData[i] == anchorB) sawB = true;
  }
  return sawA && sawB;
}

// osdmain_t image: flags osd+0x8C8, numcvars +0x8CC, cvars +0x8D0.
inline unsigned long long ekm_osd_count_addr(unsigned long long slot) { return slot - 4; }
inline unsigned long long ekm_osd_flags_addr(unsigned long long slot) { return slot - 8; }

inline bool ekm_osd_capturing(unsigned flags) { return (flags & EK_OSD_CAPTURE) != 0; }

// Render thread, once; a refusal leaves console_capturing() false.
void console_locate(void);

bool console_capturing(void);
