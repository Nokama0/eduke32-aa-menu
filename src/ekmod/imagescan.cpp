// The image-scan toolkit.

#include "imagescan.h"
// For ekm_us_rel32_target: the displacement rule belongs to one inline
// function, not to each walk that decodes a five-byte rel32.
#include "hitscan.h"

// The host image's sections, filtered by what we want to search; guarded: VirtualSize can run past what is committed.
bool ek_image_sections(const unsigned char* base, bool wantExec,
                       EkSpan* out, int max, int* count) {
  *count = 0;
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;
  for (int i = 0; i < nsec && *count < max; i++) {
    const bool exec = (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    if (exec != wantExec) continue;
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
    DWORD span = sec[i].Misc.VirtualSize;
    // The raw size is the part that is certainly backed by the file; .bss-like
    // sections have a VirtualSize far past it and hold nothing we can match.
    if (sec[i].SizeOfRawData == 0) continue;
    if (span > sec[i].SizeOfRawData) span = sec[i].SizeOfRawData;
    if (span == 0) continue;
    out[*count].base = base + sec[i].VirtualAddress;
    out[*count].size = span;
    (*count)++;
  }
  return true;
}

// Which section an ADDRESS is in. VirtualSize is the committed extent (SizeOfRawData is 0 for .bss); headers only.
bool ek_image_data_extent(const unsigned char* base, unsigned rva,
                          unsigned* begin, unsigned* end) {
  *begin = 0;
  *end = 0;
  __try {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const int nsec = (int)nt->FileHeader.NumberOfSections;
    for (int i = 0; i < nsec; i++) {
      if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
      // An object file can leave VirtualSize zero and mean SizeOfRawData; a
      // section with neither is not somewhere an address can be.
      DWORD span = sec[i].Misc.VirtualSize;
      if (span == 0) span = sec[i].SizeOfRawData;
      if (span == 0) continue;
      const unsigned b = sec[i].VirtualAddress;
      if (rva < b || rva - b >= span) continue;
      *begin = b;
      *end = b + span;
      return true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *begin = 0;
    *end = 0;
    return false;
  }
  return false;
}

void ek_image_find_string(const unsigned char* base, const char* s, size_t len,
                          EkStrHit* out) {
  out->rva = 0; out->count = 0; out->faulted = 0;
  // A null or empty anchor is a fault, not a no-match.
  if (!s || len == 0) { out->faulted = 1; return; }
  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, false, spans, 16, &n)) { out->faulted = 1; return; }

  for (int i = 0; i < n; i++) {
    if (spans[i].size < len) continue;
    __try {
      const DWORD last = spans[i].size - (DWORD)len;
      for (DWORD off = 0; off <= last; off++) {
        if (spans[i].base[off] != (unsigned char)s[0]) continue;
        size_t k = 1;
        for (; k < len; k++)
          if (spans[i].base[off + k] != (unsigned char)s[k]) break;
        if (k != len) continue;
        if (out->count == 0) out->rva = ek_image_rva(base, spans[i].base + off);
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A SECTION THAT FAULTED PART-WAY WAS NOT SEARCHED, and the partial count must not be reported as if it were: the
      // bytes never looked at could hold the second occurrence that would make this ambiguous.
      out->faulted = 1;
    }
  }
}

// Instructions loading the anchor's address: `REX.W 8D /r` (lea r64,[rip+disp32]). Requiring the lea shape keeps a bare
// 4-byte match off data.
void ek_image_find_lea_refs(const unsigned char* base, unsigned targetRva,
                            EkRefHit* out) {
  out->rva = 0; out->count = 0; out->raw = 0; out->faulted = 0;
  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, true, spans, 16, &n)) { out->faulted = 1; return; }

  for (int i = 0; i < n; i++) {
    if (spans[i].size < 8) continue;
    __try {
      const DWORD last = spans[i].size - 4;
      for (DWORD off = 3; off <= last; off++) {
        const unsigned dispRva = ek_image_rva(base, spans[i].base + off);
        // The displacement this position would have to hold to reach the
        // anchor. Computed per position rather than searched for, so the
        // comparison is four bytes and nothing is decoded.
        const long long want = (long long)targetRva - ((long long)dispRva + 4);
        if (want < -2147483648LL || want > 2147483647LL) continue;
        const int d = (int)want;
        if (*(const int*)(spans[i].base + off) != d) continue;
        out->raw++;
        const unsigned char r = spans[i].base[off - 3];
        const unsigned char op = spans[i].base[off - 2];
        const unsigned char modrm = spans[i].base[off - 1];
        if ((r != 0x48 && r != 0x4C) || op != 0x8D) continue;
        if ((modrm & 0xC7) != 0x05) continue;
        if (out->count == 0) out->rva = dispRva - 3;   // the lea's own address
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out->faulted = 1;
    }
  }
}

