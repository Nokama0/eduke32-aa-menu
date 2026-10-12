# Game-function hooks: P_PreFireHitscan, A_ShootWithZvel, A_IncurDamage (gamehooks.cpp)

The three functions are found in the shipped image by content, then verified by decoded facts. Any ambiguity means no
hook. Build: eduke32.exe r10669. Lengths come from the PE exception directory.

## Locations
- P_PreFireHitscan (player.cpp:475) begins at rva 0x0BA610, 731 bytes.
- A_ShootWithZvel (player.cpp:1689) at rva 0x0BA8F0, 18175 bytes.
- A_IncurDamage (actors.cpp:1299-1330) at rva 0x005B10, 1305 bytes.
- The first sixteen bytes of the first two occur 3 and 16 times in .text. Prologues are callee-save pushes (r14, r13,
  r12, rbp, rdi, rsi, rbx, then `sub rsp,imm`; the same with r15 in front).

## P_PreFireHitscan, five facts (player.cpp:475-529)
1. :478-479 angRange=32, zRange=256, written by two Gv_SetVar calls (:484-485): rva 0x0BA68E `mov edx,0x20` and 0x0BA69E
   `mov edx,0x100` into the SAME call target (0x090D60).
2. :480 GetAutoAimAng with 5<<8 and 0+1: rva 0x0BA642 loads r9d=0x500 and stores 1 and 0x100 into the outgoing stack
   slots.
3. :505-509 `(unsigned)statNum <= 30 && (statNumMap & (1<<statNum))`, with statNumMap = STAT_ACTOR | ZOMBIEACTOR |
   PLAYER | DUMMYPLAYER = bits 1, 2, 10, 13 = 0x2406: `cmp eax,0x1e`, `mov edx,0x2406`, `bt edx,eax` at 0x0BA7C7.
4. :528 `srcVect->z -= ZOFFSET6` is the single exit: rva 0x0BA7DE `sub dword ptr [rdi+8],0x400` (rdi is the 4th argument,
   8 is vec3_t::z, 0x400 is ZOFFSET6); every other path jumps to it.
5. Argument layout read from code: after 7 pushes and `sub rsp,0x70`, [rsp+0xD0] zvel, +0xD8 shootAng, +0xE0
   accurateAim, +0xE8 doSpread, which are entry rsp +0x28/+0x30/+0x38/+0x40, arguments five to eight.

## A_ShootWithZvel, four facts (player.cpp:1689-1730)
1. :1697-1703 `forceZvel != SHOOT_HARDCODED_ZVEL` (INT32_MIN, player.h:374): rva 0x0BA944 `cmp r8d,0x80000000`; both
   arms write g_overrideShootZvel and g_shootZvel.
2. :1710-1714 player arm: rva 0x0BAE31 reads pPlayer->pyoff, writes 0x309 (PCRACKTIME, player.h:46 = 777) as a 16-bit
   store into crack_time, and computes pos.z + pyoff + 0x400.
3. :1720-1724 actor arm: `startPos.z -= ((yrepeat*tilesiz.y)<<1) - ZOFFSET6` is `lea edx,[rdx+rdx-0x400]`, `sub
   ebp,edx` at 0x0BA998; `-= (7<<8)` is `sub ebp,0x700` at 0x0BA9BC.
4. :1728-1729 enemy muzzle offset sintable[(shootAng+1024+96)&2047] / [+512+96]: `lea eax,[rsi+0x460]` at 0x0BA9E2 and
   `lea eax,[rsi+0x260]` at 0x0BA9FB, both masked with 0x7FF.

## A_IncurDamage, six facts (actors.cpp:1299-1330; actor offsets from actorstate.h:48-52)
1. :1301-1302 `&actor[spriteNum]`: rva 0x005B26 `lea rax,[rsi+rsi*2]`, `shl rax,5` (stride 96).
2. :1305 the three-way safety test: `movzx r13d,[rax+0x4C]` (htextra) 0x005B34, `cmp word [rbx+0x2A],0` (sprite extra;
   spritetype is 44 bytes, `imul rbx,rsi,0x2C` at 0x005B43), `movzx eax,[rax+0x48]` (htpicnum) 0x005B5C, each followed
   by a `js` to the shared `htextra = -1; return -1` block at 0x006010.
