# Preditor Flee Fire - SSE 1.7.104 port notes (2026-09-12/13)

Upstream: MrGezz/Preiditor-flee (fork of CageTV/Preiditor-flee), branch `IcZ_SSE`.
HEAD `bc9782a` "Fix OnTick access violation and correct the ForEachReferenceInRange lambda
signature" (2026-09-12 23:03 +0800). License: from upstream repo (not MIT-licensed like
the survival trio; do not relicense or strip headers).

Makes predators and prey flee from lit torches, lanterns, lightning spells, and poison
spells. SKSE plugin; no ESP. Version 1.0.0 (CMakeLists.txt).

## CommonLib: two stacked defects in colorglass CommonLibSSE-NG b93280e

The colorglass vcpkg registry (HEAD `6309841a`, 2023-05-13) serves CommonLibSSE-NG at
commit b93280e. That commit has two defects that crash every plugin built against it on
Skyrim 1.7.104:

**1. `REL::Module::load_version()` / `mock()` misclassify 1.7.x as SE.**
Both functions switch on `_version[1]` (the MINOR version): `case 4 → VR`, `case 6 → AE`,
`default → SE`. Skyrim 1.7.104 has minor 7, so it falls to the SE branch.
`IDDatabase::load()` then requests `Data/SKSE/Plugins/version-<ver>.bin` in **format 1**,
and every `RELOCATION_ID(se, ae)` picks the SE id. A format-1 companion file for 1.7.104
was briefly served by the `IcZ - Address Library Format 2 (1.7.104)` MO2 mod; it parsed
cleanly but resolved SE ids into an AE offset table. The result was `PlayerCharacter::
GetSingleton()` returning a non-canonical pointer (`0xE9031C36A90D8D48` -- x86-64 code
bytes read as a data pointer), crashing `FleeManager::OnTick` at `player->GetParentCell()`
two crash logs, uptimes 32 s and 4 min 57 s, same ASLR-invariant value both times.
Confirmed across two DLLs (PreditorFleeFire and StatusIndicatorFramework).

**2. `IDDatabase` has no format-5 support at all.**
`Format` enum at b93280e is `{ SSEv1, SSEv2, VR }`. On a correct AE build (minor==6),
`load()` passes `a_formatVersion=2` and `header_t::read()` calls `report_and_fail` when
the file's format word is 5. Runtime 1.7.104 ships **only** `versionlib-1-7-104-0.bin` in
format 5 (565,759-slot dense table, 130,597 holes at offset 0).

There is no newer colorglass baseline to bump to -- the registry is dead. A library
upgrade to a format-5-capable CommonLibSSE-NG lineage would require an API migration.
The chosen fix is a **vcpkg overlay port that patches b93280e in place**.

## Overlay port: `overlayports/commonlibsse-ng/`

`vcpkg-configuration.json` was modified to add `"overlay-ports": ["./overlayports"]`
alongside the colorglass registry entry, which is kept so vcpkg can still resolve other
colorglass packages.

`overlayports/commonlibsse-ng/portfile.cmake` fetches CharmedBaryon/CommonLibSSE at
`b93280e832f263dbef44e44cbe2936622a02f91a` (SHA512 `c98a0dde...`) and applies
`commonlibsse-ng-b93280e-1.7.x-address-library-format5.patch` before building.
`overlayports/commonlibsse-ng/vcpkg.json` names the port `commonlibsse-ng`, version-semver
`3.8.0`, **port-version 2** -- the bumped port-version forces a new vcpkg ABI hash,
ensuring the patched library is not confused with any cached unpatched install.

The patch (189 lines) touches three files in CommonLibSSE-NG b93280e:

- **`include/REL/Module.h`** -- adds `case 7:` alongside `case 6:` in BOTH
  `load_version()` and `mock()`, so Skyrim 1.7.x is classified as AE.
- **`include/REL/ID.h`** -- `header_t::read()` accepts format 5 when format 2 was
  requested and parses its different 96-byte header tail (fixed 64-byte name, pointerSize,
  reserved dataFormat, count). Adds `_format` field and `format()` accessor. `unpack_file()`
  gains a dense branch that reads every slot and skips entries whose offset is 0 (holes);
  the real entry count is stored in a sentinel slot at `base[denseCount]`. `id2offset()`
  checks `it->id == a_id` on **every** runtime, not only VR (upstream bug: a missing id
  silently resolved to the nearest neighbour).
- **`src/REL/ID.cpp`** -- `load_file()` sizes the shared mapping to
  `(address_count + 1) * sizeof(mapping_t)` for format 5, names it
  `CommonLibSSEOffsets-v5f-<ver>` (not `-v2-`), and on the mmap-reuse path recovers the
  entry count from `base[address_count].id`. Port-version 2 adds: if that sentinel still
  reads 0 (another process opened the mapping but had not finished unpacking), re-unpacks
  into it rather than handing out an empty span.

