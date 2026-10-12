# Locating `cansee` in the shipped image (cansee.cpp)

The menu calls the engine's `cansee` for line-of-sight checks. The function is located by content.

## The function
`cansee` begins at rva 0x1664B0 and ends 0x167261 in eduke32.exe r10669 (engine.cpp:12718-12915). Seven facts
identify it:
1. Nine arguments, the 4th and 8th read as 16 bits (`movsx r9d,r9w` for sect1, `movsx r9d,bx` for the 8th stack
   argument): the build.h:1351 signature.
2. Two calls to one 4-argument helper on (x1,y1,z1,(int16)sect1) and (x2,y2,z2,(int16)sect2), guarded by `> 0`: the yax
   block :12727-12731 calling yax_cansee_fixsector twice.
3. `or eax,esi; cmp ax,0xfff; jbe` then `xor eax,eax; ret`: the two MAXSECTORS tests :12747-12748 merged by GCC.
4. Inlined sectorsareconnected (:12750): the 2D bitmap at sect1 * bitmap_size(numsectors) + sect2/8, tested with
   pow2char.
5. `mov ecx,0x40; rep stosq` over a .bss array: `Bmemset(sectbitmap,0,bitmap_size(MAXSECTORS))` :12758 (512 bytes,
   build.h:48, :59); the bit for sect1 is set at :12768 and the frontier seeded at :12769.
6. `if (x1 == x2 && y1 == y2) return (sect1 == sect2);` :12762-12763, compiled as two compares into `cmp si,bp; sete al`.
7. Nine mulscale24 (imul r64 then `sar r64,24`) and two `(1<<24)` bound tests: the only function with that count pair.

## Call sites
23 call sites pass the ninth argument as the literal 0x20 (the default) and load the 8th as `movsx r8d, word ptr
[rbx+0x18]` (sprite.sectnum). One of them (rva 0x0F5F95) is the auto-aim shape from player.cpp:312.

## The 53-byte pattern
The pattern (53 bytes from rva 0x1667D0, engine.cpp:12784-12791, the per-wall segment intersection) starts and ends in a
run with no rip-relative operand, absolute address or branch displacement. It lands 0x320 into the body. It is
compiler-dependent: [hitscan.md](hitscan.md) locates its function by content instead.

## Behaviour notes
- A probe at 0x3000000 distance overflows int32 in every segment product (engine.cpp:12795 carries an "XXX: OVERFLOW"
  note). With sect1 == sect2 the terminal bitmap_test (:12914-12915) is set, so a loop that rejects nothing returns 1.
- The CSTAT_WALL_1WAY value is buildtypes.h:148 = 0x0020.

## Method
Ghidra on the stock eduke32.exe r10669; byte patterns checked for uniqueness across .text.
