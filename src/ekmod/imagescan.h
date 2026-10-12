#pragma once

// Find a function in the stripped host by its source text: an anchor string, the one rip-relative `lea` reaching it,
// and the RUNTIME_FUNCTION containing it. Scans fault rather than crash and never report a partial count as complete:
// refuse on `faulted`. DLL only.

#include <windows.h>

struct EkSpan { const unsigned char* base; DWORD size; };

bool ek_image_sections(const unsigned char* base, bool wantExec,
                       EkSpan* out, int max, int* count);

bool ek_image_data_extent(const unsigned char* base, unsigned rva,
                          unsigned* begin, unsigned* end);

inline unsigned ek_image_rva(const unsigned char* base, const unsigned char* p) {
  return (unsigned)(size_t)(p - base);
}

// More than one occurrence means the anchor identifies nothing.
struct EkStrHit { unsigned rva; int count; int faulted; };
void ek_image_find_string(const unsigned char* base, const char* s, size_t len,
                          EkStrHit* out);

struct EkRefHit { unsigned rva; int count; int raw; int faulted; };
void ek_image_find_lea_refs(const unsigned char* base, unsigned targetRva,
                            EkRefHit* out);

bool ek_image_pdata(const unsigned char* base,
                    const IMAGE_RUNTIME_FUNCTION_ENTRY** fn, DWORD* n);

bool ek_image_extent_containing(const unsigned char* base, unsigned rva,
                                unsigned* begin, unsigned* end);

unsigned ek_image_entry_containing(const unsigned char* base, unsigned rva);

const int EK_IMAGE_MAX_OWNERS = 32;

struct EkCallCount {
  int sites; int owners; int capped; int faulted;
  unsigned owner[EK_IMAGE_MAX_OWNERS];
};

void ek_image_count_calls(const unsigned char* base, unsigned entry,
                          EkCallCount* out);
