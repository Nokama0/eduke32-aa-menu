#pragma once

#include <string.h>   // strcmp: a row's operator, by the generator's own name

// Reload ledger: every site gen_con_stops_aa.py attributes to a covered slot (or every slot) with a stop at or above
// the slot's floor is a row. E (state) and U (unclear) rows hold the advance; C (cosmetic) and I (inert) rows are never
// applied (masks EK_RL_ALL). `--check-reload` fails on a site no row names or a row no site backs.

const int EK_RL_ANY_SLOT = -1;
enum { EK_RL_E = 0, EK_RL_U = 1, EK_RL_C = 2, EK_RL_I = 3 };

// pchar, AADEFS.CON:3267: 0 duke, 1 bombshell, 2 wes, 3 mando, 4 deanova.
const int EKM_RELOAD_CHARS = 5;
const int EK_RL_DUKE = 1, EK_RL_SHELLY = 2, EK_RL_WES = 4, EK_RL_MANDO = 8,
          EK_RL_NOVA = 16, EK_RL_ALL = 31;

struct EkReloadRow {
  int weapon;
  int frame;
  const char* op;
  int chars;
  int cls;
  const char* file;
  int line;
  const char* text;
};

// Ledger floor per slot: the lowest literal WEAPONn_TOTALTIME in AAPLAYER.CON (w01 4, w02 11, w03 12). Below the floor
// no reload set applies.
inline int ekm_reload_floor(int weapon) {
  switch (weapon) {
    case 1: return 4;
    case 2: return 11;
    case 3: return 12;
    default: return 0;
  }
}

const int EKM_RELOAD_ROW_COUNT = 76;