Verified: `PreditorFleeFire.dll` (2026-09-13 02:46) contains the UTF-16 string
`CommonLibSSEOffsets-v5f-` -- confirms the patched library is linked.

The same patch and port structure is deployed in Status-Indicator-Framework-SKSE
(`.buildenv/overlayports/`) and UnsearchedCorpseIndicator.

## What this port changed

**Committed on `IcZ_SSE`:**

- `bc9782a` (2026-09-12) -- **Fix OnTick access violation and correct the
  ForEachReferenceInRange lambda signature.** Two-pass restructure: phase 1 collects
  deterrent holders into a vector; phase 2 iterates the vector. Removes nested
  ForEachReferenceInRange (double lock-hold during cell scan). Adds `IsActorValid()`
  guard (`IsDeleted()`, `IsDisabled()`, `GetParentCell()`, `Get3D()`). Adds
  `std::atomic<bool> shutdownFlag` set on `kPreLoadGame`, cleared on `kPostLoadGame`/
  `kNewGame` -- tick thread skips posting tasks while set. Adds tracker pruning (stale
  FormIDs erased per tick). Copies player position by value before the scan loop.
  Also replaces the GitHub default `.gitattributes` with the workspace-standard one
  (LF in repo, eol overrides, binary rules for game assets).
- `ddbd3bd` / `4633aee` (2026-09-07) -- Add Lightning and Poison spell deterrents (WIP,
  not yet in-game tested), then document them in README.
- `3e346d5` (2026-09-06) -- Fix build, rename project, rework flee mechanism.

`main.cpp` also contains a dependency-free `WriteRawMarker()` that appends a timestamped
line to `%TEMP%\PFF_marker.txt` at plugin load, so a zero-byte or missing marker file
after a game session means the SKSE loader never called `SKSEPluginLoad` at all.

**Changed by this port:**

- `.gitignore` -- adds `Skyrim/Data/SKSE/Plugins/*.dll`, `*.pdb`, `build/`, `install/`.
  The DLL and PDB are staged into the `Skyrim/Data/` tree by the workspace and synced
  to MO2 by `deploy.py`; keeping them out of git prevents accidental binary commits.
- `vcpkg-configuration.json` -- adds `"overlay-ports": ["./overlayports"]`. The existing
  colorglass registry entry is unchanged.
- `include/logger.h` -- NDEBUG-conditional log level (see below).
- `overlayports/commonlibsse-ng/` -- the overlay port described above (portfile.cmake,
  vcpkg.json, the patch file).

## Build

Generator: **Visual Studio 18 2026**, toolset **v145**, `-A x64`, triplet
`x64-windows-static-md`, config `RelWithDebInfo`. CMakePresets.json declares Ninja as its
default generator; the workspace standard for SKSE plugins is VS 18 2026 / v145, invoked
from PowerShell. The presets were NOT used. Verified from `D:\b\pff\CMakeCache.txt`:
`CMAKE_GENERATOR = Visual Studio 18 2026`, toolset `v145`, platform `x64`.
Binary dir **`D:\b\pff`** -- never inside the checkout (vcpkg buildtrees hit MAX_PATH).

```powershell
cmake `
    -S "<checkout>" `
    -B D:\b\pff `
    -G "Visual Studio 18 2026" -T v145 -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded`$<`$<CONFIG:Debug>:Debug>DLL" `
    -DCMAKE_CXX_FLAGS="/permissive- /Zc:preprocessor /EHsc /MP /W4 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE"
cmake --build D:\b\pff --config RelWithDebInfo
```

Run from PowerShell, **never Git Bash** -- MSYS argument conversion rewrites `/O2` into
`C:/Program Files/Git/O2`.

RelWithDebInfo flags: `/Zi /O2 /Ob1 /DNDEBUG` (optimised; the PDB lets CrashLoggerSSE
print source file and line numbers).

Output: `D:\b\pff\RelWithDebInfo\PreditorFleeFire.dll` (2,450,944 bytes) +
`PreditorFleeFire.pdb`. Both staged into `Skyrim/Data/SKSE/Plugins/` (git-ignored).

Dependencies (vcpkg.json): `spdlog`, `rapidcsv`, `commonlibsse-ng` (satisfied by the
overlay port). In-tree: SKSEMenuFramework headers at `include/` (no external package).

## In-game validation (2026-09-13)

Session 02:20–02:40, runtime 1.7.104, SKSE 2.3.1.

`D:\Downloads\Documents\My Games\Skyrim Special Edition\SKSE\skse64.log`:

    checking plugin PreditorFleeFire.dll
    loading plugin "Preditor Flee Fire"
    plugin PreditorFleeFire.dll (00000001 Preditor Flee Fire 01000000) loaded correctly (handle 14)

No error / unsupported / fail lines for this plugin in that log.

`D:\Downloads\Documents\My Games\Skyrim Special Edition\SKSE\Preditor Flee Fire.log`
(2,739 lines, 259,380 bytes):

- `[02:20:35] PFF: kDataLoaded -- settings loaded, menu registered, starting tick thread`
- 2,312 `PFF: OnTick running, cell=0x...` lines at ~500 ms intervals across the session.
  Of those, 1,163 report `cell=0x12125EE6` -- a Legacy of the Dragonborn cell (plugin
  index `[12]` is `LegacyoftheDragonborn.esm`, confirmed from
  `crash-2026-09-13-00-03-14.log` PLUGINS section).
- Per-actor classification lines for Goat, Chicken and Horse, each followed by `"no lit
  light holder within N units"`. The reworked OnTick and the pointer lambdas ran against
  real actors for ~20 minutes without a crash.
