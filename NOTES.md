# GameGuru MAX – Co-op Fork Notes

Working notes for the `coop-net` branch (a development log - `COOP.md` is the overview).
Paths are relative to the repo root (e.g. `D:\DEV\GAMEGURUMAXREPO`) unless stated. "Core" = `GameGuru Core\`.

---

## 1. Build steps that worked

### Folder layout (all must be siblings inside one DEV folder)

```
D:\DEV\
  GAMEGURUMAXREPO\     this repo (branch coop-net)
  WICKEDREPO\          https://github.com/TheGameCreators/WickedRepo
  ASSIMP\              https://github.com/TheGameCreators/assimp
  BUILD\GameGuru Wicked MAX Build Area\Max\   copy of the Steam install
```

The `.vcxproj` files use relative paths like `../../../WICKEDREPO/WickedEngine`,
so the folder names (and capitals) matter. The Wicked-MAX project writes
`GameGuruMAX.exe` straight into `$(GG_MAX_BUILD_PATH)\Max\`, overwriting the
Steam copy of the exe there (that is intended). The Steam install itself is never touched.

### One-time setup

1. **Visual Studio 2022 Community** with *Desktop development with C++*, a
   Windows 10 SDK, and **C++ MFC for latest v143 build tools (x86 & x64)**.
   MFC is not in the default workload; without it GameGuru fails to link.
2. **Env var** `GG_MAX_BUILD_PATH = D:\DEV\BUILD\GameGuru Wicked MAX Build Area`
   (set per-user with `setx`). Restart VS / terminals afterwards.
3. **Clone** the three repos above with plain `git clone` (no login needed).
4. **Missing LIBs**: download `GameGuruMAX-MissingLIBs-v5.zip` from the repo's
   `VS2022` GitHub release and copy its `GameGuru Core\` contents over
   `Core\`. All of these files are gitignored.
5. **Build area**: copy the Steam install
   (`W:\SteamLibrary\steamapps\common\GameGuru MAX`, ~47 GB) into
   `BUILD\GameGuru Wicked MAX Build Area\Max`. Use the clean Valve
   `steam_api64.dll` (it is in the Missing LIBs zip under
   `SDK\Steamworks SDK\redistributable_bin\win64\`).
6. **Wicked NuGet packages**: extract `WICKEDREPO\packages.zip` in place
   (creates `WICKEDREPO\packages\`). VS normally restores these itself;
   command-line MSBuild does not, so it errors on `WinPixEventRuntime.targets`.

### Build (verified 2026-10-12, VS 2022 / MSVC 14.41)

```
REM 1) Wicked Engine (~5.5 min) -> Core\Guru-WickedMAX\x64\Release\WickedEngine_Windows.lib
MSBuild D:\DEV\WICKEDREPO\WickedEngine.sln -t:WickedEngine_Windows -p:Configuration=ReleaseForGG -p:Platform=x64 -m

REM 2) GameGuru MAX (~10 min for libs + ~10 min for the exe) -> $(GG_MAX_BUILD_PATH)\Max\GameGuruMAX.exe
MSBuild "D:\DEV\GAMEGURUMAXREPO\GameGuru Core\GameGuruWickedMAX.sln" -t:Wicked-MAX -p:Configuration=ReleaseForSteam -p:Platform=x64 -m
```

In VS: open the .sln, pick the configuration, right-click the project →
Set as Startup Project → Rebuild. F5 runs it with the build area as the working directory.

**Use `ReleaseForSteam`, not `Release` (the README is out of date here).** Wicked's
`ReleaseForGG` config compiles with `USE_PIX`, so the exe must link
`WinPixEventRuntime.lib`. Only GameGuru's `ReleaseForSteam` config does that.
With `Release` the link fails with `LNK2001 PIXEventsReplaceBlock /
PIXGetThreadInfo`. `ReleaseForSteam` still uses the `Release` builds of the 27
libs, and outputs to the same place. (The 2024 prebuilt
`WickedEngine_Windows.lib` in the Missing LIBs zip was built without PIX,
which is presumably why `Release` worked when the README was written.)

Expect ~9k compiler warnings. They are normal for this codebase.

### Launch / smoke test

Steam must be running. Run
`D:\DEV\BUILD\GameGuru Wicked MAX Build Area\Max\GameGuruMAX.exe` (working dir =
that folder). Expected result: the editor's welcome screen. `Max\Guru-MapEditor.log` should
contain `Guru Map Editor Loop Starts`. The original Steam exe is kept beside it
as `GameGuruMAX.steam-original.exe` for comparison.

---

## 2. Solution / project layout

`Core\GameGuruWickedMAX.sln` has ~28 projects. Most are **static libs** from
the old Dark Basic Pro engine. GameGuru started life on DBPro, and these wrap
DBPro-style commands (`ObjectPositionX`, `CameraAngleY`, `Inkey`…) on top of
Wicked Engine:

| Project(s) | Where | What |
|---|---|---|
| Animation, Basic2D, Bitmap, Camera, Core (DBDLLCore), File, Image, Input, Light, Memblocks, Objects, Setup, Sound, Sprites, System, Text, Vectors, Bullet, ConvX, FTP | `Core\Dark Basic Public Shared\Dark Basic Pro SDK\DarkSDK\` | DBPro command set re-implemented over Wicked |
| **DarkLUA** | `...\DarkSDKMore\DarkLUA\DarkLUA.cpp` (~15k lines) | Lua VM glue + **all Lua command bindings** |
| DarkAI | `...\DarkSDKMore\DarkAI\` | Old DBPro AI lib (mostly superseded by Recast/Detour + Lua) |
| PhotonMultiplayer | `...\DarkSDKMore\PhotonMultiplayer\` | Legacy GG Classic multiplayer (see §8) |
| Enhancements, InfiniteVegetation, GGVR, CPU3D | `...\DarkSDKMore\` | misc |
| **Wicked-MAX** | `Core\Guru-WickedMAX\Template_Windows.vcxproj` | **The exe.** Compiles `Guru-WickedMAX\*.cpp` *and* all of `Core\GameGuru\Source\*.cpp` (the actual game/editor code) and links the libs above + `WickedEngine_Windows.lib` |

Wicked Engine itself is built separately from `WICKEDREPO\WickedEngine.sln`
(`ReleaseForGG` config) and consumed as a `.lib`.

Game/editor source: `Core\GameGuru\Source\`. Naming is DBPro-era:
`M-*.cpp` = modules, `G-*.cpp` = game-time, `Common*.cpp` = shared.
Nearly all state lives in two giant globals, **`t`** and **`g`** (declared in
`Core\GameGuru\Include\gameguru.h`, types in `Include\Types.h`).

**Calling convention:** many functions take *no* parameters and read/write
fields of `t` instead. For example, `entity_lua_hurtplayer()` sets `t.tdamage`, `t.te`
and then calls `physics_player_takedamage()`. When tracing code, always check
which `t.*` fields are set just before a call.

---

## 3. Main loop

```
main.cpp            wWinMain → parses cmdline (wiStartupArguments), single-instance mutex, runs Master
master.cpp          Master::RunCustom()  (custom Wicked run loop, VR aware)
                    MasterRenderer::Update(dt)  (master.cpp ~2095)