inline const EkReloadRow* ekm_reload_rows(void) {
  static const EkReloadRow kRows[EKM_RELOAD_ROW_COUNT] = {
    // Every slot. Bot dodge, all characters; above TotalTime only on rows with TT <= 7.
    { EK_RL_ANY_SLOT, 8, "ifle", EK_RL_ALL, EK_RL_U, "CHEERBOT.CON", 326,
      "ifle player[].kickback_pic 8" },
    // Enemy dodge while the player "fires", gated `ifle pchar 1` at :742.
    { EK_RL_ANY_SLOT, 12, "ifl", EK_RL_DUKE | EK_RL_SHELLY, EK_RL_U, "ATARASHII.CON", 739,
      "ifl player[].kickback_pic 12" },
    // EVENT_PREVIOUSWEAPON / NEXTWEAPON: `setp kickback_pic 0` on the bow.
    { EK_RL_ANY_SLOT, 14, "ife", EK_RL_ALL, EK_RL_E, "AAPLAYER.CON", 23171,
      "ife player[].kickback_pic 14" },
    { EK_RL_ANY_SLOT, 14, "ife", EK_RL_ALL, EK_RL_E, "AAPLAYER.CON", 23320,
      "ife player[].kickback_pic 14" },
    // Wes's SKAR block, GROW/SHRINKER only (:13615-13617).
    { EK_RL_ANY_SLOT, 5, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 13682,
      "setp[].kickback_pic 5" },
    { EK_RL_ANY_SLOT, 17, "ife", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 13688,
      "ife player[].kickback_pic 17" },
    { EK_RL_ANY_SLOT, 40, "ifg", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 13697,
      "ifg player[].kickback_pic 40" },
    // The saber's write, above every Reload: the engine resets on the next tic.
    { EK_RL_ANY_SLOT, 99, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 10326,
      "setp[].kickback_pic 99 // to prevent KNEE from firing" },

    // w01 pistol. Tracer x-flip on the shot's own sprite.
    { 1, 4, "ifg", EK_RL_ALL, EK_RL_C, "AAGAME.CON", 18158,
      "ifg player[].kickback_pic 4 seta[RETURN].cstat 4" },
    // Shots: Duke's dual (:12604), Bombshell's dual Glock (:12671), Wes's dual revolvers (:12773), Mando's flame loop
    // (:12855).
    { 1, 5, "ife", EK_RL_DUKE, EK_RL_E, "AAPLAYER.CON", 12611,
      "ife player[].kickback_pic 5" },
    { 1, 5, "ife", EK_RL_SHELLY, EK_RL_E, "AAPLAYER.CON", 12706,
      "ife player[].kickback_pic 5" },
    { 1, 7, "ife", EK_RL_WES, EK_RL_E, "AAPLAYER.CON", 12828,
      "ife player[].kickback_pic 7 // 6" },
    { 1, 7, "ife", EK_RL_MANDO, EK_RL_E, "AAPLAYER.CON", 12913,
      "ife player[].kickback_pic 7" },
    // Reload-start landings: the trigger is TotalTime-1 (:12581) or rest (:22277, :22310, :22921).
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 12590,
      "setplayer[THISACTOR].kickback_pic 7" },
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22290,
      "setp[].kickback_pic 7" },
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22302,
      "{ sound GLOCKMAGOUT setplayer[THISACTOR].kickback_pic 7 }" },
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22319,
      "setp[].kickback_pic 7" },
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22926,
      "setp[].kickback_pic 7" },
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22939,
      "setp[].kickback_pic 7" },
    { 1, 7, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22950,
      "ifvarand shellyupgrades[PISTOL_WEAPON] 8 { sound GLOCKMAGOUT setplayer[THISACTOR].kickback_pic 7 } else" },
    { 1, 9, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 12587,
      "ife pchar 2 setp[].kickback_pic 9" },
    { 1, 9, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22286,
      "ife pchar 2 setp[].kickback_pic 9" },
    { 1, 9, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22935,
      "ife pchar 2 setp[].kickback_pic 9" },
    { 1, 10, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 12589,
      "ife pchar 1 setp[].kickback_pic 10 else" },
    { 1, 10, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22288,
      "ife pchar 1 setp[].kickback_pic 10" },
    { 1, 10, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22937,
      "ife pchar 1 setp[].kickback_pic 10" },
    // Falling clips: `espawn SHELL`, mtype CLIPFALL (NOTENEMY.CON:2700).
    { 1, 12, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12633,
      "ife player[].kickback_pic 12" },
    { 1, 12, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12730,
      "ife player[].kickback_pic 12" },
    { 1, 15, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11727,
      "else ife player[].kickback_pic 15 screensound CYLINDEROUT" },
    // Wes's casing loop: TMP_A/TMP_B are local, the block only spawns SHELL.
    { 1, 15, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12789,
      "ifg fastfire 0 ife player[].kickback_pic 15 set temp YES" },
    { 1, 15, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12791,
      "ifvarand perks 2048 ife player[].kickback_pic 15 set temp YES" },
    // Nova GREENBULLET (NOTENEMY.CON:10292-10296): arms the stuck bullets.
    { 1, 15, "ifg", EK_RL_NOVA, EK_RL_E, "NOTENEMY.CON", 10298,
      "ifg player[].kickback_pic 15" },
    { 1, 16, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12793,
      "ife player[].kickback_pic 16 set temp YES" },
    { 1, 17, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11713,
      "ife player[].kickback_pic 17 screensound WRISTBUTTON" },
    { 1, 22, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12737,
      "ife player[].kickback_pic 22 // 27" },
    // THE REFILL, `set THISCLIP PISTOLCLIP` (D12). No pchar gate in its block.
    { 1, 25, "ifge", EK_RL_ALL, EK_RL_E, "AAPLAYER.CON", 12556,
      "ifge player[].kickback_pic 25" },
    { 1, 25, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12640,
      "ife player[].kickback_pic 25 sound GLOCKMAGIN" },
    { 1, 26, "ifle", EK_RL_ALL, EK_RL_E, "AAPLAYER.CON", 12557,
      "ifle player[].kickback_pic 26" },
    { 1, 26, "ife", EK_RL_ALL, EK_RL_E, "AAPLAYER.CON", 12562,
      "ife player[].kickback_pic 26" },
    // Nova GREENBULLET (:11689, :11711): ends that reload.
    { 1, 27, "ifge", EK_RL_NOVA, EK_RL_E, "AAPLAYER.CON", 11714,
      "ifge player[].kickback_pic 27" },
    { 1, 30, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12626,
      "ife player[].kickback_pic 30 sound EJECT_CLIP" },
    { 1, 33, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11728,
      "else ife player[].kickback_pic 33 screensound DEAGLE_CLIPIN" },
    { 1, 33, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12751,
      "ife temp YES ife player[].kickback_pic 33 sound GLOCKMAGOUT" },
    // Nova (NOTENEMY.CON:10292): `set initsprite YES`, read at :10347, :10354.
    { 1, 33, "ife", EK_RL_NOVA, EK_RL_E, "NOTENEMY.CON", 10306,
      "ife player[].kickback_pic 33" },
    { 1, 34, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12752,
      "else ife player[].kickback_pic 34 sound GLOCKMAGOUT" },
    { 1, 34, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12762,
      "ife player[].kickback_pic 34 sound DEAGLE_CLIPOUT" },
    { 1, 42, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12627,
      "ife player[].kickback_pic 42 sound INSERT_CLIP" },
    { 1, 46, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12766,
      "ife temp YES ife player[].kickback_pic 46 sound DEAGLE_CLIPIN" },
    { 1, 47, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12768,
      "ife player[].kickback_pic 47 sound DEAGLE_CLIPIN" },
    { 1, 53, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12757,
      "ife temp YES ife player[].kickback_pic 53 sound GLOCKMAGIN" },
    { 1, 54, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12758,
      "else ife player[].kickback_pic 54 sound GLOCKMAGIN" },
    { 1, 55, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 12647,
      "ife temp YES ife player[].kickback_pic 55 sound GLOCKMAGIN" },

    // w02 shotgun. Duke's double barrel (:13022): out of shells, kickback_pic 0.
    { 2, 11, "ifg", EK_RL_DUKE, EK_RL_E, "AAPLAYER.CON", 13027,
      "ifg player[].kickback_pic 11 ifl dbammo 2 setp[].kickback_pic 0" },
    // Reload-start landings at rest (:13257, :22203) or from the stake end.
    { 2, 11, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 13284,
      "setp[].kickback_pic 11" },
    { 2, 11, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 13371,
      "setp[].kickback_pic 11" },
    { 2, 11, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22232,
      "setp[].kickback_pic 11" },
    // The bow held drawn: `temp` is set only for pchar 1 and 2 (:13143-13144).
    { 2, 12, "ifvarg", EK_RL_SHELLY | EK_RL_WES, EK_RL_E, "AAPLAYER.CON", 13152,
      "ifvarg player[].kickback_pic 12 ifvarl player[].kickback_pic 16 ifvarand bits 4" },
    { 2, 14, "case", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11703,
      "case 14 screensound OPENGUN break" },
    { 2, 14, "set", EK_RL_SHELLY | EK_RL_WES, EK_RL_E, "AAPLAYER.CON", 13153,
      "setplayer[].kickback_pic 14" },
    { 2, 16, "ifvarl", EK_RL_SHELLY | EK_RL_WES, EK_RL_E, "AAPLAYER.CON", 13152,
      "ifvarg player[].kickback_pic 12 ifvarl player[].kickback_pic 16 ifvarand bits 4" },
    { 2, 16, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 13270,
      "setp[].kickback_pic 16" },
    { 2, 16, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22213,
      "setp[].kickback_pic 16" },
    { 2, 17, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13033,
      "ife player[].kickback_pic 17 spawn SHOTGUNSHELL" },
    { 2, 17, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22976,
      "ifvarand dukeupgrades[SHOTGUN_WEAPON] 32 setp[].kickback_pic 17" },
    { 2, 20, "case", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11704,
      "case 20 case 32 screensound SMALLCHARGE break" },
    { 2, 21, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13030,
      "ife player[].kickback_pic 21 screensound DBLOAD" },
    { 2, 23, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13040,
      "ife player[].kickback_pic 23 spawn SHOTGUNSHELL" },
    { 2, 23, "set", EK_RL_ALL, EK_RL_I, "AAPLAYER.CON", 22977,
      "else setp[].kickback_pic 23" },
    // The stake's end restores WEAPON2_* and can start Wes's reload; `ifge
    // pchar 2` at :13349.
    { 2, 24, "ifg", EK_RL_WES | EK_RL_MANDO | EK_RL_NOVA, EK_RL_E, "AAPLAYER.CON", 13355,
      "ifg player[].kickback_pic 24 set temp YES else" },
    { 2, 29, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13031,
      "ife player[].kickback_pic 29 screensound DBLOAD" },
    { 2, 32, "case", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11704,
      "case 20 case 32 screensound SMALLCHARGE break" },
    { 2, 33, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13037,
      "ife player[].kickback_pic 33 screensound DBLOAD" },
    { 2, 36, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13032,
      "ife player[].kickback_pic 36 screensound DBCLOSE" },
    { 2, 45, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13038,
      "ife player[].kickback_pic 45 screensound DBLOAD" },
    { 2, 47, "case", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 11705,
      "case 47 screensound SELECT3 break" },
    { 2, 53, "ife", EK_RL_ALL, EK_RL_C, "AAPLAYER.CON", 13039,
      "ife player[].kickback_pic 53 screensound DBCLOSE" },
    // w03 chaingun: every site at or above 12 is an every-slot row above.
  };
  return kRows;
}

