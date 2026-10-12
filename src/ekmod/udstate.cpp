// Deriving &ud from the cvar registry: cvars register pointers to the real variables, several of them inside
// `user_defs ud` (osdcmds registers "fov" as (void*)&ud.fov), so address minus known offset is the struct.

#include <windows.h>
#include "log.h"
#include "cvars.h"
#include "udstate.h"

static const unsigned char* s_ud = NULL;
static bool s_resolved = false;

const unsigned char* ud_base(void) {
  if (s_resolved) return s_ud;
  s_resolved = true;

  const EkCvar* fov = cvars_find("fov");
  const EkCvar* aim = cvars_find("cl_autoaim");
  if (!fov || !aim) {
    ek_log("ud: cvar(s) missing (fov=%p cl_autoaim=%p); camera unavailable",
           (void*)fov, (void*)aim);
    return NULL;
  }

  const unsigned char* fromFov = (const unsigned char*)fov->ptr - EK_UD_FOV;
  const unsigned char* fromAim = (const unsigned char*)aim->ptr - EK_UD_CONFIG_AUTOAIM;

  if (fromFov != fromAim) {
    // A refusal, not a warning: disagreeing cvars mean user_defs changed shape, so every offset in udstate.h is
    // wrong.
    ek_log("ud: LAYOUT CHANGED -- fov implies %p, cl_autoaim implies %p "
           "(delta %lld). user_defs has moved; refusing to guess.",
           (const void*)fromFov, (const void*)fromAim,
           (long long)(fromAim - fromFov));
    return NULL;
  }

  s_ud = fromFov;
  ek_log("ud: &ud = %p (rva 0x%07X), agreed by fov and cl_autoaim",
         (const void*)s_ud,
         (unsigned)(s_ud - (const unsigned char*)GetModuleHandleA(NULL)));
  return s_ud;
}

bool ud_camera(EkCamera* out) {
  if (!out) return false;
  const unsigned char* ud = ud_base();
  if (!ud) return false;

  out->x = *(const int*)(ud + EK_UD_CAMERAPOS + 0);
  out->y = *(const int*)(ud + EK_UD_CAMERAPOS + 4);
  out->z = *(const int*)(ud + EK_UD_CAMERAPOS + 8);
  out->angFix   = *(const int*)(ud + EK_UD_CAMERAQ16ANG);
  out->horizFix = *(const int*)(ud + EK_UD_CAMERAQ16HORIZ);
  out->sect     = *(const short*)(ud + EK_UD_CAMERASECT);
  out->fov      = *(const int*)(ud + EK_UD_FOV);

  // fix16_t is 16.16; masked because the engine lets the raw value run outside 0..2047 and wraps on use.
  out->ang = (out->angFix >> 16) & (EK_BUILD_ANGLE_UNITS - 1);
  return true;
}

// Guarded: the derived base is very likely right, and a write through such a pointer is what a guard is for. All six
// fields or none (a partial substitution would draw the camera's position with the player's angle).
bool ud_camera_write(int x, int y, int z, int angFix, int horizFix, int sect) {
  unsigned char* const ud = (unsigned char*)ud_base();
  if (!ud) return false;
  __try {
    *(int*)(ud + EK_UD_CAMERAPOS + 0)   = x;
    *(int*)(ud + EK_UD_CAMERAPOS + 4)   = y;
    *(int*)(ud + EK_UD_CAMERAPOS + 8)   = z;
    *(int*)(ud + EK_UD_CAMERAQ16ANG)    = angFix;
    *(int*)(ud + EK_UD_CAMERAQ16HORIZ)  = horizFix;
    *(short*)(ud + EK_UD_CAMERASECT)    = (short)sect;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// The bracket's read is guarded because it runs inside a five-byte code patch in the renderer's call path, unlike
// project.cpp's read in the SwapBuffers hook.
bool ud_camera_guarded(EkCamera* out) {
  if (!out) return false;
  __try {
    return ud_camera(out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ud_paused(bool* out) {
  if (!out) return false;
  const unsigned char* const ud = ud_base();
  if (!ud) return false;
  __try {
    *out = *(const short*)(ud + EK_UD_PAUSE_ON) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool ud_level_identity(int* volume, int* level) {
  if (!volume || !level) return false;
  const unsigned char* const ud = ud_base();
  if (!ud) return false;
  int v = -1, l = -1;
  __try {
    v = *(const int*)(ud + EK_UD_VOLUME_NUMBER);
    l = *(const int*)(ud + EK_UD_LEVEL_NUMBER);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (!ekm_ud_level_plausible(v, l)) return false;
  *volume = v;
  *level = l;
  return true;
}

// Plausibility is the only cross-check here (nothing anchors a field 1348 bytes in): overhead_on is only 0, 1 or 2
// and show_help only 0 or 1, otherwise the layout moved and the caller gets no answer.
bool ud_map_or_help(bool* out) {
  if (!out) return false;
  const unsigned char* const ud = ud_base();
  if (!ud) return false;
  int overhead = 0, help = 0;
  __try {
    overhead = *(const char*)(ud + EK_UD_OVERHEAD_ON);
    help     = *(const char*)(ud + EK_UD_SHOW_HELP);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (overhead < 0 || overhead > 2 || help < 0 || help > 1) return false;
  // Only overhead_on == 2 suppresses the draw; == 1 is the small corner map, which draws the world underneath it.
  *out = (overhead == 2) || (help != 0);
  return true;
}
