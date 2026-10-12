#pragma once

// Switch picnums, frames and marker text; taxonomy follows the engine's switch macros plus MULTISWITCH.
// Base picnum names: docs/re/picnums.md.
#include <string.h>

const char* const EKM_SWITCH_LABEL = "Switch";
const char* const EKM_DOOR_LABEL   = "Usable";

enum { EKM_PIC_NUKEBUTTON = 142 };

enum EkSwitchFamily {
  EK_SW_NONE = 0,
  EK_SW_ACCESS,
  EK_SW_TWOSTATE,
  EK_SW_MULTI,
  EK_SW_COMBINATION,
  EK_SW_DOOR,
  EK_SW_EXIT
};

enum { EKM_EXIT_NONE = 0, EKM_EXIT_NORMAL, EKM_EXIT_SECRET };

// `family`/`exitKind` are int, not enums: MSVC 19.5x /O2 loses narrow members of a struct returned by value.
struct EkSwitchInfo {
  int family, base, exitKind;
  const char* line;
};

inline int ekm_isadoorwall(int picnum) {
  return (picnum >= 150 && picnum <= 159) ||
         picnum == 395 ||
         (picnum >= 447 && picnum <= 449) ||
         picnum == 717 ||
         picnum == 781 ||
         picnum == 1102 ||
         picnum == 1144 ||
         picnum == 1169 ||
         picnum == 1178 || picnum == 1179 ||
         picnum == 4391;
}

inline int ekm_switch_base_for(int picnum) {
  // MULTISWITCH is the one four-frame family: 146..149 map to 146.
  if (picnum >= 146 && picnum <= 149) return 146;

  static const int kBases[18] = {
    130,
    132,
    134,
    136,
    138,
    140,
    162,
    164,
    166,
    168,
    170,
    712,
    860,
    862,
    864,
    1111,
    1122,
    1142
  };
  // Diverges from G_GetBaseSwitch, which omits ACCESSSWITCH/2 from its `+ 1` list: this asks "which family".
  for (int i = 0; i < 18; i++)
    if (picnum == kBases[i] || picnum == kBases[i] + 1) return kBases[i];

  if (ekm_isadoorwall(picnum)) return picnum;

  return -1;
}

// The palette alone decides exit kind. World Tour also demands lotag 65535/32767 for a normal exit; do not port that.
inline int ekm_switch_exit_kind(int picnum, int pal) {
  if (picnum != EKM_PIC_NUKEBUTTON) return EKM_EXIT_NONE;
  return pal ? EKM_EXIT_SECRET : EKM_EXIT_NORMAL;
}

inline EkSwitchInfo ekm_switch_classify(int picnum, int pal, int hitag, int lotag, int gotAccess) {
  EkSwitchInfo info;
  info.family   = EK_SW_NONE;
  info.base     = -1;
  info.exitKind = EKM_EXIT_NONE;
  info.line     = 0;

  int exitKind = ekm_switch_exit_kind(picnum, pal);
  if (exitKind != EKM_EXIT_NONE) {
    info.family   = EK_SW_EXIT;
    info.base     = picnum;
    info.exitKind = exitKind;
    info.line     = (exitKind == EKM_EXIT_SECRET) ? "Secret exit" : "Exit";
    return info;
  }

  int base = ekm_switch_base_for(picnum);
  if (base < 0) return info;

  if (lotag == 0) return info;

  info.base = base;

  int pressed = (picnum != base) ? 1 : 0;

  // ACCESS: pals {0,21,23} = blue, red, yellow test got_access bits 1,2,4.
  if (base == 130 || base == 170) {
    info.family = EK_SW_ACCESS;
    if (!pressed) {
      if (pal == 0)       { if (!(gotAccess & 1)) info.line = "Needs blue card"; }
      else if (pal == 21) { if (!(gotAccess & 2)) info.line = "Needs red card"; }
      else if (pal == 23) { if (!(gotAccess & 4)) info.line = "Needs yellow card"; }
    }
    return info;
  }

  if (base == 162 || base == 166 || base == 1142) {
    info.family = EK_SW_COMBINATION;
    info.line   = (pressed == (hitag != 0 ? 1 : 0)) ? "Leave" : "Flip";
    return info;
  }

  // Door before the two-state default, or a door tile would be labelled a switch.
  if (ekm_isadoorwall(picnum)) {
    info.family = EK_SW_DOOR;
    return info;
  }

  if (base == 146) {
    info.family = EK_SW_MULTI;
    return info;
  }

  info.family = EK_SW_TWOSTATE;
  return info;
}