bool ek_image_pdata(const unsigned char* base,
                    const IMAGE_RUNTIME_FUNCTION_ENTRY** fn, DWORD* n) {
  *fn = NULL; *n = 0;
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  const IMAGE_DATA_DIRECTORY* dir =
      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (dir->VirtualAddress == 0 || dir->Size < sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY))
    return false;
  *fn = (const IMAGE_RUNTIME_FUNCTION_ENTRY*)(base + dir->VirtualAddress);
  *n = dir->Size / (DWORD)sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
  return true;
}

// ZERO IS THREE ANSWERS IN ONE: no usable exception directory, no entry covering `rva`, and a walk that faulted. A
// caller's refusal message may not claim to know which, and camera.cpp's says so.
bool ek_image_extent_containing(const unsigned char* base, unsigned rva,
                                unsigned* begin, unsigned* end) {
  if (begin) *begin = 0;
  if (end) *end = 0;
  const IMAGE_RUNTIME_FUNCTION_ENTRY* fn = NULL;
  DWORD n = 0;
  if (!ek_image_pdata(base, &fn, &n)) return false;

  bool found = false;
  __try {
    for (DWORD i = 0; i < n; i++) {
      if (fn[i].EndAddress <= fn[i].BeginAddress) continue;
      if (rva < fn[i].BeginAddress || rva >= fn[i].EndAddress) continue;
      if (begin) *begin = fn[i].BeginAddress;
      if (end) *end = fn[i].EndAddress;
      found = true;
      break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // A faulted walk leaves no half-answer: the loop may have written one of the pair already.
    if (begin) *begin = 0;
    if (end) *end = 0;
    found = false;
  }
  return found;
}

unsigned ek_image_entry_containing(const unsigned char* base, unsigned rva) {
  unsigned begin = 0, end = 0;
  return ek_image_extent_containing(base, rva, &begin, &end) ? begin : 0;
}

void ek_image_count_calls(const unsigned char* base, unsigned entry,
                          EkCallCount* out) {
  out->sites = 0; out->owners = 0; out->capped = 0; out->faulted = 0;
  for (int k = 0; k < EK_IMAGE_MAX_OWNERS; k++) out->owner[k] = 0;
  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, true, spans, 16, &n)) { out->faulted = 1; return; }

  unsigned* const owners = out->owner;
  for (int i = 0; i < n; i++) {
    if (spans[i].size < 5) continue;
    __try {
      const DWORD last = spans[i].size - 5;
      for (DWORD off = 0; off <= last; off++) {
        if (spans[i].base[off] != 0xE8) continue;
        const unsigned site = ek_image_rva(base, spans[i].base + off);
        const int disp = *(const int*)(spans[i].base + off + 1);
        if (ekm_us_rel32_target(site, disp) != entry) continue;
        out->sites++;

        const unsigned own = ek_image_entry_containing(base, site);
        if (own == 0) continue;
        int seen = 0;
        for (int k = 0; k < out->owners; k++)
          if (owners[k] == own) { seen = 1; break; }
        if (seen) continue;
        if (out->owners >= EK_IMAGE_MAX_OWNERS) { out->capped = 1; continue; }
        owners[out->owners++] = own;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out->faulted = 1;
    }
  }
}