inline int ekm_reload_row_stops(const EkReloadRow& r, int* out) {
  if (!out || !r.op) return 0;
  static const char* const kAtF[] = { "ife", "ifvare", "ifge", "ifvarge", "ifle",
                                      "ifvarle", "case" };
  static const char* const kAtF1[] = { "ifg", "ifvarg" };
  bool atF = false, atF1 = false;
  for (int i = 0; i < (int)(sizeof(kAtF) / sizeof(kAtF[0])); i++)
    if (strcmp(r.op, kAtF[i]) == 0) atF = true;
  for (int i = 0; i < (int)(sizeof(kAtF1) / sizeof(kAtF1[0])); i++)
    if (strcmp(r.op, kAtF1[i]) == 0) atF1 = true;
  const bool both = !atF && !atF1;
  int n = 0;
  if ((atF || both) && r.frame - 1 >= 1) out[n++] = r.frame - 1;
  if ((atF1 || both) && r.frame >= 1) out[n++] = r.frame;
  return n;
}

inline bool ekm_reload_row_applies(const EkReloadRow& r, int weapon, int pchar) {
  if (r.weapon != weapon && r.weapon != EK_RL_ANY_SLOT) return false;
  if (r.cls != EK_RL_E && r.cls != EK_RL_U) return false;
  if (pchar < 0 || pchar >= EKM_RELOAD_CHARS) return false;
  return (r.chars & (1 << pchar)) != 0;
}
