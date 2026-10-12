// AA's bullet headshot band, and the Head aim point that targets it. Pure so the suite can pin it. The rule is
// SHOTSPARK1's (NOTENEMY.CON:11769-11859): a bullet landing at z <= sprite.z - ysize*yrepeat*K/2 on a whitelisted
// picnum is a decapitation, i.e. f = K*ov/(8*ysize) of the height above the feet; AA's band is [f, 1].

#pragma once
#include <string.h>
#include "aimsolve.h"   // EKM_HEAD_FRAC: the Head height when there is no band

// One whitelisted picnum. `dive` is K = 7 (else 6); `mtypeYsize` is the ysize AA substitutes at mtype 3, 0 for none;
// `inNamesH` 0 marks the six AA defines only in AADEFS.CON, which names.h must NOT name.
struct EkAaHeadTile {
  short picnum;
  unsigned char dive;
  unsigned char mtypeYsize;
  unsigned char inNamesH;
  const char* name;
};

static const EkAaHeadTile k_ek_aa_head_tiles[] = {
  {  1680, 0, 52, 1, "LIZTROOP" },
  {  2120, 0,  0, 1, "LIZMAN" },
  {  5270, 0,  0, 1, "LIZRANGER" },
  {  2000, 0,  0, 1, "PIGCOP" },
  { 17036, 0,  0, 1, "NEWPIG" },
  { 17081, 1,  0, 1, "NEWPIGDIVE" },
  { 17020, 0,  0, 1, "NEWPIGPISS" },
  { 16896, 0, 70, 1, "NEWTROOP" },
  { 16931, 0,  0, 1, "NEWTROOPDUCKING" },
  { 16993, 0,  0, 1, "NEWTROOPONTOILET" },
  { 16895, 0,  0, 1, "NEWTROOPJUSTSIT" },
  { 24320, 0, 70, 0, "NIPTROOP" },
  { 24355, 0,  0, 0, "NIPTROOPDUCKING" },
  { 24417, 0,  0, 0, "NIPTROOPONTOILET" },
  { 24439, 0,  0, 0, "NIPTROOPJUSTSIT" },
  {  2045, 1,  0, 1, "PIGCOPDIVE" },
  {  5300, 0,  0, 1, "LIZRANGERDUCKING" },
  {  1741, 0,  0, 1, "LIZTROOPONTOILET" },
  {  1742, 0,  0, 1, "LIZTROOPJUSTSIT" },
  {  4620, 0,  0, 1, "NEWBEAST" },
  {  8555, 0,  0, 1, "CYBERBEAST" },
  { 18505, 0,  0, 1, "CRONEN" },
  {  8795, 0,  0, 1, "ZOMBIE" },
  { 10624, 0,  0, 1, "ARMYANT" },
  { 10674, 0,  0, 1, "ARMYANTCRAWL" },
  { 12080, 0,  0, 1, "SPACEBULL" },
  { 20302, 0,  0, 1, "CRAZYLADY" },
  {  1920, 0,  0, 1, "COMMANDER" },
  { 17408, 0,  0, 1, "NEWCOMM" },
  { 17611, 0,  0, 1, "NEWTSABERDEATH" },
  { 17618, 0,  0, 1, "LIZMANSABERDEATH" },
  { 17625, 0,  0, 1, "NEWPIGSABERDEATH" },
  { 17632, 0,  0, 1, "DEASABERDEATH" },
  { 17638, 0,  0, 1, "MANDOSABERDEATH" },
  { 17644, 0,  0, 1, "SYTHSABERDEATH" },
  { 17650, 0,  0, 1, "SITHSABERDEATH" },
  { 22784, 0,  0, 0, "ARMLIZTROOP" },
  { 22839, 0,  0, 0, "ARMLIZTROOPDUCK" },
};
const int EK_AA_HEAD_TILE_COUNT =
    (int)(sizeof(k_ek_aa_head_tiles) / sizeof(k_ek_aa_head_tiles[0]));

// AA's own `ife actorvar[hitsprite].mtype 3` (:11774).
const int EK_AA_HEAD_MTYPE_OVERRIDE = 3;

// Head's lowest point stays 1/32 of the body above AA's line: the gap from a normal enemy's line (0.75) up to grid
// sample 0.78125 (stays eligible), and above half a pitch quantum at AA's 3072 headshot range.
const float EKM_AA_HEAD_MARGIN = 1.0f / 32.0f;

inline int ekm_aa_head_index(int picnum) {
  for (int i = 0; i < EK_AA_HEAD_TILE_COUNT; i++)
    if ((int)k_ek_aa_head_tiles[i].picnum == picnum) return i;
  return -1;
}

inline int ekm_aa_head_override_ysize(int idx, int ysize, bool haveMtype, int mtype) {
  if (idx < 0 || idx >= EK_AA_HEAD_TILE_COUNT) return ysize;
  const int ov = (int)k_ek_aa_head_tiles[idx].mtypeYsize;
  return (ov > 0 && haveMtype && mtype == EK_AA_HEAD_MTYPE_OVERRIDE) ? ov : ysize;
}

inline bool ekm_aa_head_name_ok(const EkAaHeadTile& row, const char* namesH) {
  if (row.inNamesH) return namesH && strcmp(namesH, row.name) == 0;
  return namesH == 0;
}

// Where Head aims on one target. `line` is f (-1 when not computed), `headFrac` the height to aim at, `floor` the
// band's lowest aimable point (0 none). INT, NOT BOOL, like every flag in a by-value struct (aimsolve.h).
struct EkAaHeadBand {
  float line;
  float headFrac;
  float floor;
  int banded;
};

// No band for Centre, an unlisted picnum, a centred sprite (cstat 128), a missing tile height, or a line so near the
// top that its middle would sit under its floor.
inline EkAaHeadBand ekm_aa_head_band(int aimPoint, int idx, unsigned cstat, int ysize,
                                     bool haveMtype, int mtype) {
  EkAaHeadBand b;
  b.line = -1.0f; b.headFrac = EKM_HEAD_FRAC; b.floor = 0.0f; b.banded = 0;
  if (!aimPoint || idx < 0 || idx >= EK_AA_HEAD_TILE_COUNT || ysize <= 0) return b;
  const int k = k_ek_aa_head_tiles[idx].dive ? 7 : 6;
  const int ov = ekm_aa_head_override_ysize(idx, ysize, haveMtype, mtype);
  b.line = (float)(k * ov) / (float)(8 * ysize);
  if (cstat & 128u) return b;
  if (b.line > 1.0f - 2.0f * EKM_AA_HEAD_MARGIN) return b;
  b.headFrac = (b.line + 1.0f) * 0.5f;
  b.floor = b.line + EKM_AA_HEAD_MARGIN;
  b.banded = 1;
  return b;
}
