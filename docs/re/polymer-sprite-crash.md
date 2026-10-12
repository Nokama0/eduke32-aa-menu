# The Polymer sprite crash on explosive barrels

Not a menu bug. An EDuke32 Polymer crash, reported upstream by others and never
fixed. The faulting code and the violated invariant are proven; the code that
corrupts the memory is not identified. The investigation is parked.

Method: WinDbg on the crash dumps, Ghidra on the stock `eduke32.exe` (r10669),
the engine source.

**Read the "Refuted" section before forming a theory.** Four explanations were
checked and all four are killed by measurement.

---

## The symptom

Shooting a group of explosive barrels drops the frame rate hard and then kills
the game. Reproduced three times. With full dumps enabled the process does not close -- it parks in Windows Error
Reporting while a ~9 GB dump is written, so it appears hung rather than crashed.

**Workaround: do not shoot the barrels.** Restarting does NOT help -- one
of the crashes happened shortly after a fresh start, which rules out anything
that accumulates over a session.

## Already reported upstream, unfixed

https://forums.duke4.net/topic/12084-tiberius-station-explosive-barrel-crash/

Same trigger, same renderer. Bisected there: good in **R9782**, broken from
**R9787**, with Danukem pointing at the upstream Polymer lighting commit
**e67e539** (Nov 2021), *"fix a few polymer lighting bugs, add interpolation to light
positions, etc"*. No fix was committed; the thread's only workaround is turning
Polymer off, which is not available here because the HRP requires Polymer.

A user there notes the attached `eduke32.log` files were useless for diagnosis.
Ours are too -- the engine writes nothing at all before dying.

---

## Proven

Each of these is re-derivable from the dump; none is inference.

1. **The failure is an access violation, not an abort.** The dump reports
   exception `0x40000015` FATAL_APP_EXIT, but that is EDuke32's own SEH handler
   calling `abort()` after catching the AV. The `.ecxr` context is the handler's,
   not the fault's.

2. **The faulting instruction**, recovered from the original exception context:

       eduke32+0x193002   movzx eax, word ptr [rbp+rdx*2]
       rbp = 0x009c009b_00000000    <- p->indices
       rdx = 0                     <- reading indices[0]
       rcx = 0x00000412_6923f6c0    <- the _prplane*

3. **The caller is `polymer_updatesprite`** (`polymer.cpp:3907`). Identified
   from the binary: that function references `"Updating sprite %i"`, which occurs
   exactly once in the source, at `polymer.cpp:3924`.

4. **The callee is `polymer_computeplane`** (`polymer.cpp:3714`), which
   `polymer_updatesprite` calls at `polymer.cpp:4139`. Confirmed by struct
   offsets -- `buffer` at +0x00, `vertcount` at +0x08, and the cross-product
   result written to +0x44 where `GLfloat plane[4]` lives -- and by the
   arithmetic being a plane-normal computation.

5. **The struct is a live, healthy sprite plane, not freed and not garbage.**
   `buffer` a valid pointer, `vertcount = 4`, `vbo = 0x2c74`,
   `mapvbo_vertoffset = -1`, a sane TBN matrix and a real plane equation
   `(0, 0, -1, -11648)`.

6. **`indices` must be NULL on a sprite plane.** `_prsprite` is `Xcalloc`'d
   (`polymer.cpp:3934`) and nothing in the file ever assigns a sprite plane's
   `indices` -- the only assignments are `s->floor.indices` / `s->ceil.indices`
   for sectors (`:3003-3006`) and model/bucket structs. Confirmed empirically:
   three healthy sibling sprite planes all hold zeros at +0xD0..+0xDF.

7. **The crashed plane held light-shaped data there instead.** Read as int16
   from +0xD4: `155, 156, 157, 159, 160` -- a sorted run with one gap, the shape
   `polymer_addplanelight` produces.

**Conclusion that survives: something corrupts the `indices` field of a live
sprite plane, writing data that looks like light indices.** The crash is
downstream of that, and is not itself the bug.

---

## Refuted -- do not revisit

Four theories, each stated confidently, each killed by measurement.

