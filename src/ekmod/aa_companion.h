#pragma once

// AA's companion bots, every pose tile (AADEFS.CON), each `useractor enemy` (DUKEBOT, BOMBSHELL, WESBOT, DEABOT.CON).
const int EK_PICNUM_DUKEBOT       = 7470;
const int EK_PICNUM_DUKEBOTCROUCH = 7525;
const int EK_PICNUM_SHELLY        = 6656;
const int EK_PICNUM_SHELLYCROUCH  = 6681;
const int EK_PICNUM_WESBOT        = 19481;
const int EK_PICNUM_WESBOTCROUCH  = 19521;
const int EK_PICNUM_DEABOT        = 22016;
const int EK_PICNUM_DEABOTCROUCH  = 22171;

const int EK_AA_MONSTFLAG_MIMIC = 131072;

// Mando is not here: a pal-15 MANDOFETT is a hostile rival with no mimic bit (own rule below).
inline bool ekm_picnum_is_aa_companion_bot(int picnum) {
  return picnum == EK_PICNUM_DUKEBOT || picnum == EK_PICNUM_DUKEBOTCROUCH ||
         picnum == EK_PICNUM_SHELLY || picnum == EK_PICNUM_SHELLYCROUCH ||
         picnum == EK_PICNUM_WESBOT || picnum == EK_PICNUM_WESBOTCROUCH ||
         picnum == EK_PICNUM_DEABOT || picnum == EK_PICNUM_DEABOTCROUCH;
}

// A companion waiting to be recruited is still a companion: team 0, never attacking, until `ifcansee ifpdistl 4096`
// sets team 1; only the mimic stays hostile.
inline bool ekm_aa_companion_bot(int picnum, bool gotMonstflags, int monstflags,
                                 bool gotTeam, int team, bool gotMonstatus,
                                 int monstatus) {
  if (!ekm_picnum_is_aa_companion_bot(picnum) || !gotMonstflags) return false;
  if ((monstflags & EK_AA_MONSTFLAG_MIMIC) != 0) return false;
  if (!gotTeam || !gotMonstatus || team == -1) return false;
  return !(team == 0 && monstatus == 1);
}

const int EK_PICNUM_MANDOFETT   = 14085;
const int EK_PICNUM_MANDOCROUCH = 14160;
const int EK_AA_INITFLAGS_MANNEQUIN = 32766;

inline bool ekm_picnum_is_aa_mando(int picnum) {
  return picnum == EK_PICNUM_MANDOFETT || picnum == EK_PICNUM_MANDOCROUCH;
}

// The character-select Mando, not the rival: its map hitag becomes initflags; at >= 32766 it breaks at spawn and
// idles in charidlecode, never reaching monsterai. Team -1 or unread is hostile.
inline bool ekm_aa_mando_mannequin(int picnum, bool gotInitflags, int initflags,
                                   bool gotTeam, int team) {
  return ekm_picnum_is_aa_mando(picnum) && gotInitflags &&
         initflags >= EK_AA_INITFLAGS_MANNEQUIN && gotTeam && team != -1;
}
