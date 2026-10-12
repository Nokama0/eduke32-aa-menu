# Skip intro: g_noLogo located by two signature scans (skipintro.cpp)

The menu sets the engine's own `-nologo` flag at DLL attach. `g_noLogo` (screens.cpp:50) is read by G_DisplayLogo before
the first frame, so a setting loaded later is too late. Two independent scans must name the same address; a value that is
not 0 at attach refuses. Build: eduke32.exe r10669.

## Anchor A, the write (cmdline.cpp:517-519)
- "nologo\0" and "quick\0" are each referenced by one `lea rdx,[rip+d]` feeding Bstrcasecmp. The `call; test eax,eax; je
  rel32` after each lea lands both on one `mov dword [rip+d], 1`.
- r10669-ec5824db8: leas at rva 0x2a3d7 and 0x2a3ee, both je to 0x2b226, `C7 05 74 20 B3 00 01 00 00 00`, naming rva
  0xB5D2A4.

## Anchor B, the read (screens.cpp:1722, inlined by LTO)
The read goes through a mingw .refptr slot, then EVENT_MAINMENUSCREEN's apScriptEvents test at 130 * 8 = 0x410
(events_defs.h:8-138):
`48 8B 05 ?? ?? ?? ??` mov rax,[rip+d] (the .refptr slot); `8B 00` mov eax,[rax]; `85 C0` test; `89 84 24 ?? ?? ?? ??` mov
[rsp+x],eax; `75 ??` jne (!g_noLogo fails); `48 8B 05 ?? ?? ?? ??` mov rax,[rip+d] (apScriptEvents); `48 83 B8 10 04 00
00 00` cmp qword [rax+0x410],0. On r10669 there is one match, at rva 0x556bd; the slot is 0x5A2010 (DIR64-relocated) and
holds ImageBase+0xB5D2A4.

## Why a write at attach survives
0xB5D2A4 is .bss (SizeOfRawData 0, the loader hands it zeroed) and `int32_t g_noLogo = 0` is constant-initialised, so no
CRT initialiser runs over it. Image-wide its only rip-relative references are five `mov dword [rip+d], 1` (cmdline.cpp:152,
:368, :375, :384, :519) and its only absolute pointer is anchor B's slot, read once. The attach path runs under the loader
lock and is safe there: no LoadLibrary, thread or wait, and the exe's relocations are applied before DllMain.

## Acceptance
The store target, the read target and a writable non-executable section must agree, with at least four bytes to the end
of the section; the target must be committed writable memory and hold 0.

## Method
Ghidra on the stock build, byte patterns checked for uniqueness in the code sections, engine source for the line numbers.
