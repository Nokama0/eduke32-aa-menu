#pragma once

#include <stdio.h>

// Names only live state can give. RAILGUNSPRITE picks one of three `userquote`s from `pchar` and its upgrade bitfield
// (AAITEMS.CON:3809); SMALLSTACK/BIGSTACK are money, read from the palette by `state coinsget` (NOTENEMY.CON:15078). A
// failed read names nothing, never a default: a wrong name is worse than none.

const int EKM_PIC_RAILGUNSPRITE = 5329;
const int EKM_PIC_SMALLSTACK    = 12247;
const int EKM_PIC_BIGSTACK      = 12248;

// `ifvarand <char>upgrades[DEVISTATOR_WEAPON] 4`, DEVISTATOR_WEAPON = 7 (AADEFS.CON:3143).
const int EKM_AA_UPG_SLOT_DEVISTATOR = 7;
const int EKM_AA_UPG_BIT_PENETRATOR  = 4;

// `ifge pchar 3` covers 3 and 4 only: AADEFS.CON:3267 declares five characters (duke, bombshell, wes, mando, deanova).
const int EKM_AA_PCHAR_MANDO = 3;
const int EKM_AA_PCHAR_MAX   = 4;

// Four separate parameters (gotPchar/pchar, gotUpg/upgBits): answers that travel together get their order lost.
inline const char* ekm_item_name_aa_runtime(int picnum,
                                            bool gotPchar, int pchar,
                                            bool gotUpg, int upgBits) {
  if (picnum != EKM_PIC_RAILGUNSPRITE) return 0;
  if (!gotPchar) return 0;
  if (pchar < 0 || pchar > EKM_AA_PCHAR_MAX) return 0;
  if (pchar >= EKM_AA_PCHAR_MANDO) return "Disintegrator";
  if (!gotUpg) return 0;
  return (upgBits & EKM_AA_UPG_BIT_PENETRATOR) ? "Penetrator"
                                               : "Railgun";
}

// `state coinsget`'s palette chain (NOTENEMY.CON:15080) in the CON's own order, not ascending; pals are compared for
// equality, so only the final default arm must stay last.
const int EKM_CASH_PAL_TINY    = 8,  EKM_CASH_CREDITS_TINY    = 25;
const int EKM_CASH_PAL_SMALL   = 1,  EKM_CASH_CREDITS_SMALL   = 125;
const int EKM_CASH_PAL_MEDIUM  = 23, EKM_CASH_CREDITS_MEDIUM  = 250;
const int EKM_CASH_PAL_LARGE   = 10, EKM_CASH_CREDITS_LARGE   = 500;
const int EKM_CASH_PAL_HUGE    = 49, EKM_CASH_CREDITS_HUGE    = 1250;
const int EKM_CASH_PAL_BIG     = 99, EKM_CASH_CREDITS_BIG     = 750;

const int EKM_CASH_CREDITS_DEFAULT = 125;

// `ifactor BIGSTACK mul B 3` runs after the chain, so it reaches the default too.
const int EKM_CASH_BIGSTACK_MULTIPLE = 3;

inline int ekm_cash_credits(int picnum, int pal) {
  if (picnum != EKM_PIC_SMALLSTACK && picnum != EKM_PIC_BIGSTACK) return 0;

  int credits = EKM_CASH_CREDITS_DEFAULT;
  if      (pal == EKM_CASH_PAL_TINY)   credits = EKM_CASH_CREDITS_TINY;
  else if (pal == EKM_CASH_PAL_SMALL)  credits = EKM_CASH_CREDITS_SMALL;
  else if (pal == EKM_CASH_PAL_MEDIUM) credits = EKM_CASH_CREDITS_MEDIUM;
  else if (pal == EKM_CASH_PAL_LARGE)  credits = EKM_CASH_CREDITS_LARGE;
  else if (pal == EKM_CASH_PAL_HUGE)   credits = EKM_CASH_CREDITS_HUGE;
  else if (pal == EKM_CASH_PAL_BIG)    credits = EKM_CASH_CREDITS_BIG;

  return (picnum == EKM_PIC_BIGSTACK) ? credits * EKM_CASH_BIGSTACK_MULTIPLE
                                      : credits;
}

// Shared by esp_collect_items and esp_draw_item_marker so box and label cannot disagree. Order: live name, money, then
// the caller's own, returned untouched for pickups this file does not know. The buffer is the caller's stack, never
// static.
inline const char* ekm_item_label_aa(int picnum, int pal, const char* base,
                                     bool gotPchar, int pchar,
                                     bool gotUpg, int upgBits,
                                     char* buf, int bufLen) {
  const char* const live =
      ekm_item_name_aa_runtime(picnum, gotPchar, pchar, gotUpg, upgBits);
  if (live) return live;

  const int credits = ekm_cash_credits(picnum, pal);
  if (credits <= 0 || buf == 0 || bufLen <= 0) return base;

  // _TRUNCATE, never sprintf_s, which is fatal on a short buffer.
  _snprintf_s(buf, (size_t)bufLen, _TRUNCATE, "Cash (%d)", credits);
  return buf;
}