3. :1311 `pSprite->picnum != APLAYER` is `cmp cx,[rbx+0xE]` at 0x005B70, splitting the actor and player arms.
4. :1319 VM_OnEvent(EVENT_PREACTORDAMAGE) is FORCE_INLINE (gameexec.h:49) and gates on apScriptEvents[nEventID]. The id
   is folded into a rip displacement on the hot path and appears as an immediate only on the cold one: 0x005B9A tests
   the global and jumps to 0x005D4A `mov ecx,0xA0` with argument five 0 at [rsp+0x20]. 0xA0 = 160 is the ordinal counted
   from events_defs.h's enum.
5. :1322 `extra -= htextra` is `sub word ptr [rbx+0x2A], r13w` at 0x005BD4; :1324 `owner < MAXSPRITES` and `statnum <
   MAXSTATUS` are `cmp ecx,0x3FFF` and `cmp ...,0x3FF`.
6. :1327-1328 is the first pattern below.
Facts 4 and 6 are both required at run time (uniqueness is not correctness). The code does not check against 0x005B10 /
1305 / +275, which would refuse a correct locate on the next build.

## Patterns
- EK_PREFIRE_SIG: 64 bytes from rva 0x0BA767 (player.cpp:500-501, the accurate-aim hitscan() call; the +512/&2047
  sintable pair, sprite[].sectnum via imul 0x2C and [rbx+0x18], literal 0x1000040 = CLIPMASK1 = ((256L)<<16)+64L,
  build.h:226).
- EK_SHOOT_SIG: 45 bytes from rva 0x0BAE31 (player.cpp:1710-1714, the 0x309 and 0x400 immediates).
- EK_INCURDAMAGE_SIG: 23 bytes from rva 0x005C23 (actors.cpp:1327-1328; both actor offsets 0x4C/0x48 and the stride-96
  arithmetic). Matched once inside the function at 0x005B10, +275.
- EK_INCURDAMAGE_SIG2: 13 bytes from rva 0x005D4A (actors.cpp:1319, fact 4), scanned within the function's extent
  0x005B10..0x006029 (once, +570; also once across all of .text).
- The P_PreFireHitscan window is: `mov r8d,edx; add edx,0x200; add rbx,[rsi]; and r8d,0x7FF; and edx,0x7FF; movsx
  r9d,[rcx+r8*2]; movsx r8d,[rcx+rdx*2]; lea rcx,[rsp+0x50]; movsx edx,[rbx+0x18]; mov [rsp+0x28],rcx; mov rcx,rdi;
  mov [rsp+0x30],CLIPMASK1; mov [rsp+0x20],eax`.

## A_InsertSprite (game.cpp:1348-1411)
Measured on the AA image first (r10669-ec5824db8), byte-identical to the stock one. Entry rva 0x034580, 1481 bytes, the
only function referencing "Too many sprites spawned." (:1370).
- Arguments: 8 pushes and `sub rsp,0xE8`, so the body's [rsp+0x150/0x170/0x178/0x180/0x190] are entry
  [rsp+0x28/0x48/0x50/0x58/0x68] = arguments 5, 9, 10, 11, 13. s_pn goes to di and picnum; s_ss, s_a, s_ve, s_zv are
  packed into xmm6 words 1, 2, 4, 6 and stored at +0x1A (statnum), +0x1C (ang), +0x20 (xvel), +0x24 (zvel).
- EK_INSERT_SIG: 87 bytes at 0x034753 (+467), the :1377 record store ending in one `movups [rax+0x18],xmm6`, `rep stosq`
  and bpos. EK_INSERT_SIG2: 21 bytes at 0x034967 (+999), `mov ecx,0x49` (EVENT_EGS = 73) with block_deletesprite++.
- Call site 0x0BB7F5 in A_ShootWithZvel (+3845) is player.cpp:995.

## Method
Ghidra on the stock build, byte patterns checked for uniqueness in .text, offsets checked against the source.