GameGuruMain.cpp      GuruLoopLogic()   staged init (GPU particles → terrain → GuruMain/common_init), then each frame:
                        common_loop_logic()      (Common.cpp:755)
                        ConstantNonDisplayUpdate()  (sound/anim housekeeping)
                      GuruLoopRender() → common_loop_render()
Common.cpp          common_loop_logic(): editor? mapeditorexecutable_loop() : gameexecutable_loop()
M-Game.cpp          game_main_loop()  (M-Game.cpp:4314) – the in-game frame
```

Order of work inside `game_main_loop()` (simplified):

1. `game_timeelapsed()`
2. `weapon_projectile_loop()` (M-Weapon.cpp) – rockets/grenades
3. `physics_loop()` → `physics_player_control()` (calls Lua `PlayerControl`) → `physics_player_handledeath()`
4. `lua_loop()` (M-LUA.cpp) – pushes globals into Lua, runs every entity's script
5. `entity_loop()` / `entity_loopanim()` (G-Entity.cpp)
6. `darkai_loop()` (M-DAINew.cpp) + `game_updatenavmeshsystem()`
7. `gun_manager()` (G-Gun.cpp) – player weapon, firing
8. decals, avatars, postprocess, `game_end_of_level_check()`, HUD

There's also an extra worker thread (`GuruLogicClass` in GameGuruMain.cpp) for
some "clean" game logic such as visibility rays.

**Hook point for networking:** a natural spot is the start/end of
`game_main_loop()`, or a call next to `lua_loop()`. That runs only in-game, not in
the editor.

---

## 4. Player (position / camera / health)

- **Movement is Lua.** C++ `physics_player_control()` (M-Physics.cpp:2179)
  gathers keys, sets `g_PlrKeyW` etc., then calls the Lua global
  `PlayerControl()` (`scriptbank\global.lua:280`), which runs
  `scriptbank\gameplayercontrol.lua`, `gameplayerhealth.lua`, `gameplayerspeed.lua`.
  These call back into C++ through bindings to move the character controller.
- **Position** = the physics capsule object **`t.aisystem.objectstartindex`**
  (read with `ObjectPositionX/Y/Z(obj)`). Exposed to Lua as `g_PlayerPosX/Y/Z`
  in `lua_loop_begin()` (M-LUA.cpp:363).
- **Camera / view angle** = camera 0: `CameraPositionX(0)`, `CameraAngleX/Y/Z(0)`;
  exposed as `g_PlayerAngX/Y/Z`.
- **Health**: `t.player[t.plrid].health` (`playertype`, Types.h:8060). `t.plrid` is
  normally 1. Damage goes through `physics_player_takedamage()`
  (M-Physics.cpp:2317); death through `physics_player_handledeath()`.
  Lua reads/writes it via `g_PlayerHealth`.
- Player state/tuning: `t.playercontrol` (`playercontroltype`, Types.h:10007).
- Init/free: `physics_player_init()` / `physics_player_free()`.

## 5. Entities (enemies, AI, health)

- Level instances: **`t.entityelement[e]`** (`std::vector<entitytype>`,
  gameguru.h:3421; `entitytype` Types.h:6567). Index 1..`g.entityelementlist`.
  Key fields: `obj` (render/physics object id), `health`, `active`, `bankindex`
  (which entity type from the entity bank), `eleprof` (per-instance properties,
  incl. its Lua script name).
- Per-frame: `entity_loop()` (G-Entity.cpp:840), `entity_loopanim()`.
- **AI is mostly Lua.** Each entity has a script (`eleprof.aimain_s`); characters
  use behaviours (`scriptbank\people\*.byc`) interpreted by
  `scriptbank\masterinterpreter.lua`. Targeting is **hard-wired to the single
  player**: it uses `g_PlayerPosX/Y/Z` and `GetPlayerDistance(e)` (a Lua function in
  `global.lua:371`). This is the main thing "nearest of two players" will have to
  change later. Zombie/horror behaviours are in `scriptbank\horror\`.
- C++ side of AI: `darkai_loop()` in **M-DAINew.cpp** (the only DAI file
  compiled; M-DAI.cpp is legacy, not in the project), Recast/Detour navmesh in
  `Core\Guru-WickedMAX\GGRecastDetour\`. Lua moves AI with bindings such as
  `AIEntityGoToPosition`.
- Entity health from Lua: `SetEntityHealth`, `SetEntityHealthWithDamage`, which go to
  `entity_lua_setentityhealth_core()` (M-LUA-Entity.cpp:2499).

## 6. Shooting / damage

- Player gun: `gun_manager()` → `gun_control()` → `gun_shoot()` /
  `gun_shoot_oneray()` (G-Gun.cpp:219/1229/4035/4295). Weapon data in `t.gun[]`,
  current gun `t.gunid`, state `t.gunmode`.
- Ray hit → `entity_hasbulletrayhit()` (G-Entity.cpp:3146) →
  `entity_determinedamagemultiplier()` → **`entity_applydamage()`**
  (G-Entity.cpp:2590; target in `t.ttte`, damage in `t.tdamage`).
- Projectiles: `weapon_projectile_loop()` (M-Weapon.cpp:290).
- Enemies hurting the player: Lua `HurtPlayer(e, dmg)` → `entity_lua_hurtplayer()`
  → `physics_player_takedamage()`. NPC gunfire: `darkai_shoottarget()` (M-DAINew.cpp:2145).

## 7. Lua execution & command bindings

- VM wrapper: **DarkLUA.cpp** (`LuaSetFunction`, `LuaCall`, `LuaSetInt`,
  `LuaSetFloat`, `LuaGetInt`…).
- **Bindings:** `addFunctions()` (DarkLUA.cpp:~13222) has ~1,400
  `lua_register(lua, "Name", CFunc)` calls. To add a new Lua command: write
  `int MyCmd(lua_State* L)` in DarkLUA.cpp and register it there.
- Older **"SendMessage" style**: many `global.lua` wrappers (e.g.
  `HurtPlayer(e,v)` → `SendMessageI_hurtplayer(e,v)`) call registered C functions
  that put args into `t.e`/`t.v` and dispatch through `int_core_sendmessagei()`
  (DarkLUA.cpp:12230) → `entity_lua_*()` in M-LUA-Entity.cpp.
- Game-side driver: **M-LUA.cpp**. `lua_init`, `lua_loadscriptin`,
  `lua_launchallinitscripts` (calls each entity's `<script>_init(e)`),
  `lua_loop()` → `lua_loop_begin()` (writes `g_*` globals) →
  `lua_loop_allentities()` (calls `<script>_main(e)` per active entity) →
  `lua_loop_finish()`.
- Scripts at runtime come from `Max\Files\scriptbank\` in the build area. The repo
  copy is `Scripts\scriptbank\`.

## 8. Things that matter for co-op later

- **Legacy multiplayer exists but is disabled.** `M-MPPhoton.cpp` / `M-MPSteam.cpp`
  (GameGuru Classic's Photon/Steam MP: `mp_*` functions,
  `t.game.runasmultiplayer`) are compiled but gated by `DISABLEMULTIPLAYERFORMAX`
  (`Include\preprocessor-moreflags.h`). It's useful as a reference for how they
  synced players and entities. Not planned for reuse (cloud service, tied to the old
  editor flow).
- **Standalone exports = the same exe, renamed.** The engine decides editor
  vs. game by exe name (`gamegurumax.exe`/`guru-mapeditor.exe` → editor,
  anything else → standalone game, `t.game.gameisexe = 1`; Common.cpp ~3917,
  GameGuruMain.cpp). So code we add ships in exported games automatically.
  Any **new DLL** would also have to be added to the export file list,
  so we prefer static linking.
- **Single-instance mutex** in `main.cpp` blocks a second copy of the *editor*
  when there are no command-line args. Standalone games (which always have args) may
  run multiple copies. This affects testing two instances on one PC.

### Side effects of running the dev build

- **Shared write folder.** If you've set a custom write folder in the editor
  preferences (here: `W:\GameGuruMAX`), the dev build uses it too. It sees and saves the
  **same projects and save games** as the Steam install. Back up a project
  before editing it in a dev build.
- The editor writes `HKCU\Software\GameGuruMAX\LatestInstallPath` with its own
  folder on every start (Common.cpp ~4248, "for future patch installers").
  Running the dev build points it at `D:\DEV\...`. Running the Steam build
  points it back. Nothing else reads it.

---

## 9. Co-op networking (Phase 2 proof of concept)

**Status:** verified 2026-10-12. Two copies on one PC (coop-host.bat + coop-join.bat,
Snowy Mountain Stroll) connect, and each overlay shows the other's position updating live.

**Code:** `GameGuru\Source\M-CoopNet.cpp` / `Include\M-CoopNet.h`. ENet 1.3.18 is
vendored unmodified in `Core\SDK\ENet\` and compiled straight into the exe (no
DLL, so the standalone export file list does not change).

**Hooks into engine code (the only edits outside the new files):**
- `M-Game.cpp`: `#include "M-CoopNet.h"`, `coopnet_update()` at the end of
  `game_main_loop()`, and `coopnet_shutdown()` at the top of `game_freegame()`.
