# Build the ekmod proxy DLL (x64) and report its hash.
# EDuke32 is x64, so this is x64 -- the 32-bit toolchain the Megaton and World
# Tour mods use cannot produce anything loadable here.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $root '..\..')
$build = Join-Path $repo 'build'
$obj = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
        -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio not found' }
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found at $cl" }

# The Windows SDK headers and libs the compiler needs. Highest complete version,
# not the last folder: a Driver Kit adds non-version folders such as Include\wdf,
# and CI images carry several SDKs.
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVer = Get-ChildItem "$sdkRoot\Include" -Directory -ErrorAction SilentlyContinue |
    Where-Object { $v = $null; [version]::TryParse($_.Name, [ref]$v) -and
                   (Test-Path "$($_.FullName)\um\windows.h") -and (Test-Path "$($_.FullName)\ucrt\stdio.h") } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty Name
if (-not $sdkVer) { throw "No complete Windows SDK (um\windows.h and ucrt\stdio.h) under $sdkRoot\Include" }
Write-Host "Windows SDK $sdkVer"

$env:INCLUDE = @("$($msvc.FullName)\include", "$sdkRoot\Include\$sdkVer\ucrt",
                 "$sdkRoot\Include\$sdkVer\um", "$sdkRoot\Include\$sdkVer\shared") -join ';'
$env:LIB = @("$($msvc.FullName)\lib\x64", "$sdkRoot\Lib\$sdkVer\ucrt\x64",
             "$sdkRoot\Lib\$sdkVer\um\x64") -join ';'

# /MT so the DLL carries its own CRT: it loads into someone else's process and
# must not depend on a redistributable being present on a player's machine.
$common = @('/nologo', '/c', '/O2', '/EHsc', '/MT', '/DWIN32_LEAN_AND_MEAN')

# THE SUITE RUNS BEFORE THE DLL IS BUILT, and fails the build on any red check.
# Four of 2026-09-15's bugs were pure arithmetic, and every one was found by the
# player looking at a screen after a build, a deploy and a launch. A red line
# here costs a second.
#
# Forward slashes on purpose: cl.exe and PowerShell both accept them, and a
# backslash in a path here has already been turned into a tab once by the
# tooling that writes this file.
#
# THE TEST OBJECT GOES IN ITS OWN DIRECTORY: when the DLL link globbed $obj, a
# test object written there shipped the whole suite inside the DLL, silently.
$testObj = Join-Path $build 'obj-test'
New-Item -ItemType Directory -Force -Path $testObj | Out-Null
$testExe = Join-Path $build 'test_host.exe'
$testSrc = Join-Path $root 'tests/test_host.cpp'
# ui.cpp only: it is the five pure layout functions the suite pins. menu.cpp
# DRAWS, and the suite links no renderer -- that is the same read-not-run line
# every engine-facing file in this port sits on. clipboard.cpp is off this line
# for the same reason: it is Win32 calls against a live global the suite can
# neither set nor read, and the decisions around it live in campos.h, which is
# a header and is pinned. (2026-09-16)
$testUi = Join-Path $root 'ui/ui.cpp'
# font_data.cpp is pure data: tb_0354 packs its glyph boxes to prove the
# corner's scaled-text rect holds 200%. (2026-09-28)
$testFont = Join-Path $root 'ui/font_data.cpp'
if (Test-Path $testExe) { Remove-Item $testExe -Force }
# menu.h now pins menu_live_count (menu.cpp's call site, rank 30 wiring), and
# it reaches outside ui/ for tbot.h the same way the DLL build's own /I list
# lets it -- the test compile had no /I list because nothing it pinned needed
# one yet. Mirror the DLL build's two roots rather than inventing a third
# path scheme. (2026-09-17)
& $cl /nologo /O2 /W3 /WX /EHsc /MT /Zc:preprocessor /I"$root" /I"$root\ui" "/Fe:$testExe" "/Fo:$testObj/" $testSrc $testUi $testFont
if ($LASTEXITCODE -ne 0) { throw "test host failed to compile ($LASTEXITCODE)" }
# CHECK THE FILE, NOT THE EXIT CODE. On 2026-09-15 this build reported success
# with the suite never having run: cl wrote test_host.obj, exited 0, and the
# .exe was never created -- the security-software behaviour this repo has seen.
# PowerShell then invoked a path that did not exist, printed nothing, left
# $LASTEXITCODE at cl's own 0, and the DLL linked past an unrun suite. A gate
# that passes when its subject is absent is worse than no gate.
if (-not (Test-Path $testExe)) {
  throw "cl reported success but $testExe was never written -- THE SUITE DID NOT RUN. " +
        "Nothing here works around that; see CLAUDE.md's probe section."
}
# AND THE SUMMARY LINE IS THE PROOF, NOT THE EXIT CODE. The existence check
# above closes the failure that happened. It does not close the next one: if the
# file EXISTS but its execution is blocked, `& $testExe` raises a non-terminating
# native-launch error, prints nothing, and leaves $LASTEXITCODE at cl's own 0 --
# success reported, suite not run, again. Only the suite itself can print its
# `<N> passed, <M> failed` line, so requiring it makes this build incapable of
# reporting a pass it did not observe. Nothing here works around any blocking;
# probe executables are never run.
$suiteOut = & $testExe
$suiteCode = $LASTEXITCODE
$suiteOut | ForEach-Object { "  $_" }
$summary = @($suiteOut | Where-Object { $_ -match '^\s*\d+ passed, \d+ failed\s*$' })
if ($summary.Count -ne 1) {
  throw "the suite printed no '<N> passed, <M> failed' line -- THE SUITE DID NOT RUN. " +
        "Nothing here works around that; see CLAUDE.md's probe section."
}
if ($suiteCode -ne 0) { throw 'SUITE FAILED -- not building the DLL' }