- No flee was triggered in this session: no deterrent holder was ever within range of a
  creature. The Lightning and Poison deterrent paths (`ddbd3bd` / `4633aee`) remain
  untested in play.

The two earlier OnTick crashes (crash-2026-09-11-01-41-50, crash-2026-09-11-02-14-09)
did not recur. AA reports playing up to the Legacy of the Dragonborn Dragonborn Gallery
intro with everything fine.

`crash-2026-09-13-00-03-14.log` is from a grass-precache run (NGIO-NG OOM); its SKSE
PLUGINS list has no PreditorFleeFire.dll. No crash log exists after that timestamp.

## logger.h: log level

The original `SetupLog()` forced `spdlog::level::trace` with `flush_on(trace)` regardless
of build config, with an author comment to dial it back once flee behaviour was confirmed
end to end. At trace level the 20-minute validation session produced 2,739 log lines and
259,380 bytes, each line flushed to disk, growing without bound.

Change: builds with `NDEBUG` defined (Release,
RelWithDebInfo) now set `level::info` and `flush_on(info)`. Info lines are rare (plugin
load, save-game events) and worth keeping if the game dies. A Debug build keeps trace.

Rebuilt 2026-09-13 02:46 (`D:\b\pff\RelWithDebInfo\PreditorFleeFire.dll`, 2,450,944
bytes), deployed to the MO2 mod "Predator Flee (SKSE)". **This DLL was deployed after
the validation session above**, so the log-level change itself has not yet been observed
in game.

## Adversarial review (2026-09-13)

Documented in `RequiemLotDPatch/analysis/crash-triage-2026-09-12.md` (Builds section).
Three independent reviewers -- dense-reader correctness, shared-mapping lifecycle, blast
radius of the AE reclassification -- returned SOUND with no required changes.

- 40 AE ids on these three plugins' paths are present in `versionlib-1-7-104-0.bin`; the
  two ids 1.7.104 lacks (82331, 99886) are unreachable from them. (Cited from crash-triage
  doc; ids verified against `versionlib-1-7-104-0.bin` with `addrlib.py` in that session.)
- `PreditorFleeFire` plugin sources (`*.cpp`, `*.h`, excluding `vcpkg_installed/` and
  `overlayports/`) contain no `RELOCATION_ID` call -- confirmed by grep.
- The shared-mapping open/create race in `src/REL/ID.cpp` is pre-existing and harmless in
  practice: SKSE's `PluginManager::InstallPlugins` loads plugins in a single-threaded loop.

## Deployment

Staged into `Skyrim/Data/SKSE/Plugins/PreditorFleeFire.{dll,pdb}` (git-ignored). Two
files total; no ESP, no INI, no MCM.

`RequiemLotDPatch/tools/deploy-map.json` entry:

```json
{
  "mod": "Predator Flee (SKSE)",
  "src": "Preiditor-flee/Skyrim/Data",
  "note": "Rebuilt 2026-09-12 with the same patched overlay port, plus the OnTick
           two-phase rework and the ForEachReferenceInRange pointer-lambda fix."
}
```

`RequiemLotDPatch/tools/deploy.py` syncs `Preiditor-flee/Skyrim/Data` into
`D:/Mosais/mods/Predator Flee (SKSE)/`. MO2 is the deployment authority; nothing writes
into the game directory directly. The mod is currently **enabled** in MO2.

DLL datestamp: 2026-09-13 02:46:01 (logger.h rebuild; see above).

## Requiem / LotD impact

None at the record level: no ESP. The DLL is a pure behaviour hook -- it posts tasks to
SKSE's task interface on a timer and uses ForEachReferenceInRange to scan for actors near
deterrent-holders. It does not touch gameplay records, NPC stats, or the Reqtificator patch.

## Not done

- Lightning and Poison deterrent paths (`ddbd3bd` / `4633aee`) are untested in play;
  no deterrent holder was near a creature in the 2026-09-13 validation session.
- The logger.h log-level change (NDEBUG-conditional info vs trace) was deployed after
  the validation session; the reduced-verbosity DLL has not yet been observed in game.