- `Template_Windows.vcxproj(.filters)`: the ENet `.c` files + M-CoopNet. Include path
  and `_WINSOCK_DEPRECATED_NO_WARNINGS` are set **per file**. The project has
  SDL checks on, which would otherwise turn ENet's deprecation warnings into errors.

**Command line** (must come *before* `project=`, which takes the rest of the line):

| Flag | Meaning |
|---|---|
| `-nethost` / `-nethost=PORT` | host (1 client max), default UDP 27015 |
| `-netjoin=IP` / `-netjoin=IP:PORT` | join; retries every 3 s until the host answers |

No flag means the module is inactive: on the first in-game frame it initialises ENet,
finds no flag, releases ENet again, and returns immediately on every later frame.

**Protocol v2** (a version mismatch disconnects):
- channel 1 (reliable): `HELLO` {u8 type, u16 version, u8 player id}; `WEAPON`
  {u8 type, u8 len, gun name}, sent on change and right after the handshake ("" = unarmed).
- channel 0 (unreliable, sequenced): `PLAYERSTATE` {u8 type, u32 seq, f32 x,y,z,
  f32 pitch,yaw, u8 action, u8 firecount, u8 meleecount}, sent at 20 Hz.
  - Position comes from the player capsule object `t.aisystem.objectstartindex`;
    angles come from camera 0.
  - Action is read from `t.gunmode`: 101–120 fire (melee if the gun is
    `weapontype >= 51` or `settings.ismelee`), 1020–1023 melee, 121–126 and 700–707 (shotgun-style) reload.
  - The counters go up each time firing or melee starts, so a short action
    between two samples isn't lost.

**Output:**
- **Top-left overlay in-game:** mode/port, players, ping, tx/rx packets per second, local
  and remote position, last event.
- **`coopnet-host.log` / `coopnet-client.log`** next to the exe: events plus
  one remote-position line per second. Connect/disconnect events are also
  written to `Guru-Game.log`.

**Running two copies on one PC:** `GameGuruMAX.exe <netflag> project=0<Project Name>`
runs a project straight as a standalone game (the storyboard "play" path).
`CoopTest\coop-host.bat` and `CoopTest\coop-join.bat` wrap this (default
project: the *Snowy Mountain Stroll* sample). The game opens on its title
screen, and networking starts once you press START and the level is running.

