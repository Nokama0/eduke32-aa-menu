#pragma once

// EDuke32's OSD cvar registry, read by shape. osdcvardata_t (osd.h, r10669): +0x00 name, +0x08 desc, +0x10 ptr, +0x18
// flags, +0x1C min, +0x20 max; size 0x28.
const int EK_CVAR_STRIDE   = 0x28;
const int EK_CVAR_NAME_MAX = 64;

const int EK_CVAR_MAX = 1024;

const unsigned EK_CVAR_FLOAT  = 0x01;
const unsigned EK_CVAR_INT    = 0x02;
const unsigned EK_CVAR_UINT   = 0x04;
const unsigned EK_CVAR_BOOL   = 0x08;
const unsigned EK_CVAR_STRING = 0x10;
const unsigned EK_CVAR_DOUBLE = 0x20;

// Variables outside the image: the console's own cvars point into the heap osdmain_t (osd.cpp:785).
const int EK_CVAR_EXTERN_MAX = 64;

struct EkCvar {
  const char* name;
  const char* desc;
  void* ptr;
  unsigned flags;
  int vmin, vmax;
  const void* rec;
};

bool cvars_scan(void);

int cvars_count(void);
const EkCvar* cvars_at(int i);

// NULL if absent or ambiguous: writing through a guessed pointer is worse than no answer.
const EkCvar* cvars_find(const char* name);

const EkCvar* cvars_find_extern(const char* name);

bool cvars_is_record(const void* rec);

bool cvar_get_int(const char* name, int* out);


// Masks the low six bits: cl_weaponswitch also carries CVAR_MULTI (0x400).
inline bool cvar_type_writable(unsigned flags) {
  const unsigned type = flags & 0x3F;
  return type == EK_CVAR_INT || type == EK_CVAR_UINT || type == EK_CVAR_BOOL;
}

// The record is the authority on its range: the engine registered it with the vmin/vmax its OSD clamps to
// (osdcmds.cpp).
inline int cvar_clamped(const EkCvar& c, int v) {
  if (v < c.vmin) return c.vmin;
  if (v > c.vmax) return c.vmax;
  return v;
}

// NULL covers absent and ambiguous alike.
inline bool cvar_row_available(const EkCvar* c) {
  return c != 0 && cvar_type_writable(c->flags);
}

// Clamped to the registry's vmin/vmax; *applied is what landed. Writes engine memory, but the engine's own OSD writes
// the same variables through this record (`cl_autosave 1`, osd.cpp:2282-2325).
bool cvar_set_int(const char* name, int value, int* applied = 0);
