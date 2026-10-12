#pragma once

// AA actors declared `useractor notenemy` (no SFLAG_BADGUY) that both die and attack the player per their own CON.
const int EK_AA_POLYMORPH   = 7220;
const int EK_AA_ROLLYTURRET = 7727;
const int EK_AA_SSLIME      = 23552;

inline bool ekm_aa_hostile_notenemy(int picnum) {
  return picnum == EK_AA_POLYMORPH || picnum == EK_AA_ROLLYTURRET ||
         picnum == EK_AA_SSLIME;
}

// AA's death marker: `enemy_death` sets monstatus 2.
const int EKM_AA_MONSTATUS_DEAD = 2;