**Two PCs:** see `CoopTest\TWO-PCS.md`.
- **Export:** the standalone export copies the running `GameGuruMAX.exe` (M-MapFile.cpp), so an
  export made from the dev build contains co-op.
- **Launchers:** `CoopTest\launcher` builds `Play.exe` (WinForms, compiled with the Windows csc).
  It has Play / Host co-op (Steam) / Join co-op (Steam) buttons, plus a folded "connect by address"
  panel (`-nethost` / `-netjoin=`, address pre-filled from `coop.ini`). `CoopTest\standalone\*.bat`
  start the direct connection without a window.
- **Logs:** written next to whichever exe is running.
- **Level changes:** `allowfragmentation` must stay at 1 (the default). Other values relaunch the
  exe between levels and drop the command-line flags.

**Background windows:** normally the whole engine stops when its window loses
focus (main.cpp message loop: `g_bActiveApp` goes false 45 s after launch, then
the loop only `Sleep(1)`s). While co-op is active, `coopnet_keeprunninginbackground()`
uses the engine's existing `bKeepActiveEvenInBackground` exception, so an
unfocused copy keeps simulating and networking. Player input is still ignored
when unfocused: the standalone game sets `t.plrhasfocus = 0` when its window
isn't in the foreground (M-Game.cpp ~2397).

### Remote player puppet (Phase 3)

**Which entity:** a character named **`Player2`** in the level (optionally also
**`Player1`**, found by `eleprof.name_s`, case-insensitive).
- The host shows the client as `Player2`.
- The client shows the host as `Player1`, falling back to `Player2`.
- **Animation set follows the weapon:** a character's animations are NOT baked in by
  Character Creator. darkai appends `charactercreatorplusnimationssets<adult male|adult female>default animations<suffix>.dbo`
  at level start, choosing the suffix from the weapon the entity holds via
  `animsystem_getweapontype` and the gun's gunspec `animsetoverride` (no weapon = `-melee`,
  `-pistol` = no suffix, `-rifle`, `-axe`, `-spear`...), or from the entity's own
  `overrideanimset_s`. The puppet holds nothing at level start, so it gets `-melee`;
  `coopnet_puppet_updateanimset` appends the set for the remote player's weapon
  (`AppendObject` + `WickedCall_RefreshObjectAnimations`) whenever it changes.
- **Melee twins (optional):** each name may have a `…Melee` twin (`Player1Melee`,
  `Player2Melee`), the same character. It is shown while the remote holds a melee weapon
  or nothing, the base one for guns. Each entity remembers its set and held weapons,
  so pipe <-> gun switching never reloads anything. Without twins the one entity
  re-appends the set on each switch (a file load).
- Without either, a grey capsule-sized box is shown (object 69469, a free gap
  below the ragdoll range in the hand-assigned IDs in Common.cpp).
- All named entities (and twins) are hidden whenever co-op is **not** running, so co-op levels
  play normally in single player, Test Game and exports.

**Taking over the entity** (every frame, `coopnet_takeoverentity`):
- collision and physics are switched off once, as Lua `CollisionOff` does;
- `lua.outofrangefreeze = 1` skips its script;
- it's removed from `t.charanimstates`. Otherwise darkai would put unseen
  characters to sleep, reset their animation and set animation speed, and
  `darkai_refresh_characters` re-adds characters whenever something spawns.

**Movement:**
- It's drawn 100 ms in the past, blending the two snapshots either side of that time.
- Feet sit at capsule y − 50. Measured: the capsule settles 48–51 above terrain,
  and the log prints `local capsule … above terrain` once a second.
- Walk/run root motion is cancelled with `WickedCall_SetBip01Position(…, 3, 0, 0)`
  each frame, the same call darkai uses on moving characters.

**Weapon:**
- The gun is made once per gun with the engine's own `entity_createattachment`
  (gunspec hand position/rotation/scale, glued to the character's firespot
  limb) and cached. Don't call that function per switch: it also pre-spawns a
  hidden loot-drop copy of the weapon every time.
- Only guns used in the level can be shown. Exports only include those.

**Animations** (standard MAX set, each state with fallbacks):

| State | Animations |
|---|---|
| Unarmed or melee weapon | `Idle`, `Walk_Loop`, `Run_Loop` |
| Holding a gun | `Idle_Aim`, `Walk_Aim_Loop`, `Run_Aim_Loop` |
| Firing while standing | `Aim_Shoot` (held 400 ms after each shot) |
| Melee swing | `Attack_01` / `Attack_02`, played once and alternating |

Walk/run thresholds are 20 and 250 units/s.

**Gunfire (protocol v6):** the sender counts rounds from its gun's total ammo going down and
sends the count in `PLAYERSTATE`. For each round the puppet shows the cosmetic half of NPC firing
(`darkai_shoottarget`/`darkai_shooteffect`): light flash and muzzle-flash decal at the held weapon's
firespot, a third-person fire sound (looping for automatics), and a tracer along the remote view
direction if the gun has tracers. The hit-the-player part is not used.

**Not yet:**
- spine pitch for aiming up and down;
- the puppet can't be damaged.

### Host-run zombies and nearest-player targeting (Phase 4)

**Status:** verified 2026-10-13 on one PC (two copies): zombies mirrored smoothly, nearest-player
targeting, client-routed damage and grabs, client hits, death sync, bullet and script blood, zombie sounds.

**Protocol v3 adds:**
- `LEVEL`: level file name plus an FNV-1a fingerprint of the index and type of every character
  *placed* in the level (`t.storedentityelementlist`). Zombie sync only runs when both sides match;
  the overlay shows `zombie sync ON` or `LEVEL MISMATCH`.
- client health in `PLAYERSTATE`;
- `ZOMBIES` (host → client, 20 Hz, same sequence number as `PLAYERSTATE`);
- `DAMAGE` (host → client, reliable).

**Zombie sync** (`coopnet_sendzombies` / `coopnet_zombies_update`):
- The host sends every placed character except the puppets: position, Y angle,
  animation frame range, loop flag, frame, speed and visibility.
