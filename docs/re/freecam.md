# Free camera: draw path, locates, transports, sky, fog, teleport

The free camera substitutes the six `ud.camera*` values for one `renderDrawRoomsQ16` call per frame and restores them
after it. This note holds what the engine does around that call and how each address is found. Build: eduke32.exe
r10669. Every rva below is a search result of that build, never an input: the mod re-derives each at run time.

## Draw path
- G_DrawRooms rebuilds all six `ud.camera*` before drawing (game.cpp:1001-1110) and hands them to the draw at :1142.
  Not every path does: game.cpp:784 returns for a dedicated server, :800 returns on the full automap, the help screen
  or cursectnum -1 (one frame each), and the `ud.camerasprite >= 0` branch (:840-876) writes only CAMERA(sect) (:819)
  and CAMERA(q16ang) (:846), never CAMERA(pos) or CAMERA(q16horiz): the camera's last position and horiz survive many
  frames there.
- G_DrawRooms clamps camera z twice before rendering: game.cpp:1074-1077 into the player's truecz/truefz band, then
  :1097 and :1107 into the camera sector's slope band from getzsofslope on live geometry. Neither clamp is reachable
  from anything the mod may write.
- `renderDrawRoomsQ16` has polymer_drawrooms inlined (rva 0x015622B..). Its head decodes as engine.cpp:9434-9443:
  0x015622B `mov dword [rip+..],0` is beforedrawrooms = 0 (:9434); 0x0156240/0x0156270/0x015628F store globalposx/y/z
  (set_globalpos, :9440) plus three floats; 0x01562A2 `call 0x155DC0` (ecx=r9d) is set_globalang(daang) (:9441);
  0x01562B5 `mov [global100horiz],edi` is :9443. Further in: 0x01564FB is engine.cpp:9497's `totalclocklock =
  totalclock`, 0x015654D is :9502's 512-byte gotsector zero, 0x0157E9C is :9480's `* 150`, 0x015950B is
  polymer.cpp:1232-1242's external arm. G_DrawRooms calls it at 0x003203D (game.cpp:1142). So polymer.cpp:2484
  polymer_animatesprites and :3079-3091 (`globalposz <= floorZ`) see the camera's globals; renderDrawMasks reads them too.
- yax_drawrooms (game.cpp:1143) returns at once under REND_POLYMER (engine.cpp:980). The REND_POLYMER branch is
  engine.cpp:9548; other renderers fall through to polymost_drawrooms() (engine.cpp:9570+), which takes no arguments
  and reads those globals.
- The hooked function is `renderDrawRoomsQ16`, not polymer_drawrooms, and its render-mode dispatch is inside it.
  There is no `rendmode` cvar (gamevars.cpp:1846 is a gamevar only); the renderer can change from the video menu
  (menus.cpp:4011). Being reached therefore says the world is drawn, not which renderer drew it.
- G_DrawRooms is the target of 7 `call rel32` sites in 6 functions; renderDrawRoomsQ16 of 10 in 6. G_DrawRooms
  itself: anchor game.cpp:803, the LOG_F "Unable to capture screenshot for savegame because player cursectnum is
  -1!", occurs ONCE at rva 0x04FA588 in .rdata (the only other "Unable to capture screenshot" literal is
  game.cpp:1159's, ending "EVENT_DISPLAYROOMS returned %d!", in a different function). Exactly one .text position
  holds the displacement bytes reaching it and passes the lea shape test: rva 0x0032288 `4C 8D 0D F9 82 4C 00` =
  `lea r9,[rip+0x4C82F9]`; the next six bytes `41 B8 23 03 00 00` are `mov r8d,0x323` = 803, the LOG_F's __LINE__.
  The function spans 0x0031A20..0x003385E and is the target of seven `call rel32` sites in six functions (the source
  has seven calls: demo.cpp:819 and :839 in one function, game.cpp x3, menus.cpp, premap.cpp).
- Which drawrooms call is the player's: the bracketed drawrooms is reached from TEN `call rel32` sites in six
  functions (mirrors, three TROR passes, a viewscreen sprite, the player's view). The return address excludes five
  owners but not G_DrawRooms' second call (game.cpp:870, viewscreen sprite); the arguments exclude that one but not
  another owner handed the same six values. `freecam_rva_in_span` is half-open (RUNTIME_FUNCTION EndAddress is past
  the function) and an empty span matches nothing.
- Draw-seen verdict is an observation, not a read: reaching the hooked drawrooms measures that a frame is drawn there,
  not that the renderer is Polymer, so the verdict must be revocable when frames stop. Otherwise a player who
  switches renderer keeps a dead camera key and an input lockout. 300 frames is 2.5-5 s at 60-120 fps. A zero enters the rule when
  the detour ran; there is no memory of having revoked. Entry, exit and the dead-man's switch are separate rules (one
  symmetric rule once left the input lockout on forever).
- Draw reachability: newowner >= 0 (game.cpp:999) draws from the camera sprite and ignores every bracket field;
  over_shoulder_on (game.cpp:1025-1040) drops the camera 3072 and runs G_DoThirdPerson, moving position and sector,
  and its clip trace on `fix16_to_int(CAMERA(q16horiz))` would see the unclamped horiz (~3652 at PITCH_MAX). The
  bracket writes ud.camera* at the drawrooms and no player field at all, so these two cases are REFUSALS.

## Renderer verdict
The detour sits on renderDrawRoomsQ16, whose REND_POLYMER test is inside it, so being reached proves the world is drawn,
not that Polymer draws it: on Polymost the verdict reads available while the Polymer-only parts (sky rule, whole-map
sweep, fog substitution) have nothing behind them. G_DrawRooms returns before any draw on the full automap and the help
screen, so a frame counter for the verdict must ask whether a frame was drawn. The sweep's frustum test uses the same pose:
the eye is the apex of every side plane.

## Audio listener and fog
- The 3D audio listener reads CAMERA(pos), CAMERA(sect), CAMERA(q16ang) on the tic path (S_Update sounds.cpp:1112-1114,
  S_PlaySound3D via S_CalcDistAndAng :872). S_Update falls back to the player at `ud.overhead_on == 2` and to the sprite
  at `ud.camerasprite >= 0`.
- Remove fog works under REND_POLYMER, where the one distance-dependent darkening term is scaled by globalvisibility.
  Hightile (HRP): polymer.cpp:5188 adds PR_BIT_FOG (mixes toward gl_Fog.color; polymost.cpp:1300-1327 fills it and
  `globalvisibility > 0` chooses a real fog range against the engine's no-fog case, fog end -GL_FOG_MAX). Art tiles:
  polymer.cpp:5345 sets the `visibility` uniform to globalvisibility/262144 * material visibility and the :340 shader
  adds length(horizDistance) * visibility to the shade. Zeroing both removes the terms while each surface keeps its
  own shade.
- The field substituted is g_visibility, not globalvisibility: engine.cpp:9460-9482 recomputes globalvisibility at the
  head of renderDrawRoomsQ16 (a value written before the call would be overwritten). g_visibility is written every
  frame at game.cpp:817 from the player's visibility (build.h:825, zero-initialised global).
- Existing fog bugs in the engine: polymer.cpp:5359 has an operator-precedence bug (`>` binds tighter than `?:` in the
  uniform_visibility ternary), polymer.cpp:5461 hightile uniform_linearFog depends alike; default r_usenewshading is
  4 (polymost.cpp:48). The `(g_visibility<<2) * 150 / (230 * 35)` constant is "found out by experiment" per the source
  comment.
- Locate g_visibility: (1) trace from engine.cpp:9480's `* 150` (a literal no other instruction there carries): imul,
  shl, dword read, pointer load, each unique in its window; the refptr load may repeat (mingw reaches r_usenewshading
  and g_visibility through .refptr slots into the SAME register two instructions apart, engine.cpp:9477 and :9480), so
  count, take the last, refuse on zero; (2) witnesses from source: engine.cpp:9460-9482 is a three-way renderer switch
  and every arm reads g_visibility (loaded more than once), and game.cpp:817 writes it exactly once inside G_DrawRooms
  (a direct rip-relative store, not a refptr load); (3) span. On r10669: `imul r32,r32,150` at 0x0157E9C (two in all
  of .text), `shl eax,2` 0x0157E90, `mov eax,[rax]` 0x0157E88, refptr loads into rax at 0x0157E75 (r_usenewshading)
  and 0x0157E7E (g_visibility, nearest wins); slot rva 0x05A2420 holds ImageBase+0x00C222E0 (.bss), loaded three times
  inside drawrooms (one per arm of the switch) and the target of exactly one direct rip-relative dword store in
  G_DrawRooms (game.cpp:817) out of nine distinct store targets.

## Pose, previous-pose pair, tic clock
- Rates are for this game's 30 Hz tic (World Tour's is 26): FREECAM_BASE_SPEED = BUILD_UNITS_PER_METRE/4 = 128
  units/tic = 3840 units/s = 7.5 m/s (15 with fast). Build z is 16x x/y. PHEIGHT 38<<8 = 9728 is the player SPRITE's z
  offset (premap.cpp:664), NOT the eye height (floorzoffset = 40<<8 = 10240, premap.cpp:660). FREECAM_PITCH_MAX 500
  units is about 87.9 degrees. The camera's horiz never passes P_UpdateAngles' -99..299 clamp (player.cpp:3179, inside
  the :3051 function P_GetInput calls), as it is written for one displayrooms call and restored; aimsolve.h's
  EKM_SHOT_HORIZ_MIN/MAX rests on the same fact.
- The camera steps on the 30 Hz tic, so at 240 fps its pose changes every eighth frame. G_DrawRooms interpolates the
  player between opos/oq16ang and pos/q16ang by smoothRatio (game.cpp:1001-1029); the previous-pose pair lets the
  camera do the same. A transport collapses only the POINT pair (the turn pair is still real mouse movement).
- Lerp: yaw takes the short arc `((cur - prev + HALF) & (UNITS-1)) - HALF` (the exact half turn folds to -HALF); horiz
  is linear (it does not wrap, and is what the renderer consumes). Use 64-bit: z can move tens of thousands of units a
  tic (z is 16x, speed to 500%) and times 65536 overflows int. smoothRatio range is 0..65536 (baselayer.h:364-375,
  game.cpp:1001).
- Pose seqlock: five ints cannot be read atomically; an odd counter means a write is in flight; camera.cpp owns the
  counter and barriers.
- Tic clock: calc_smoothratio clamps to exactly 65536 when a frame takes longer than a tic (baselayer.h:364-375), so
  at <= 30 fps a rule watching for a decrease never sees one and the camera stops stepping. ototalclock (game.cpp:7122
  advances it by TICSPERFRAME per processed tic; calc_smoothratio's numerator is totalclock - ototalclock) is read
  directly. ANY change is a tic, a decrease included (reset on level entry, savegame.cpp:704 on a load); no previous
  reading is not a tic; several tics between two frames step once (the counter can jump ~100 across a level load).
  Tic clock: ClockTicks has wholeTicks first (clockticks.hpp:136-139); only the first four bytes are read. World Tour
  reads two engine clocks instead.
- Locate rules for the clock: (1) shape: `totalclocklock = totalclock` (engine.cpp:9497) is a ClockTicks assignment
  ending `ticksS32 &= 0xFFFFFFFFFFFF0000` (clockticks.hpp:155) = GCC `xor ax, ax`; exactly ONE in the located
  drawrooms, its value arrives by exactly ONE `mov rax,[reg+8]` (ticksS32 is the second member) from exactly ONE
  rip-relative refptr load naming totalclock; (2) witness: G_DrawRooms carries the same statement (game.cpp:786) and
  must give the same slot; (3) ototalclock is found by what the game puts INTO totalclock in eight places
  (game.cpp:4953, :4979, :4996, :5013, :5130, :5145, menus.cpp:5538, screens.cpp:1023; demo.cpp:76 is not a copy and
  not in the list), scanning only the owners of the G_DrawRooms call sites; the source of every copy must be one slot
  (looking at what totalclock is copied into would also find totalclocklock); (4) span: the slot is in a
  non-executable section and the address it holds leaves room for a 16-byte ClockTicks in the image.
- On r10669: `xor ax,ax` at 0x01564FB in drawrooms, `mov rax,[rax+8]` at 0x01564F4, refptr load at 0x01564DF naming
  slot 0x05A4580 (holds ImageBase+0x071B0F0 in .bss), read twice inside G_DrawRooms (same statement at game.cpp:786);
  six ClockTicks copies over the six owners all name one source slot at rva 0x05A39F0 (ImageBase+0x0B52BF0) = ototalclock.

## Sector pick and sky
- No clipping, so past a boundary wall updatesector answers none; refusing there drew the player's view (read as
  collision and being teleported back). An index outside 0..maxSectors-1 is dangerous (the draw walks the sector
  graph); a stale valid one is what the engine does. World Tour's camera relies on that.
- Band pick is bounded to the last sector and one ring of wall neighbours (a second ring costs O(ring squared) reads);
  the case it buys is flying up off a rooftop into the open street sector's tall parallaxed band. getzsofslopeptr
  indexes wall[sec->wallptr] unchecked and follows wall[wal->point2] (int16, may be stale; lands within +-1 MB of
  wall[] in a 21 MB .bss).
- Sky: polymer_drawrooms' external-view arm (polymer.cpp:1214-1251) fires when the sector is out of range or z lies
  outside the band (`daposz > cursectflorz || daposz < cursectceilz`, :1215-1217, from getzsofslope :1211), and RETURNS
  at :1250 before polymer_displayrooms (:1254) where polymer_drawsky is called (:1845): whole map, no sky.
- The band pick prefers a reachable sector whose band contains z, applied every frame (camera.cpp lists the
  engine-resolved sector first; zero cost in band). `exact` bands (engine getzsofslopeptr via camerascan.cpp, or flat
  with no slope bit, engine.cpp:14292) beat guessed ones. A guessed candidate zero REFUSES the substitution (monotone:
  substituting on a false miss seeds a sector the camera is not in); an EXACT candidate zero that misses is safe.
  game.cpp:1080-1082 walks CAMERA(sect) unbounded, so out-of-range is never chosen.
- Widening: polymer.cpp:1211 fills its two band locals from ONE getzsofslopeptr call in the bracketed function (a grep
  of polymer.cpp for cursectceilz/cursectflorz finds four lines: declaration, call, two comparisons, verbose log);
  camera.cpp redirects that call site to a thunk. freecam_band_widen extends only the plane z falls outside of and
  never moves inward (an already containing band returns byte for byte). Exactly one `call rel32` in the bracket may
  reach getzsofslopeptr. The redirect arms on the whole-map switch plus the sweep (the sweep pays for the sky; out of
  bounds the portal walk reaches little). It widens only while the substituted draw is in flight. Sprites used to
  gate it: forcing the normal arm hands sprite scanning to the portal walk (only sectors it walks); that was ruled too
  high a price and reversed, since a missing distant actor was never reported against black sky.
- Disproved sky options: calling the original twice (polymer_glinit glClear :941), drawing the sky ourselves
  (polymer_drawsky is static, polymer.h:423, one call site, inlined into polymer_displayrooms), hunting a far sloped
  sector. polymer_drawsector's getzsofslopeptr call is polymer.cpp:3080.
- Sky side effect of the sweep: polymer_drawsector calls polymer_getsky when !prdidsky (:3066-3069, :3115-3118), getsky
  writes cursky/curskypal/curskyshade (:4192-4194), prdidsky resets only in displayrooms (:1852).

## getzsofslopeptr locate
- build.h:1417 declaration, build.h:1434 getzsofslope (static FORCE_INLINE), engine.cpp:14288 definition,
  polymer.cpp:1211 caller. On r10669: 86 `call rel32` sites inside 0x01561B0..0x0159ED3 reach 45 distinct functions,
  exactly ONE passes the shape: rva 0x0165070, also the only one of the 8554 exception-directory rows (End > Begin, of
  8561) that passes it. Its body opens `mov eax,[rcx+4] / mov [r9],eax / mov eax,[rcx+8] / mov [rdi],eax / movzx
  esi,[rcx+0xC] / movzx ebx,[rcx+0xE] / or eax,ebx / test al,2 / je` (engine.cpp:14290-14292). 42 sites in 16
  functions call it. ceilingz/floorz are the z at the sector's first wall.

## Polymer sweep arrays (anchors found by the camera)
The whole-map sweep is described in [polymer-sweep.md](polymer-sweep.md); the locates as the camera finds them:
- The four sweep functions: updatesector 0x0193640, drawsector 0x01926A0, updatewall 0x0195090, drawwall 0x0192BE0; one
  call site each in polymer_drawrooms at 0x015950B, 0x0159515, 0x0159597, 0x01595A6 (the loops decode as
  polymer.cpp:1229-1244 with polymer_scansprites inlined between). Eight anchors each occur once with one
  rip-relative lea. Sweep source: polymer.cpp:1232-1242; the portal walk drew sectors at :1859/:2058 and walls at
  :1978/:2063.
- prsectors[]: candidate load at rva 0x01936CF (`mov r11,[rdi+rax*8]`, `test r11,r11`, `je`), refptr load into rdi at
  0x01936B5, slot at rva 0x05A3DF0 (.rdata, one DIR64 relocation) holds ImageBase+0x13E9D00 (.bss, 0x8000 bytes of
  image after it). The slot is rip-referenced three times inside polymer_updatesector, once in drawsector, once in
  updatewall, never in drawwall. The shape is unique at 0x100/0x200/0x400 from the entry but four exist in the whole
  function.
- prwalls[]: one indexed load at rva 0x0192C35 (`mov r13,[r9+rax*8]`, feeding `movzx 0x39C(r13)` = w->underover) with
  one refptr load into r9 at 0x0192C0A, slot rva 0x05A3E10 (read 0 times in drawsector, once in updatewall, once in
  drawwall), holds ImageBase+0x10E62C0 leaving 0x9BFD40 bytes for an array needing 0x20000. polymer_drawwall is 0x397
  bytes; updatewall 0x1C33 and four arrays.
- drawingstate: candidate callee rva 0x0197BA0 (drawsector x2, drawwall x2); `rep stosq` at 0x0197C93 with one `mov
  ecx,0x40` and one `lea rdi,[rip]` in the 0x100 before it, naming rva 0x14B0BE0 in .bss; one more lea naming it 0xD
  bytes after (polymer.cpp:1832's bitmap_set); nine lea sites in the function; a raw .text scan finds the same nine.
  polymer_culllight reaches the array through a refptr slot (rva 0x059E1B0 holds ImageBase+0x14B0BE0), loaded twice at
  0x019EDF0..0x019F6C1, and memsets it too (:6034) but its callers run before the walk; polymer_updatelights (:1122)
  and polymer_prepareshadows (:1131) run ahead of the walk (:1254).
- Mirror recursion: polymer_displayrooms clears drawingstate at :1827 and recurses at :2129 over one static (:711); the
  return restores cursectormasks and count (:2132-2133). The mirror pass polymer.cpp:2072-2129 (overpicnum 560, cstat&32,
  depth 0, :1905) calls displayrooms again, which memsets the bitmap and refills it from the mirror's sector.
- Nothing else is bucketed: polymer_emptybuckets :2199, prcanbucket back to 0 (:2245); scansprites appends under a
  MAXSPRITESONSCREEN test. domasks TRUE: GL_BLEND/GL_ALPHA_TEST are on only inside polymer_drawmasks (:1274-1275,
  :1305-1306), off at glinit (:949-950); glinit sets GL_LEQUAL (:947).
- scansprites window 0x015951A..0x0159597: `imul rdx,rcx,0x2C` at 0x0159543, `add rdx,r8` at 0x0159547, pointer loads at
  0x0159527 (through rdi) and 0x015952A (through rsi = sprite base r8), `movsxd r10,[r11]` at 0x0159556 and `cmp
  r10d,0xFFF` at 0x0159559; hoisted slot loads 0x01594EC (slot 0x05A45C0 -> tsprite), 0x01594F3 (slot 0x05A43F0 ->
  sprite, rva 0x0AEE360), 0x0159533 (slot 0x05A4440 -> spritesortcnt). The back window answers one apiece at 0x30,
  0x40, 0x80, 0x100 and 0x200 bytes back.
- The swept sprite array is rva 0x127C640 (the image walk names `sprite`; a content scan once named the CON VM's
  vm.pSprite at rva 0x0DDC150, same value). Visibility: WALLS_ARE_CONSISTENT makes a neighbour's endpoints the same
  two points (engine.cpp:1296-1297); polymer.cpp:3843-3844 loads projection then multiplies modelview.

## Transports (G_MoveTransports, actors.cpp:3908, paired SE 7)
- None of World Tour's four z constants survived. ZBAND 6144 is the airborne carry window (actors.cpp:3993 klabs test,
  :4001-4004; a rising camera lands ABOVE the far effector, others below). EMERGE 1080 is P_Emerge's band below the
  ceiling (actors.cpp:3843). P_Submerge tests the floor plane (:3814) with no constant.
- Engine facts: landing `cursectnum = sprite[OW].sectnum` :4011, hold test :3945, onfloor :3924, water tests
  :3814/:3843; SE 7 t_data[4] = (sector.floorz == sprite.z) at game.cpp:2930; one-way SE 23 (owner == self,
  game.cpp:2928) is skipped at actors.cpp:3916; P_ProcessInput re-arms the hold to 2 (player.cpp:5233-5234). Hold
  values: 13 down for a pad, -2 up for water.
- Hold: transporter_hold = -2 for a paired surface (actors.cpp:3878). A NEGATIVE hold counts up (player.cpp:5236-5237) in
  the else branch of the positive one that holds the top-up (:5233-5234), so only the ONFLOORZ teleporter's 13 tics
  (actors.cpp:3972) enter it. Carrying the sign IS the scoping (applying the top-up to a water pair made it
  one-way). The airborne teleporter gets no hold.
- The visible teleporter (SE 7 on its sector's floor, lotag 0) is designed, not ported: World Tour (freecam.h:366) and
  Megaton (freecam.h:365) both refuse it. The engine's gate is on_ground with no jetpack (actors.cpp:3947),
  meaningless for a camera, so the position on_ground stands for is ported: viewpoint at most floorzoffset (10240)
  above the floor (player.cpp:5409).
- FreecamTransport: lotag is the effector's SECTOR's (0 none, 1 above water, 2 underwater, game.h:535), -1 unreadable;
  tempData4 is t_data[4], for SE 7 the ONFLOORZ flag (game.cpp:2930, read back as T5, actors.cpp:3924).
- freecam_transport_jump has four cases disjoint in (onFloor, lotag): onFloor == 0 first (not lotag-gated,
  actors.cpp:3993), then the floor teleporter (tested first by the engine, :3947), then the water cases (:4018-4023).
  The direction term is required: each water landing is the paired plane EXACTLY (actors.cpp:3829, :3859; no landing
  offset, unlike the fork whose commented-out forms sit beside them) and so inside the other half's trigger.
  P_Submerge wants crouch or a fall faster than 2048 (actors.cpp:3815), P_Emerge vel.z <= 0 (:3843); a camera's own
  downward step stands for the dive. The fall-speed arm is unusable: at 100% the step is exactly 2048 with down alone
  (fails > 2048) and 4080 with pitched-down forward.
- Z windows are tested against the SEGMENT travelled this tic, not the end point (a fast camera steps over both: up to
  40960 z/tic against an airborne band 12288 wide and a pad window 10240). Both reduce to the point form at dz 0 and
  keep strict edges, since a landing sits ON the far boundary and freecam_pose_carry_pos collapses the previous point
  onto it.
- All four arms ADD the pair separation; the engine's floor teleporter ASSIGNS the far effector's position
  (actors.cpp:3975); adding is dead centre to dead centre. The landing is the far pad's window edge, 512 higher than
  the engine's set-down (actors.cpp:3975-3976). The z edge refuses only the landing POINT (drift of one unit within
  13 tics re-enters the far window), so the engine's on_warping_sector top-up is ported: the engine sets it when the
  sprite shares a sector with a PAIRED effector (actors.cpp:3943, after the owner == self skip :3916), and the
  camera's reading is deliberately that broad (the narrower "floor arm would fire" is false at the landing edge).
  Breadth is safe because the hold's sign scopes it. Unpaired (owner == self) effectors are skipped as the engine does.
- freecam_transport_sector asks two questions: is the point in ANY sector (updatesector; a no means nothing fires;
  freecam_sector_pick is deliberately sticky for the sky, and searching that stale sector carried a camera nowhere
  near the water at the pair's offset) and which sector. The resolve is seeded from s_camSect: updatesectorneighbor
  returns the seed untouched when getsectordist is 0 (engine.cpp:13517-13520) and updatesector_compat returns on
  inside_p (engine.cpp:13281-13282), so with stacked sectors sharing a 2D footprint the seeded query returns the
  tracked sector; a different answer means the point left it, hence refusal.
- Both bounds are checked (camera.cpp indexes headspritesect[]); a -1 refusal is silent (flying outside the map is
  ordinary). World Tour has no such gate (it searches its sticky seed sector), so the out-of-bounds carry is live there.
- A call that fills an out-parameter must not read it in the same argument list: the first port did, MSVC evaluated the
  read first, the sector searched was -1 every tic and the transport was dead both ways, silently.

## Teleport (from the camera)
- Ported from Megaton's freecam with `bool ok` -> `int ok`. EK_TP_PHEIGHT 10240 is Megaton's 0x2800 exactly: eye
  height is floorzoffset (premap.cpp:660, read into floorZOffset player.cpp:5292, written `pos.z = floorZ -
  floorZOffset` :5414); 9728 refuses rooms the player fits in and lands arrivals 512 low.
- EK_TP_CEIL_MARGIN 1024 is Megaton's PHEIGHT_MARGIN: the standing clamp is P_ClampZ's ceilZ + PMINHEIGHT
  (player.cpp:5005-5006, PMINHEIGHT 1024, player.h:44). player.cpp:4939-4940 is inside P_DoJetpack, a different clamp
  4.5x the margin (4608).
- Marker column: half a metre across (224 units; Build xy unit is 1/512 m, z unit 1/8192, 0x3000 = 1.5 m), under the
  2 m footprint Megaton found too big. Marker corners: one turn around the square (a zig-zag order draws a bowtie),
  widened and saturated.
- The teleport is a way OUT of the camera, so its own switch is deliberately absent from the fire rule. Hitscan
  direction: vz from pitch alone (the camera has no horizoff), computed with ekm_sintable (shotray.h:57) rather than a
  passed pointer; multiplies, not shifts (shotray.h:95-99). Stage one steps back along the ray for a wall or sprite hit;
  stage two refuses tight spaces rather than clamping, lands the EYE at florz - eyeHeight (Build z grows downward;
  never write florz, player.cpp:5414), and tests the gap florz - ceilz widened to 64 bit. Notice visibility uses
  unsigned subtraction (49.7-day tick wrap).
- EK_TP_NO_FLOOR: ek_updatesector resolved the sector but cam_band_fill fell back to its flat pair on a sloped sector.
- A refusal's floor/ceiling band and reason are logged per key press under Debug log.

## Input, bots, screenshot
- Bots: with the panel shut and ESP on, overlay.cpp's early-out did not fire and aim_frame/tbot_frame ran from the
  PLAYER's position during flight; screenshot mode stands them down only when on AND the panel shut.
- Input ownership: release_held_keys forges WM_KEYUP for keys GetKeyboardState reports down (physical state); the
  release is owed once, on the edge where the mod takes the keyboard (panel or camera), never on a swap between owners.
- Screenshot mode over this camera: [shot.md](shot.md).

## Method
Ghidra on the stock eduke32.exe r10669 (shape searches, exception-directory function bounds, call-site counts) against
the open EDuke32 source; in-game checks of each shipped behaviour.
