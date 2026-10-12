#pragma once

// THE ACTORS WHOSE GATE PICKUP RANGE MAY WIDEN, and only these. Each is an AAITEMS.CON actor whose pickup is AA's
// standard gate -- `ifpdistl RETRIEVEDISTANCE` then `ifcanseetarget` (ACCESSCARD: either `ifpdistl 384` or
// sight,:405-406) -- paid through getcode / getweaponcode / quikweaponget or its own inline epilogue.

struct EkPickupAaRow { short picnum; const char* name; short line; };

const int EKM_PR_AA_COUNT = 68;

inline const EkPickupAaRow* pickuprange_aa_rows(void) {
  static const EkPickupAaRow kRows[EKM_PR_AA_COUNT] = {
    {    21, "FIRSTGUNSPRITE",   1794 },
    {    22, "CHAINGUNSPRITE",   2806 },
    {    23, "RPGSPRITE",        1124 },
    {    24, "FREEZESPRITE",     3690 },
    {    25, "SHRINKERSPRITE",   3451 },
    {    28, "SHOTGUNSPRITE",    1239 },
    {    29, "DEVISTATORSPRITE", 3746 },
    {    32, "GROWSPRITEICON",   3569 },
    {    36, "SAWSPRITE",        2330 },
    {    37, "FREEZEAMMO",        578 },
    {    40, "AMMO",              486 },
    {    41, "BATTERYAMMO",       836 },
    {    42, "DEVISTATORAMMO",    868 },
    {    44, "RPGAMMO",          1021 },
    {    45, "GROWAMMO",          797 },
    {    46, "CRYSTALAMMO",       703 },
    {    47, "HBOMBAMMO",        1067 },
    {    48, "AMMOLOTS",          683 },
    {    49, "SHOTGUNAMMO",       637 },
    {    51, "COLA",             1625 },
    {    52, "SIXPAK",           1547 },
    {    53, "FIRSTAID",         1733 },
    {    54, "SHIELD",            206 },
    {    55, "STEROIDS",          117 },
    {    56, "AIRTANK",           271 },
    {    57, "JETPACK",           335 },
    {    59, "HEATSENSOR",        181 },
    {    60, "ACCESSCARD",        378 },
    {    61, "BOOTS",             156 },
    {   100, "ATOMICHEALTH",     1677 },
    {  1348, "HOLODUKE",          296 },
    {  5178, "PIGBOSSGUN",       2303 },
    {  5329, "RAILGUNSPRITE",    3876 },
    {  5331, "RAILGUNAMMO",       898 },
    {  5334, "RADARSPRITE",      4056 },
    {  5860, "LASERPISTOL",      2151 },
    {  5885, "SHIELDBATTERY",    2487 },
    {  8867, "SAWAMMO",           991 },
    {  8903, "TRIPBOMBUPGRADE",  3009 },
    {  8909, "ATOMICSHOTTY",     1186 },
    {  8938, "BOWSPRITE",        2096 },
    {  8946, "QUIVERSPRITE",      947 },
    { 10357, "NUKESPRITE",       1455 },
    { 10374, "CHAINGUNUPGRADE",  2895 },
    { 10375, "M4UPGRADE",        2932 },
    { 10376, "DEUPGRADE",        3059 },
    { 10377, "PISTOLUPGRADE",    3158 },
    { 10392, "SHOTGUNUPGRADE",   2969 },
    { 10400, "INCINUPGRADE",     3227 },
    { 10401, "FREEZEUPGRADE",    3191 },
    { 10423, "PLASMASPRITE",     3976 },
    { 10424, "PLASMAAMMO",       4011 },
    { 10700, "SPIDERMINESPRITE", 2767 },
    { 10934, "SABERSPRITE",      2406 },
    { 11005, "DUALDUKE",         3091 },
    { 11006, "DUALSHELLY",       3124 },
    { 13010, "BMFGSPRITE",       3513 },
    { 15480, "SHOCKRIFLESPRITE", 3616 },
    { 16633, "COVERSPRITE",       357 },
    { 17494, "COMMGUN",          2230 },
    { 17660, "RESCUEREFILL",     1711 },
    { 18669, "DBSPRITE",         1361 },
    { 18670, "DBAMMO",           1424 },
    { 19941, "BLOODAMMO",         762 },
    { 19960, "STAKESPRITE",      4033 },
    { 22011, "STEALTHCHARGER",   2562 },
    { 22639, "RADAWAY",          4078 },
    { 26103, "JETPACKUPGRADE",   4646 },
  };
  return kRows;
}

// The row index of a picnum, or -1. Linear: 68 compares, run at verify only.
inline int pickuprange_aa_row(int picnum) {
  if (picnum <= 0) return -1;
  const EkPickupAaRow* const rows = pickuprange_aa_rows();
  for (int i = 0; i < EKM_PR_AA_COUNT; i++)
    if (rows[i].picnum == picnum) return i;
  return -1;
}

inline bool pickuprange_aa_listed(int picnum) { return pickuprange_aa_row(picnum) >= 0; }

// THE SEVEN WEAPONS WHOSE DROPPED SLIDE IS `ifpdistl RETRIEVEDISTANCE resetcount` (AAITEMS.CON, one line each). That
// 844 moves with the gate: left at 844, a swapped weapon slides just past it, stops, and is re-grabbed inside R in a
// loop.
const int EKM_PR_AA_RESETS = 7;

inline bool pickuprange_aa_resets(int picnum) {
  static const short kResets[EKM_PR_AA_RESETS] = {
    10357,   // NUKESPRITE
       21,   // FIRSTGUNSPRITE
     8938,   // BOWSPRITE
     5860,   // LASERPISTOL
    17494,   // COMMGUN
       36,   // SAWSPRITE
    10934,   // SABERSPRITE
  };
  for (int i = 0; i < EKM_PR_AA_RESETS; i++)
    if (kResets[i] == picnum) return true;
  return false;
}