- The client takes each one over every frame (Lua skipped, removed from darkai)
  and drives it from the host's snapshots, using the same timeline smoothing as the puppet.
- Physics stays on, so the client still bumps into zombies. The capsule is steered
  placed exactly at the host position every frame (`ODESetBodyPosition`, velocity zeroed).
  Steering it with `entity_updatepos` (darkai's velocity mover) lagged far behind.
- Animations copy the host's range and mode. The frame is resynced if it drifts more
  than 8 frames, and root motion is cancelled.

**Targeting** (host only; every hook is a no-op otherwise):
- Each character picks the nearer player every 250 ms. The other player must be 15%
  nearer to switch, and dead players are skipped.
- Engine code asks `coopnet_targetremote(e, …)`, which overwrites the local-player
  values only for characters chasing the remote player. Hooks (all marked `// coopnet:`):
  - **M-LUA.cpp** `lua_loop_allentities`: `g_PlayerPosX/Y/Z` and `g_PlayerHealth` swapped
    only around the entity's `_main` call (so `GetPlayerDistance` follows);
  - **G-Entity.cpp** `entity_controlrecalcdist`: `plrdist` measured from the target. This
    also stops zombies near the client being frozen as out of range;
  - **M-DAINew.cpp** `darkai_calcplrvisible`: `plrvisible` uses the target's health,
    position and eye (capsule + 28.5). The ray stops 25 units short of the puppet;
  - **M-LUA-Entity.cpp** `entity_lua_hurtplayer`: if the attacker chases the remote player,
    a `DAMAGE` message is sent instead, and the client calls `physics_player_takedamage`.
  - **Zombie grab/bite** (protocol v4 `PLAYERCMD`): while a character chasing the remote
    player runs its script, `FreezePlayer`/`UnFreezePlayer`, `SetCameraOverride`,
    `SetCameraPosition`/`SetCameraAngle` (camera 0) and `ChangePlayerWeaponID` are sent to
    the client instead of acting on the host. Hooks: **DarkLUA.cpp** `RawSetCameraData`,
    **M-LUA-General.cpp** `lua_freezeplayer`/`lua_unfreezeplayer`, **M-LUA-Entity.cpp**
    `entity_lua_changeplayerweaponid`. The client gives back its own weapon after the grab,
    and unfreezes itself if the host disconnects mid-grab.
- Characters only: pickups and zones also read `plrdist` and stay with the host player.
- **Timing (v4):** `PLAYERSTATE` and `ZOMBIES` carry the sender's `timeGetTime()`, and all
  smoothing uses it. Timing by `seq * 50 ms` ran slow whenever the sender managed fewer than
  20 sends a second, so the client's clock ran ahead and zombies were never smoothed.
- `coopnet_targetremote` is read from the visibility worker thread, so its table is fixed
  size (65536 entities) and the remote values are plain copies.

**Deaths, blood, sounds (protocol v5):**
- **Client hits:** `entity_applydamage` (G-Entity.cpp) asks `coopnet_forwardhit` first. On the
  client, a hit on a mirrored zombie is not applied locally. `HIT` sends every input
  `entity_applydamage` reads (target, damage, multiplier, force, source, ray, impact direction,
  limb, ray type), and the host replays it through `entity_applydamage`.
- **Deaths:** the host sends `DEATH` (with the ragdoll-plus impact direction) when a synced
  character's health reaches zero. The client stops mirroring it and sets health 0 and
  `ragdollplusactivate`, so its own engine runs the death sequence (a `Death_*` animation if there
  is room, then the ragdoll). It sets `aimain = 0` and stops skipping the entity's Lua table, so
  corpse faders see the death.
- **Bullet blood:** `entity_triggerdecalatimpact` sends `BLOODFX` both ways.
- **Script side effects:** while a synced character's script runs on the host, the DarkLUA
  dispatchers (`int_core_sendmessagei/f`) forward Show/Hide/Destroy/Set/ResetPosition/SetRotation
  of *other* entities and all sound commands (`LUACMD`), and the client replays them through the
  same `entity_lua_*` handlers. That covers blood decal pools moved by zombie scripts, and zombie sounds.

**Not yet:**
- a zombie that drops to zero health and is then revived by its script would stay dead on the client;
- zombies spawned during play aren't synced (not needed for this game: all zombies are placed in the level);
- ragdolls show as the death animation on the client;
- NPC gunfire (`darkai_shoottarget`) still only hurts the host;
- client zombies stay frozen if the host disconnects.

### Co-op player death (protocol v7)

**Status:** verified 2026-10-13: repeated deaths and respawns on both sides, spectating, shared
checkpoints, the partner's death animation played once and held.

**Rules** (chosen for this game): respawn at a **shared checkpoint** after **10 s**, **spectate the
partner** while dead, **unlimited lives in co-op**. The life a death takes is given back as the death
starts, and it's game over only if both players are dead at the same time. Single player (or a
disconnected partner) keeps the level's lives. The out-of-lives spectate path below is only a fallback.

**How:** the engine's own death and respawn still do the work.
- **Delay:** the 2 s `deadtime` set by `physics_player_takedamage` is stretched to 10 s while a
  partner is connected (`coopnet_updatedeath`).
- **Spectating:** after 1.5 s the camera follows the partner's smoothed eye position
  (`g.luacameraoverride = 3`, released 0.2 s before respawn). The puppet is hidden meanwhile.
- **Lives:** **M-Physics.cpp** `physics_player_handledeath` asks `coopnet_outoflives()` before
  GAME OVER. While the partner is still in, the player stays dead and spectates. `PLAYERSTATE`
  carries lives and an out-of-lives flag.
- **Checkpoints:** **M-LUA-Entity.cpp** `entity_lua_checkpoint` calls `coopnet_sharecheckpoint`
  (`CHECKPOINT` message), and the receiver sets `t.playercheckpoint` the same way.
- **Partner's puppet:** plays a `Death_*` animation held on its last frame (hidden if the
  character has none), and jumps to the respawn point.
- **On screen:** centre-screen text shows the respawn countdown and out-of-lives state.
- **Dead screen:** `gameplayerhealth.lua` fades the screen to full red/black while dead (cleared on
  respawn). The death fog is cleared every frame while spectating.
