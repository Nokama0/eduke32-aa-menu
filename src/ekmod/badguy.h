#pragma once

// Aligned, not packed: flags at +32 on x64 (x86's +16 misreads defproj).
const int EK_TILEDATA_STRIDE = 40;
const int EK_TILEDATA_FLAGS  = 32;

const unsigned EK_SFLAG_BADGUY           = 0x00000020u;
const unsigned EK_SFLAG_HARDCODED_BADGUY = 0x00040000u;
const unsigned EK_SFLAG_ANY_BADGUY = EK_SFLAG_BADGUY | EK_SFLAG_HARDCODED_BADGUY;

bool badguy_scan(void);
bool badguy_ready(void);
unsigned badguy_rva(void);
int badguy_count(void);

bool picnum_is_enemy(int picnum);

// Engine-flagged no-threat enemies (RAT).
bool picnum_is_harmless_critter(int picnum);

const unsigned char* badguy_tile_record(int picnum);
