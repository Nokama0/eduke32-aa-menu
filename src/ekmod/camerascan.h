#pragma once

// Locate half of camera.cpp's shared state: located rvas, draw-seen/sweep/fog verdicts, image-reading helpers.

extern volatile LONG s_drawBegin;
extern volatile LONG s_drawEnd;
extern volatile LONG s_getzRva;
extern volatile LONG s_getzSiteRva;

extern const unsigned char* s_base;

extern volatile LONG s_usRva;
extern volatile LONG s_dsRva;
extern volatile LONG s_uwRva;
extern volatile LONG s_dwRva;
extern volatile LONG s_prsRva;
extern volatile LONG s_prwRva;
extern volatile LONG s_dstateRva;
extern volatile LONG s_sprSlotRva;
extern volatile LONG s_tsprSlotRva;
extern volatile LONG s_ssCntSlotRva;
extern volatile LONG s_headSlotRva;
extern volatile LONG s_nextSlotRva;

extern volatile LONG s_sweepOk;
extern volatile LONG s_spritesOk;
extern volatile LONGLONG s_gvisAddr;

extern volatile LONGLONG s_otclkAddr;

// Image rva a.refptr slot holds, or 0 if unreadable or outside the module.
size_t slot_target_rva(const unsigned char* base, unsigned slot);

size_t host_image_size(const unsigned char* base);