- **Shake hits:** behaviours' hurt-with-shake calls `HurtPlayer(0, …)`, so the hurt redirect goes by
  whose script is running, not by the entity number passed in.

### Pickups (protocol v8)

**Status:** built 2026-10-13, awaiting test.

**Rules:** guns and ammo are **per-player**. Each player collects their own copy, and these pickup
scripts already run on both machines. Any **other placed item** (health, keys, quest items, anything
a script `Destroy()`s) is **shared**: once either player takes it, it's gone for both.

**How:** `ITEMGONE` (u16 entity, reliable, both ways).
- **Send:** `coopnet_itemleftworld(e)` is called from two places:
  - `coopnet_forwardluacommand` on any `Destroy(e)`;
  - **DarkLUA.cpp** `SetEntityCollectedEx` when it removes the item from the game.

  Only placed (level) entities are sent, and never characters, `isweapon > 0` or `isammo != 0`.
- **Receive:** `coopnet_applyitemgone` turns collision off, hides the object and sets `active = 0`,
  like `Destroy` does. It does **not** set `collected`, so a key the partner picked up doesn't count
  as yours (`darklua_refreshhaskeystatefor`).
- **Race:** if both players grab the same item in the same moment, both get it.

### Doors (protocol v9)

**Status:** built 2026-10-13, awaiting test.

**Problem:** each machine runs its own copy of every door script, and that copy only reacts to its
own player.

**How:** when a door opens or closes on one machine, the partner's copy of the same script is put
into the same transition (`DOOR` message: u16 entity, u8 1 = open / 2 = close). That copy then plays
its own sound, animation or rotation, and handles collision, the navmesh blocker, logic links and
prompts itself.
- **Stock scripts untouched:** a Lua chunk (`g_pCoopNetDoorLua` in M-CoopNet.cpp) is loaded into
  the game's Lua state only while a co-op session is live. It is reloaded after a level load or
  reconnect.
- **State tables:** reached through script globals (`g_door_rotate`, `g_door_sliding`,
  `g_trapdoor`) or script locals via `debug.getupvalue` (`door`, `doors`).
- **Adapters:** one per script: `door`, `door_rotate`, `door_rotate_multi`, `door_sliding`,
  `oneway_door`, `trapdoor`, `trapdoor_rotate`. Each adapter has:
  - `phase()`, which maps the script's state to closed / opening / open / closing;
  - `open()` / `close()`, which make the same change the script makes on an E press (sound and
    logic links included).

  The entity → adapter mapping comes from the entity's script name (`eleprof.aimain_s`).
- **Send:** `coopnet_doorpoll()` runs after each frame's scripts and reports doors that crossed
  closed↔open since the last poll. On session start every door counts as closed, so doors that are
  already open are sent to a partner who joins late.
- **Receive:** a change that arrives while the door is mid-way through the opposite move is held
  for up to 10 s and applied when that move ends. Otherwise it is ignored (already in that state).
- **Errors:** Lua errors are logged as `door sync Lua error: …`, and the doors found are logged at
  session start.
- **Locks:** locked doors stay per-player (each needs the key). Once one player opens a door, it
  opens for both.
- **Logic links:** a door's logic links fire on both machines.
- **New door scripts** need a new adapter. Switches, lifts and other scripted objects are not synced.

### Up to 4 players (protocol v10)

**Status:** direct IP verified 2026-10-14 with 4 copies on one PC. Steam with several clients
(plus the pause-menu keep-alive) verified with friends 2026-10-15. F8 invites were removed the same day:
with App ID 480 an invite accepted while the game is closed starts Spacewar. Friends join from the
launcher's friend list instead (`-netsteam=join +connect_lobby <id>`).

