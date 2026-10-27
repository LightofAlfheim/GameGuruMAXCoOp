# Online co-op for GameGuru MAX

This branch (`coop-net`) adds 2-4 player online co-op to GameGuru MAX games. It is an engine
change: games exported from this build get co-op, with nothing to script in Lua.

## What it does

- **Host-authoritative:** the host runs the zombies/AI; other players see them in sync and their
  hits are sent to the host. Characters chase the nearest player.
- **Players:** each remote player appears as a character in the level (`Player1`..`Player4`, with a
  `...Melee` twin for melee weapons) that walks, runs, aims, shoots, reloads, swings and dies.
- **Shared world (each optional, per project):** pickups, doors, checkpoints, spectating the team
  when out of lives, and level changes (everyone or the first player at the exit).
- **Connections:** over Steam (friend list, lobbies, Valve relays; App ID 480 until your game has
  its own) or by address (direct IP, VPN, LAN) using [ENet](https://github.com/lsalzman/enet).
- **Light anti-cheat:** the host can refuse players whose scripts or weapon files differ, and stop
  co-op when a known cheat tool runs.
- **Launcher:** `Play.exe` (Play / Host co-op / Join co-op / Connect by address), added to the
  export automatically.

Not synced yet: switches, lifts and other scripted objects, projectiles and explosions.

## Installing (no build needed)

Run `GameGuruMAX-CoopSetup.exe` (from this repo's Releases). It finds GameGuru MAX in your Steam
library, keeps the Steam exe as `GameGuruMAX-original.exe` and puts the co-op exe and launcher kit
in its place. Only the exe is swapped, so you need your own copy of GameGuru MAX from Steam.
**Uninstall** in the same tool puts the original back.

Steam updates (and "Verify integrity of game files") put the Steam exe back. Run the installer
again afterwards; if GameGuru MAX was updated, use an installer made for that version (the
installer says which version it was made for).

To make the installer: build the engine, then run `CoopTest\installer\build-installer.bat`.

## Using it

1. Install as above, or build the engine (see `NOTES.md` section 1) and the launcher (`CoopTest\launcher\build-launcher.bat`;
   put `Play.exe` and `How to play.txt` in `<MAX folder>\coop\`).
2. In the editor: **Co-op → Co-op Settings...** sets up the project and can start 2-4 copies on
   one PC for testing.
3. Add `Player1`..`Player4` characters (and `...Melee` twins) to the level.
4. Export the game from the dev build. Players start it with `Play.exe`.

`CoopTest\TWO-PCS.md` covers exporting, Steam, playing by address and troubleshooting.

Command line (what the launcher passes to the game exe):

| Flag | Meaning |
|---|---|
| `-nethost[=port]` | host by address (UDP, default 27015) |
| `-netjoin=address[:port]` | join a host by address |
| `-netsteam=host` / `-netsteam=join` | host / join over Steam |

In the dev build, put these before `project=` (the engine reads the rest of the line as the
project name).

## Code

| File | What |
|---|---|
| `GameGuru Core/GameGuru/Source/M-CoopNet.cpp` | protocol, sessions, sync, puppets, settings |
| `GameGuru Core/GameGuru/Source/M-CoopNetSteam.cpp` | Steam lobbies and P2P transport |
| `GameGuru Core/GameGuru/Source/M-CoopNetEditor.cpp` | Co-op Settings window, local testing, export |
| `GameGuru Core/GameGuru/Include/M-CoopNet*.h` | the hooks the engine calls |
| `GameGuru Core/SDK/ENet/` | ENet 1.3.18, unmodified (MIT licence) |
| `CoopTest/` | launcher and installer source, test scripts, guide |

The rest of the engine is touched only at small hooks marked `// coopnet:`. Co-op does nothing
unless a `-net...` flag is given, so single player is unchanged.

`NOTES.md` is the development log: how each part works, the protocol versions, and engine gotchas.
