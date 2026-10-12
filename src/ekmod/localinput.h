#pragma once

// localInput, the input record the tic consumes, written by the triggerbot's fire, weapon replay and Alt Fire bits. Bit
// numbers are macros.h #defines, confirmed by `shl eax, 0x1a` at rva 0x4b3ce = SK_GAMEQUIT 26.

// Once per frame from the SwapBuffers hook, even with the triggerbot off (the Unavailable label asks
// localinput_ready()).
void localinput_locate(void);

bool localinput_ready(void);

unsigned localinput_rva(void);

const int EK_SK_FIRE = 2;

// OR a mask into localInput.bits (own address, SEH guard). Never clear: the record is cleared every tic and the player
// may hold the key.
bool localinput_or_bits(unsigned mask);

// Current bits: P_GetInput's output plus our ORs (tbotswitch.h reads the weapon field).
bool localinput_read_bits(unsigned* out);

const int EK_EXT_ALT_FIRE = 6;
const int EK_EXT_CHAT_MODE = 7;

// input_t (player.h:171, pack(1)): bits +0, fvel/svel +4/+6, q16avel/q16horz +8/+12, extbits +16. The locate's 0x80
// store (`extbits = BIT(EK_CHAT_MODE)`, player.cpp:3208) must land here.
const int EK_INPUT_T_EXTBITS = 16;

inline bool ekm_localinput_extbits_agree(unsigned bitsRva, unsigned extRva) {
  return extRva == bitsRva + (unsigned)EK_INPUT_T_EXTBITS;
}

bool localinput_or_extbits(unsigned mask);
bool localinput_read_extbits(unsigned* out);