**Shape:** the host is player 0 and the hub. Clients connect to the host only and get ids 1..3
(first free one) in the host's HELLO. The host relays to the other clients:
- `PLAYERSTATE` and `WEAPON` (both now carry a player id, which the host overwrites with the
  sender's);
- `BLOODFX`, `ITEMGONE`, `DOOR` and `CHECKPOINT`, when the sender is on its level.

`PLAYERLEFT` tells clients when one leaves. A joining client is sent the host's and the other
clients' weapons.

**Per-player data:** `sCoopNetPlayerSlot g_CoopNetPlayers[4]` holds, per player id, the direct
connection (host: each client; client: the host as player 0), the remote state, snapshots, clock
offset and puppet.
- **Selecting a player:** the old single-player names (`g_CoopNetRemote`, `g_CoopNetPuppet`,
  snapshot and time-offset globals) are now macros on `g_CoopNetPlayers[g_iCoopNetCur]`, so the
  tested puppet code is unchanged. The loop in `coopnet_update` and the `PLAYERSTATE` handler set
  `g_iCoopNetCur`.
- **Session flags:** `g_bCoopNetConnected` / `g_bCoopNetHandshakeDone` mean "any connection"
  (`coopnet_updatesession`).
- **Levels:** compared per connection (`g_CoopNetRemoteLevels`). `coopnet_levelsmatch()` checks
  the sender's connection while a packet is being handled, otherwise any connection.
- **Sending:** `coopnet_send` sends to every connection; `coopnet_sendtoplayer` sends to one.
- **Zombie clock:** a client's zombie mirroring uses the host's clock offset (player 0)
  explicitly.

**Puppets:** entities `Player1..Player4` plus `...Melee` twins (twin = slot + 4). Player id n uses
`Player<n+1>`, or borrows any free one (`coopnet_assignpuppets`). Each machine hides itself, so N
players need any N-1 of these entities in the level. Missing ones become one grey box per player
(objects 69469-id).

**Zombies (host):** `g_CoopNetTarget[e]` = the player id chased (0 = host). Each character picks
the nearest living player and keeps its target unless another is 15% nearer. Damage and grab
commands go to that client only (`coopnet_targetof`, `g_iCoopNetLuaTarget`).

**Death:** a dead player spectates the nearest living teammate (`g_iCoopNetSpectateID`; that
puppet is hidden). Game over when every teammate on the level is dead at once. The out-of-lives
fallback waits until everyone is out.

### Light anti-cheat (protocol v11)

**Status:** built 2026-10-15, awaiting test.

**File check:** at co-op start, `coopnet_hashfiles` takes an FNV-1a 64 fingerprint over the files a
player could edit to cheat: `Files\scriptbank\**\*.lua` and `Files\gamecore\guns\**\*.txt` next to
the exe, path (lower case) plus bytes, in path order.
- **Joining:** `HELLO` carries the fingerprint. The host refuses a client whose fingerprint differs
  (`KICK`, reason files), so everyone needs the same build.
- **Before joining:** until a client passes the `HELLO` checks, the host ignores everything else it
  sends.

**Kick / block (host):** F7 lists the players in the game. Number = kick, Shift+number = kick and
block.
- **Kicking:** `coopnet_kick` sends `KICK` (reason), stops listening to the player and tells the
  others they left (`PLAYERLEFT`). It hangs up 1 s later (`g_dwCoopNetKickAt`) so the `KICK`
  arrives first.
- **Blocking:** writes `steam:<id>` or `ip:<address>` (plus the name as a note) to
  `coop-blocklist.txt` next to the exe. Blocked players are refused when they connect. Delete a
  line to unblock.
- **The kicked client:** shows the reason on screen and stops trying (`g_iCoopNetKickedReason`
  blocks reconnects; Steam: `coopsteam_shutdown`).

**Live checks (protocol v12):** built 2026-10-17, not yet tested live.
- **Scripts changed after launch:** `FindFirstChangeNotification` on scriptbank and gamecore\guns;
  the fingerprint is taken again 1 s after a change.
- **Cheat tools:** every 3 s, the "Mod Menu" window, a modmenu / Cheat Engine process, and Mod Menu
  files in scriptbank.
- **On a hit:** co-op stops on that PC until restart. A client sends `KICK(CHEAT)` to the host
  and leaves. A host ends the game for everyone (`KICK(HOSTCHEAT)`).

### Editor integration: Co-op Settings (protocol v13, v14)

**Status:** built 2026-10-17. v14 (2026-10-26): the puppet action animations below were removed again.

**Settings file:** `Files\projectbank\<project>\coopsettings.ini`, one per project (in the writable
area when the project is there). Read and written by `coopnet_loadsettings` /
`coopnet_savesettings` (M-CoopNet.cpp). The game reads it when the project is known
(`coopnet_updatesettings`: the storyboard loads after co-op starts), and again if the project
changes.

**Editor:** Co-op menu > Co-op Settings (M-CoopNetEditor.cpp, `coopnet_editor_window`, called next
to `ProcessPreferences` in M-GridEdit.cpp). Any change saves the file. Sections:
- **Players:** maximum players (2-4), and a check of which Player1-4 / ...Melee entities the
  loaded level has.
- **Shared gameplay:** pickups, doors, checkpoints, spectate when out of lives.
- **Anti-cheat:** file check, cheat-tool check.
- **Launcher and export:** title, accent, host address, port, "add co-op files when exporting".
- **Test co-op on this PC:** starts 2-4 copies of the exe with `-nethost=PORT` / `-netjoin=127.0.0.1:PORT
  project=0<project>`. They run next to the editor and use the saved project.

**Host-authoritative switches:** after `HELLO` the host sends `SETTINGS` (bits: pickups, doors,
checkpoints, spectate, cheat check). The client keeps them over its own file
(`g_iCoopNetHostSettings`) until the session ends. Max players: the host refuses a `HELLO` that
would go over the limit with `KICK(FULL)`. File check off: the host skips the fingerprint
comparison.

**Export** (`coopnet_export_addfiles` in stage2a, `coopnet_export_copyextras` in stage4,
M-MapFile.cpp):
- **File collection** (encrypted with the rest): coopsettings.ini; the
  `sets\adult male|female\default animations*.dbo` for the character types the level's Player
  entities use (both if none). Before this, exports had
  no animation sets, so puppets kept their model's own animations.
- **Next to the exe:** Play.exe and How to play.txt, from `<MAX folder>\coop\` (copy them there
  from `CoopTest\launcher` after rebuilding the launcher). Also coop.ini (title / accent / host /
  port / game from the settings) and steam_appid.txt (480).
- **Runtime lookups:** `coopnet_gamefileexists` also finds the `_e_` (encrypted) copy.

**Puppet action animations (v13, removed in v14):** jump / fall / land, crouch, hurt, and use /
pick up / door (custom .dbo clips appended after the character's set) were tried and dropped -
they kept breaking: appended clips sometimes showed nothing after a set reload (melee twin swap),
the clips were long and slow, and pickups were hard to detect reliably on the client (an item
taken with E is not always within range of the capsule). PLAYERSTATE is back to the v12 layout;
the puppet plays only walk / run / aim / shoot / reload / melee / death. Code: commit dc8403f0
and earlier on this branch if it is ever wanted again.

### Level changes for the team (protocol v15)

**Status:** built 2026-10-26, awaiting test.

**Level exits:** `game_jump_to_level_from_lua` / `game_finish_level_from_lua` (M-Game.cpp, what
JumpToLevel / JumpToLevelIfUsedEx / FinishLevel end in) ask `coopnet_levelexit` first. With nobody
else on the level it returns false (single-player behaviour). Otherwise the level does not end there:
- **Client:** sends `LEVELEXIT` (finish flag, reset states, `t.tleveltojump_s`,
  `pAdvanceWarningOfLevelFilename`) to the host, again every 2 s while waiting.
- **Host:** records who is at an exit (`bAtExit`, the first exit's request is where the team goes).
  Co-op Settings > Shared gameplay > Level exit:
  - **All players must reach it** (`exitneedsall=1`, default): goes when every player in play on the
    host's level and not out of lives has reached an exit. `EXITSTATUS` shows "At the exit - waiting
    for your team (n/m)" / "n/m players are waiting at the exit".
  - **First player takes everyone** (`exitneedsall=0`).
- **Going:** the host sends `LEVELGO` to the players on its level and runs the same jump itself
  (`coopnet_exitgo`, with `g_bCoopNetExitAllowed` so the hook lets it through). Every PC replays the
  same call, so storyboard "next level" logic gives the same level everywhere.
- **Reached = reached:** an exit counts for the rest of the level (exits like "press E to continue"
  report once). Reset when the level changes (`coopnet_newlevel`, which also clears spectating /
  out-of-lives).
- **Not copied:** the win zone's spawn-marker user global is only set on the PC that reached it.

**Follow the host:** `LEVEL` now carries the level path (from `mapbank\`). A client on another level
than the host (just joined, or the storyboard sent it elsewhere) jumps there, as a specific level
(`pAdvanceWarningOfLevelFilename`), retrying at most every 15 s; it logs it if that level is missing.

**Loading:** a level load (and the storyboard screens between levels) stops the game loop for many
seconds. `loadingpageprogress` and `titleslua_blocking_run` (M-Titles.cpp) call
`coopnet_loadingupdate`:
- **First call:** sends `LOADING`, and gives every connection a 90 s timeout (ENet
  `enet_peer_timeout`, Steam `coopsteam_settimeout`). A receiver of `LOADING` does the same for that
  connection until it hears from it again.
- **While loading:** the connections are serviced, but packets are held (`g_CoopNetHeldPackets`;
  PLAYERSTATE / ZOMBIES dropped) - the level's entities are half built.
- **First game frame after:** `coopnet_loadingdone` recomputes our level first (so old-level events
  no longer match), then handles the held packets in order and restores the timeouts.
- **Zombies:** a player still loading reports the old level, so the host's zombies don't target them.

### Steam transport (M-CoopNetSteam.cpp)

**Status:** verified 2026-10-14 on two PCs over the internet (App ID 480).

**Transport switch:** `-netsteam=host|join` (or Steam's `+connect_lobby <id>`) sets
`g_bCoopNetSteam`. The game protocol is unchanged; only the transport underneath differs.
- **Sending:** `coopnet_send` hands packets to `coopsteam_send` (ENet's reliable flag maps to
  `k_EP2PSendReliable`, everything else to `k_EP2PSendUnreliableNoDelay`).
- **Receiving:** `coopnet_servicesteam` polls connection events and packets.
- **"Connected":** checks use `g_bCoopNetConnected` instead of `g_pCoopNetPeer`.

**Lobby:** the host creates a friends-only lobby (4 members) with lobby data `edcoop=1` and
`host=<name>`. Steam connections are numbered like player ids: host clients 1..3 (first free
one on CONNECT, BUSY when full), a client's host 0 (`sCoopSteamPeer m_Peers[4]`). A client
joins in one of three ways:
- from `GameLobbyJoinRequested_t` (an invite, or "Join game" in the friends list);
- from `+connect_lobby` on the command line;
- by searching its friends every 2 s (`GetFriendGamePlayed`: same App ID, a lobby, `edcoop=1`).
  Lobby data it doesn't have yet is fetched with `RequestLobbyData`. Friends already in a lobby
  show the same lobby, so entries are de-duplicated by lobby and named after its host. If several
  lobbies are found, the player picks one with keys 1-9.

**Connection:** Steam P2P (`ISteamNetworking`, relay allowed). Every packet starts with a
transport byte: GAME, CONNECT/ACCEPT/BUSY, BYE, PING/PONG.
- **Joining:** the client sends CONNECT every second until ACCEPT.
- **Who gets in:** session requests are accepted from friends and lobby members only.
- **Dropping out:** after 10 s with no packets, on BYE, on P2P failure, or when the partner leaves
  the lobby.
- **Packet size:** unreliable packets over 1200 bytes go reliable.
- **Ordering:** ZOMBIES packets are now ordered by their sequence number
  (`g_uCoopNetLastZombieSeq`), since Steam's unreliable packets can arrive out of order.

**App ID:** `steam_appid.txt` in the working folder if present. Otherwise `SteamAppId=480` in the
environment, with a fallback of writing `steam_appid.txt` and retrying.
- **Steam not running:** the game retries every 5 s and says so on screen.
- **Shutdown:** SteamAPI is never shut down (the lobby is left and the session closed).

**Compatibility:** the engine's Steamworks headers are old (SteamClient017 / Friends015 /
MatchMaking009 / Networking005). The shipped `steam_api64.dll` contains exactly those interface
versions and exports the legacy accessors, so `SteamFriends()` etc. are safe. Check this again if
the DLL is ever updated.

**On screen:** Steam status top-centre until connected and for 5 s whenever the player count
changes; 1-9 = pick a host (client started without a lobby). Steam starts at launch
(`coopnet_earlylaunch` from WinMain) so the overlay can attach. The top-left technical readout is hidden in exported games; F9 toggles it.

### Engine gotchas found along the way

- **`SetObjectFrame` stops the animation.** In the Wicked build it calls
  `WickedCall_SetObjectFrame`, which does `Stop()` and turns looping off. To set a frame on
  a playing animation, use `WickedCall_SetObjectFrameEx` (as `smoothanimupdate` does).
- **`PlayObject(obj, start, end)` doesn't stop at `end`.** It runs on into the next animations in the
  model, so hold the last frame yourself (speed 0 plus `WickedCall_SetObjectFrameEx`).
- **`GetPlaying()` reads 0 even while a character animation runs.** Watch `GetFrame()`
  instead.
- **The engine's animation name lookup matches loosely.** Check for an exact name in
  `pObject->pAnimationSet` first.
- **Character animation sets come from the held weapon at level start** (darkai), not from
  Character Creator.
- **`entity_createattachment` also pre-spawns a hidden loot-drop entity every call.**
- **Menu mouse mode parked the cursor at screen point (320,240)**, outside the window on a second monitor: now the window centre
  (M-LUA.cpp) so menus work on a second monitor.
- **Resizing the game window after start-up hides centred script text.** `GetDisplayWidth/Height` keep
  the start-up size, so prompts, the pickup crosshair "+" and `TextCenterOnX` are drawn off screen.
  `move-window.ps1` used to stretch the client window to fill a 2048×1152 second monitor. It now only
  moves the window and keeps its size.

**Known limitations (fine for the proof of concept):**
- Networking only runs inside `game_main_loop`, so on title, loading and pause
  screens nobody services ENet. A pause longer than ENet's timeout (~5–30 s)
  drops the connection; the client then reconnects by itself.
- Both copies write the same `Guru-Game.log`.
- Over the internet the host needs UDP 27015 forwarded, or both players on a
  virtual LAN (Tailscale/ZeroTier).
