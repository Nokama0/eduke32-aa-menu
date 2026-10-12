# Polymer whole-map sweep: sky, array bounds, culling, masks

The out-of-bounds free camera needs the whole map drawn. The mod drives the engine's own sweep functions over the
arrays Polymer keeps per sector and wall. This note holds the engine facts that decide when that is safe. Build:
eduke32.exe r10669. The rvas of the sweep functions and array slots are in [freecam.md](freecam.md).

## The two arms of polymer_drawrooms
- The sky is drawn on the normal arm only. polymer_drawrooms' external-view arm (polymer.cpp:1214-1251) is taken when
  the z handed in lies outside the sector's band; it returns at :1250 before polymer_displayrooms (:1254) and
  polymer_drawsky (:1845).
- The external arm (polymer.cpp:1229-1243) loops to numsectors / numwalls and draws everything with no visibility test.
  It is a rarely taken fallback in the engine, but it ran every frame the camera row was on.

## Array bounds
- sector[] is a pointer: enginePreInit assigns `sector = sector_s` (engine.cpp:9154, :9174) above `wall = wall_s`. The
  adjacency anchor (sector next to wall in memory) is dead: GCC groups those globals in a debug build and scatters them
  in release.
- Validator: three walls minimum (zeroed memory dies at index 0); the first wall loop closes within wallnum steps via
  wall[].point2 (inner loops close early).
- Sweep bound: polymer_freeboard NULLs prsectors[]/prwalls[] (:2487-2527); loadboard initialises exactly
  numsectors/numwalls (:1053-1067); polymer_drawsector dereferences prsectors[sectnum] unchecked (:3043). The
  polymer_updatesector -1 guard is not in the shipped binary (callers ignore it and GCC merged both returns). The ESP
  deliberately uses the tiling heuristic (a false accept is only a phantom marker, see mapextent.h); the sweep cannot
  (a NULL dereference in polymer_drawplane).
- prsectors[] slot reads: drawsector once (:3043), updatewall twice (:3258, :3262), drawwall never; head of
  polymer_updatesector `_prsector *s = prsectors[sectnum]; if (s == NULL)` (:2624-2626).
- prwalls[] slot reads: updatewall (:3257) and drawwall (:3652, `w->underover` :3664), never drawsector (:3033-3142);
  polymer_updatewall's extent test is `sectofwall >= numsectors || wallnum > numwalls` (:3239-3240); polymer_pokesector
  is `static inline` (:3816) and reads prwalls[] at :3830. polymer_drawwall is 0x397 bytes on r10669; sector[] and
  wall[] (40 and 32 byte records) are reached by lea/shl, never a scale-8 mov. Bound: MAXWALLS (4x prsectors[]).
- drawingstate is set at polymer.cpp:1832, :1991, :2004, :2030; its callees' calls are drawsector :1859/:2058 and
  drawwall :1978/:2063. `Bmemset(drawingstate,0,512)` is one `rep stosq`, ecx 0x40 (= bitmap_size(MAXSECTORS) qwords)
  into one rip `lea`, then `bitmap_set(drawingstate, dacursectnum)` names the same address. Nine source uses (:1827,
  :1832, :1917, :1987, :1991, :2000, :2004, :2012, :2030), four in the YAX block, so five without TROR. A span rule
  that used a byte-search membership test drops .bss (SizeOfRawData 0) and always refuses; the span is checked
  against the containing SECTION.
- E4L1 sector 34 has wallptr 182, wallnum 2. A discovery rule that rejected it cost the sweep the other 512 of 546
  sectors; E3L4 loses 23%.
- sectorstate_rva: both counts lie within a hundred bytes of the `sector` pointer; mapextent once anchored a search on it.

## Shape rules for the located arrays
- prsectors[] slot: one `mov r64,[base+idx*8]` + test + je in polymer_updatesector's head (:2624-2626) and one rip-relative
  qword load into its base; the slot sits in a non-executable image section with room for MAXSECTORS pointers.
- The sector tiling validator closes wall loops within wallnum steps via wall[].point2, requires wallnum >= 3 (a zeroed
  region at index 0 dies), and scans at most EK_SECTOR_SCAN_DEPTH (256).
- With domasks TRUE the sweep costs the look of masked floors in swept sectors: GL_BLEND and GL_ALPHA_TEST are on only
  inside polymer_drawmasks.

## Backface culling
polymer_glinit sets glCullFace(GL_BACK), glFrontFace(GL_CCW) and enables GL_CULL_FACE (polymer.cpp:967-971);
engine.cpp:9561 calls it above polymer_drawrooms and the external arm (:1228-1243) runs inside it. engine.cpp:9563
buildgl_setDisabled(GL_CULL_FACE) is the last GL state change before `polymost2d = 0`. The set of sectors was never
the cause of missing faces; the engine's external arm draws a strict superset and looks right.

## Masked floors and ceilings (domasks)
- With domasks FALSE a masked floor/ceiling is appended to cursectormasks (polymer.cpp:1859; Xmalloc'd for numsectors,
  :1838; appends :3094, :3135, unbounded). GL_BLEND is on only inside polymer_drawmasks (:1274-1275).
- polymer_displayrooms recurses for mirrors (:2129) over one static bitmap (:711), memsets it (:1827), and the return
  restores only cursectormasks and count (:2132-2133). A tile 560 mirror in view therefore makes the bits the
  sub-walk's. A sound FALSE would need cursectormaskcount located and the appends budgeted.

## Method
Ghidra on the stock eduke32.exe r10669 against the EDuke32 source; sweep coverage measured on E4L1 and E3L4.