Push-Location $obj
try {
  # VENDORED IMGUI AT /W0, DELIBERATELY. It is not our code and we do not edit
  # it, so its warnings are noise that would either bury ours or, under /WX,
  # block the build over someone else's style. Ours stays /W3 /WX.
  $imguiSrc = Get-ChildItem (Join-Path $root 'imgui') -Filter *.cpp | ForEach-Object { $_.FullName }
  & $cl @common /W0 /I"$root\imgui" @imguiSrc
  if ($LASTEXITCODE -ne 0) { throw "imgui compile failed ($LASTEXITCODE)" }

  $ourSrc = @('proxy.cpp', 'log.cpp', 'perf.cpp', 'iat.cpp', 'overlay.cpp', 'cvars.cpp', 'input.cpp', 'gamestate.cpp', 'udstate.cpp', 'project.cpp', 'esp.cpp', 'tiles.cpp', 'badguy.cpp', 'tilenames.cpp', 'gamevars.cpp', 'gamearrays.cpp', 'weapondata.cpp', 'firestops.cpp', 'constops.cpp', 'targets.cpp', 'config.cpp', 'gamecfg.cpp', 'hook.cpp', 'gamehooks.cpp', 'ui/canvas.cpp', 'ui/font_data.cpp', 'ui/fontgen.cpp', 'uibridge.cpp', 'ui/ui.cpp', 'ui/menu.cpp', 'playerstate.cpp', 'cansee.cpp', 'imagescan.cpp', 'hitscan.cpp', 'aim.cpp', 'localinput.cpp', 'tbot.cpp', 'weaponspeed.cpp', 'spiketotaltime.cpp', 'autosave.cpp', 'actorstate.cpp', 'wallstate.cpp', 'sectorstate.cpp', 'mapextent.cpp', 'tracer.cpp', 'camerascan.cpp', 'camera.cpp', 'maskscan.cpp', 'storeyflush.cpp', 'clipboard.cpp', 'shot.cpp', 'fov.cpp', 'worldquad.cpp', 'modelbounds.cpp', 'secretvol.cpp', 'secrettrig.cpp', 'secretpath.cpp', 'dmgnum.cpp', 'nocull.cpp', 'cutaway.cpp', 'footprint.cpp', 'cutpaint.cpp', 'coinmagnet.cpp', 'pickuprange.cpp', 'norecoil.cpp', 'console.cpp', 'skipintro.cpp') |
            ForEach-Object { Join-Path $root $_ }
  & $cl @common /W3 /WX /I"$root" /I"$root\ui" @ourSrc
  if ($LASTEXITCODE -ne 0) { throw "ekmod compile failed ($LASTEXITCODE)" }

  # Only the objects this run compiled: a leftover .obj in $obj (a deleted source, a test) must not ship.
  $linkObj = @($imguiSrc + $ourSrc | ForEach-Object {
    Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') })
  $missingObj = @($linkObj | Where-Object { -not (Test-Path $_) })
  if ($missingObj.Count -gt 0) { throw "compile left no object for: $($missingObj -join ', ')" }

  # glu32.lib is for footprint.cpp's CPU tessellator (2026-09-19). GLU32.dll is not a KnownDLL, so
  # /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32) keeps a copy beside the game from loading first.
  $def = Join-Path $root 'version.def'
  & $cl /nologo /LD /MT @linkObj `
        /link /DLL /DEF:"$def" /OUT:"$build\version.dll" /IMPLIB:"$obj\version.lib" /DEPENDENTLOADFLAG:0x800 `
        kernel32.lib user32.lib gdi32.lib opengl32.lib glu32.lib dwmapi.lib
  if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$out = Join-Path $build 'version.dll'
if (-not (Test-Path $out)) { throw "no output at $out" }
# A wildcard path, because -Include without one matches nothing.
Get-ChildItem -Path (Join-Path $build '*'), (Join-Path $obj '*') -Include *.exp,*.lib -File |
    Remove-Item -Force -EA SilentlyContinue

# EXPORT PARITY IS A BUILD GATE, NOT A POST-HOC CHECK. The first build of this
# DLL compiled and linked cleanly while exporting NOTHING -- extern "C" alone
# does not export -- and it would have loaded into the game as dead weight.
# A build that cannot be loaded is a failed build, so it fails here.
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
function Get-Exports($path) {
  $r = & $dumpbin /nologo /exports $path |
       Select-String -Pattern '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)' |
       ForEach-Object { ($_.Line.Trim() -split '\s+')[3] }
  return @($r)   # force an array: a single result must not collapse to a scalar
}
$mine = Get-Exports $out
$real = Get-Exports "$env:SystemRoot\System32\version.dll"
if ($real.Count -eq 0) { throw 'could not read the real version.dll exports' }
$missing = @($real | Where-Object { $mine -notcontains $_ })
if ($missing.Count -gt 0) {
  throw "proxy is missing $($missing.Count) export(s) the real version.dll has: $($missing -join ', ')"
}

# Every static import resolves from System32 only: the load config's DependentLoadFlags must read 0x800.
$dlf = & $dumpbin /nologo /loadconfig $out | Select-String -Pattern '^\s*([0-9A-F]+) Dependent Load Flag'
if (-not $dlf -or ([Convert]::ToInt32($dlf.Matches[0].Groups[1].Value, 16) -band 0x800) -eq 0) {
  throw "the DLL's DependentLoadFlags lacks LOAD_LIBRARY_SEARCH_SYSTEM32 (0x800): imports would use the search order"
}

"built $out"
"  {0:N0} bytes" -f (Get-Item $out).Length
"  exports {0}/{1} (parity with System32\version.dll)" -f $mine.Count, $real.Count
"  sha256 $((Get-FileHash $out -Algorithm SHA256).Hash)"
