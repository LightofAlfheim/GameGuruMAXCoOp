# Co-op on several PCs

There are two ways to connect, both from the same launcher (`Play.exe`):
- **Steam (recommended):** friends click **Join co-op** and pick the host from their Steam friend
  list. There are no addresses, ports or VPNs. Steam connects the PCs directly, or through Valve's
  relay servers when it has to.
- **By address:** the direct connection, over a VPN (e.g. Radmin VPN), port forwarding or a home
  network (section 4).

Up to **4 players**: the host plus up to 3 friends (the limit is set in Co-op Settings). Every PC
must run **the same export** and **the same level**. The level needs a player character for each of
the other players: `Player1`..`Player4` (each with a `...Melee` twin), any N-1 of them for N
players. If the builds differ, the overlay says "protocol mismatch"; if the levels differ, the
zombies stay unsynced.

## 1. Make the package you send

1. **Build the launcher once:** run `CoopTest\launcher\build-launcher.bat` and copy the `Play.exe`
   it makes, plus `How to play.txt`, into `<MAX folder>\coop\` (next to the dev build's
   `GameGuruMAX.exe`). See the top of `build-launcher.bat` for the optional icon, background and
   logo arguments, e.g.

   ```bat
   CoopTest\launcher\build-launcher.bat "<project folder>\project256.ico" "" "" "<art>\KeyArt.png" "<art>\Logo.png"
   ```

   It uses the C# compiler that ships with Windows, so there's nothing to install.
2. **Set up the project:** in the editor, **Co-op → Co-op Settings...** (player count, shared
   pickups / doors / checkpoints, level exit rule, anti-cheat, launcher title and colours). The
   settings are saved per project.
3. **Export from the dev build:** start the dev build's `GameGuruMAX.exe` (no arguments), open the
   project and use the storyboard's standalone export. The export copies whichever `GameGuruMAX.exe`
   is running, so exporting from the Steam copy would leave co-op out. With "Add co-op files to
   the export" on, the export also gets `Play.exe`, `How to play.txt`, `coop.ini` (from your
   settings) and `steam_appid.txt`.
4. **Zip the folder** and send it.

**Re-export and send again** whenever the engine or the level changes. Leave the project's
`allowfragmentation` setting at its default (1). Any other value makes the game relaunch itself
between levels, which drops the co-op connection.

Sharing an export with someone else is distributing your game. That is what standalone export is
for, but check the GameGuru MAX licence for what it allows.

## 2. Playing over Steam

Everyone: Steam open and signed in, and Steam friends with the host.

1. **Host:** `Play.exe` → **Host co-op** → start the level.
2. **Friends:** `Play.exe` → **Join co-op** → the launcher lists Steam friends who are hosting (it
   refreshes by itself) → click one → start the same level. "Join whoever is hosting" at the bottom
   lets the game find a hosting friend by itself; if several are hosting, press 1-9 to pick one.

### App ID 480 ("Spacewar")

- **What it is:** Steam ties lobbies, friends' game status and peer-to-peer connections to a game's
  App ID. Until the game has its own, exports use **480**, Valve's public test app.
- **What you'll notice:** Steam shows you as playing "Spacewar". Accepting a Steam invite would
  start Spacewar, so friends join from the launcher's list instead.
- **Finding the right lobby:** the game marks its own lobbies, so it never joins somebody else's
  Spacewar test.
- **Getting your own App ID:** register the game on Steamworks (Steam Direct). Then put the new
  number in the exported `steam_appid.txt`. Nothing else changes.
- **Testing:** test Steam co-op with exports, on separate PCs with separate Steam accounts. One
  Steam account can't connect to itself.

## 3. What the launcher does

`Play.exe` starts the game exe in the same folder:

| Button | Starts the game with |
|---|---|
| **Play** | no flags (single player) |
| **Host co-op** | `-netsteam=host` |
| **Join co-op** | `-netsteam=join` (`+connect_lobby <id>` when a friend is picked) |
| **Connect by address** | opens the direct options: `-netjoin=<address>` / `-nethost` |

- **Steam check:** the Steam buttons check that Steam is running first.
- **Remembered address:** the last address typed is remembered per user, until `host=` in
  `coop.ini` changes.
- **First run:** an unsigned exe from a zip gets the "Windows protected your PC" screen the first
  time (*More info → Run anyway*). The exported game exe gets the same screen.

## 4. Playing by address (no Steam)

UDP port **27015** by default (Co-op Settings): the host listens, the others connect.

- **VPN (e.g. Radmin VPN):** everyone joins the same VPN network. The host clicks **Connect by
  address instead → HOST**; friends join the host's VPN address (put it in Co-op Settings so `coop.ini` has it).
  Windows often treats a VPN as a **Public** network, so allow the game on Public networks when
  Windows Firewall asks.
- **Port forwarding:** in the host's router, forward **UDP 27015** to the host PC. Friends join the
  host's public IP. This doesn't work behind CGNAT, where the router's WAN address differs from
  "what is my IP".
- **Same home network:** join the host's `192.168.x.x` address.

## 5. Troubleshooting

| Symptom | Likely cause |
|---|---|
| "Steam is not running" / "Steam is offline" | Start Steam and sign in. The game keeps retrying every few seconds |
| Steam: stuck on "Looking for a friend hosting" | The host hasn't started the level yet, you aren't Steam friends, or the host's Steam is set to Offline/Invisible |
| Steam: the session is full | The host already has the maximum number of players |
| By address: stuck on "connecting" | Host not started, wrong address, firewall on the host, port not forwarded (or CGNAT), or VPN not connected |
| "protocol mismatch" / "game files don't match" | The PCs run different exports. Re-export and send again |
| Connected but zombies not synced / partner not shown | Not on the same level, or the level files differ between the PCs |

Each PC writes `coopnet-host.log` / `coopnet-client.log` next to the game's exe. Steam steps are
logged with `steam:`. Send the logs when something goes wrong. **F9** in the game shows the
technical co-op readout (always on in the dev build).

## Dev build without exporting

**Co-op → Co-op Settings → Test co-op on this PC** starts 2-4 copies of the dev build on one PC.
`coop-host.bat` / `coop-join.bat` in this folder do the same from the command line, e.g.
`coop-join.bat "My Project" 192.168.1.20` joins another PC (both PCs need the dev build and the
project's content).