1. **`inthash_add(): table full!`** -- the Polymer shader cache
   (`prprogramtable`, 341 slots, 19-bit key, no eviction). Plausible, and the
   that exact message box was seen on one crash, but `inthash_add`
   is `FUN_14016a500` and appears **nowhere** on the stack. The two failures are
   unrelated; that message box is still unexplained.

2. **Out-of-bounds `tspr->owner`.** `polymer_updatesprite:3928` checks only
   `owner < 0`, while `owner` is `int16_t` and `prsprites[]` has `MAXSPRITES`
   (16384) entries -- and the engine's own convention allows `owner` up to
   `MAXSPRITES+MAXUNIQHUDID` (`mdsprite.cpp:861-862` guards exactly that).
   A real latent bug, but **not this one**: the faulting pointer is stored at
   `prsprites` base + 0x9B28, index **4965**, comfortably in range.

3. **Use-after-free of the `_prsprite`.** Killed by point 5 above, and the only
   free path (`:2538-2540`) uses `DO_FREE_AND_NULL`, so a stale non-NULL slot
   cannot arise.

4. **Overflow of this plane's own light array via `polymer.cpp:1611`.** The
   crashed plane's `lightcount` is `0x1F` = 31, exactly the legitimate cap, and
   so are its three healthy neighbours'. An overflow there would have overwritten
   the counter with a light index. The neighbouring block at -0x140 has
   `lightcount = 0`, so it did not overflow either.

---

## Two genuine latent bugs found on the way

Worth reporting on their own merits, whether or not they cause this crash.

- **`polymer.cpp:1611`** writes `s->plane.lights[s->plane.lightcount++] = i`
  with **no bound against `PR_MAXPLANELIGHTS`** (32), while the sibling writer
  `polymer_addplanelight` (`:5850`) caps at 31. `lights[]` occupies +0xE0..+0x11F
  and `lightcount` sits at +0x120, so `lights[32]` **aliases the counter itself**
  -- an overflow overwrites the count with a light index, after which every
  further write lands at an arbitrary offset.

- **`polymer_deleteplanelight:5906`** does
  `Bmemmove(&lights[i], &lights[i+1], sizeof(int16_t) * (lightcount - i))`.
  That copies one element too many; it should be `lightcount - i - 1`.

---

## What would close it

The open question is only: *what writes the `indices` field of a live sprite
plane?* A dump cannot answer it -- it shows the damage, not the writer.

- A **data breakpoint** on `&s->plane.indices` for a sprite that is about to be
  hit, or a **page-heap / ASAN build** of EDuke32. Both mean compiling the
  engine, which is ruled out for distribution but possible as a one-off local
  diagnosis.
- Failing that, the report is the "Proven" section plus the two latent bugs,
  with the writer stated as unidentified.

---

## Artifacts and tools

- Dumps: two minidumps and one full dump (9.46 GB, the only one worth keeping;
  the minidumps add nothing it does not have).
- Ghidra project: the imported, auto-analysed `eduke32.exe` of r10669.
  Re-run a script against it without re-analysing:

      analyzeHeadless.bat <proj> eduke32 -process eduke32.exe -noanalysis         -scriptPath <proj> -postScript <Script>.java

  Scripts: `ResolveCrash.java` (stack RVA to function), `IdentifyFault.java`
  (decompile plus referenced strings).
  **Ghidra 12 will not run `.py` headless without PyGhidra -- use Java.**
- Useful WinDbg commands, in order:

      .ecxr; k 16                      # the handler's stack
      .cxr <KiUserExceptionDispatch Child-SP>   # the REAL fault context
      dq <plane> L1c                   # the struct
      dq 00007ff6e8c43e00 L1           # -> prsprites base
      s -q poi(00007ff6e8c43e00) L?0x4200 <plane>   # which index holds it

## Crash dumps and disk

Full crash dumps are enabled for `eduke32.exe` and each one is about 9.5 GB,
with `DumpCount = 3`: up to ~28 GB on the system drive. To turn them off,
elevated:

    Remove-Item -Path 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\eduke32.exe' -Recurse -Force

## Open questions

- What writes the `indices` field of a live sprite plane?
- What caused the `inthash_add(): table full!` message box seen on one crash?
