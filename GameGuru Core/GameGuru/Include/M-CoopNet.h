//----------------------------------------------------
//--- GAMEGURU - M-CoopNet
//--- 2-player co-op networking (ENet). Kept self-contained so upstream merges
//--- only touch the few hook lines that call into this module.
//----------------------------------------------------

#pragma once

// Call once per in-game frame (from game_main_loop). Lazily reads the command
// line on first call; does nothing at all unless -nethost / -netjoin= was given.
void coopnet_update ( void );

// Call when the game is freed (game_freegame). Safe to call when inactive.
void coopnet_shutdown ( void );

// Call every frame while the in-game menu is open (game_main_loop isn't running then): keeps the
// co-op connections alive so the other players don't time us out.
void coopnet_menuupdate ( void );

// Call once at launch, before the renderer is created (main.cpp): when the command line asks for
// co-op over Steam, starts Steam already so its overlay can attach to the game.
void coopnet_earlylaunch ( const char* pCommandLine );

// True while hosting/joining: the main loop (main.cpp) must keep running when
// the window is in the background, or the remote player gets no answers.
// The game already ignores player input while unfocused (t.plrhasfocus).
bool coopnet_keeprunninginbackground ( void );

// Nearest-player targeting (host only). Engine code that asks "where is the player?" on
// behalf of a character fills in the local player's values, then calls this: if that
// character is chasing the remote player it overwrites them and returns true.
// Safe to call from the visibility worker thread. Any pointer may be NULL.
bool coopnet_targetremote ( int e, float* pX, float* pY, float* pZ, float* pEyeY, int* pHealth );

// Around each entity's Lua <script>_main call (M-LUA.cpp): point g_PlayerPosX/Y/Z and
// g_PlayerHealth at that entity's target, and put them back afterwards.
void coopnet_lua_beginentity ( int e );
void coopnet_lua_endentity ( void );

// HurtPlayer(e, damage) (M-LUA-Entity.cpp): if character e is chasing the remote player,
// sends the damage to them and returns true - the local player is not hurt.
bool coopnet_hurtremoteplayer ( int e, int iDamage );

// Player-control commands a script issues on behalf of a character chasing the remote
// player (zombie grab/bite: freeze, camera override/position/angle, weapon take-away)
// go to the remote player instead. Returns true if redirected - don't apply locally.
enum eCoopNetPlayerCmd
{
	COOPNET_PCMD_CAMERAOVERRIDE = 1,	// value
	COOPNET_PCMD_CAMERAPOSITION = 2,	// x, y, z (camera 0)
	COOPNET_PCMD_CAMERAANGLE = 3,		// x, y, z (camera 0)
	COOPNET_PCMD_FREEZE = 4,
	COOPNET_PCMD_UNFREEZE = 5,
	COOPNET_PCMD_WEAPON = 6,			// value = weapon id (0 = take away)
};
bool coopnet_redirectplayercommand ( int iCmd, int iValue, float fX, float fY, float fZ );

// entity_applydamage (G-Entity.cpp), reading the usual t.ttte/t.tdamage/... inputs: on the
// client, damage to a zombie mirrored from the host is sent to the host instead (host-
// authoritative). Returns true if forwarded - don't apply locally.
bool coopnet_forwardhit ( void );

// entity_triggerdecalatimpact (G-Entity.cpp): a blood splat at a bullet impact is shown on
// the other machine too.
void coopnet_forwardbloodimpact ( float fX, float fY, float fZ );

// DarkLUA command dispatchers: while a synced character's script runs on the host, these
// commands are replayed on the client (blood decal pools, zombie sounds...).
enum eCoopNetLuaCmd
{
	COOPNET_LUACMD_HIDE = 1, COOPNET_LUACMD_SHOW, COOPNET_LUACMD_DESTROY,
	COOPNET_LUACMD_SETSOUND, COOPNET_LUACMD_PLAYSOUND, COOPNET_LUACMD_PLAYSOUNDIFSILENT,
	COOPNET_LUACMD_LOOPSOUND, COOPNET_LUACMD_STOPSOUND, COOPNET_LUACMD_PLAYNON3DSOUND,
	COOPNET_LUACMD_SETPOSITIONX, COOPNET_LUACMD_SETPOSITIONY, COOPNET_LUACMD_SETPOSITIONZ,
	COOPNET_LUACMD_RESETPOSITIONX, COOPNET_LUACMD_RESETPOSITIONY, COOPNET_LUACMD_RESETPOSITIONZ,
	COOPNET_LUACMD_SETROTATIONX, COOPNET_LUACMD_SETROTATIONY, COOPNET_LUACMD_SETROTATIONZ,
};
void coopnet_forwardluacommand ( int iCmd, int e, float fValue );

// SetEntityCollected (DarkLUA.cpp), when an item is removed from the game: placed items other
// than guns and ammo are shared - it disappears for the partner too. (Destroy() is covered by
// coopnet_forwardluacommand.)
void coopnet_itemleftworld ( int e );

// entity_lua_checkpoint (M-LUA-Entity.cpp): checkpoints are shared - the partner gets it too.
void coopnet_sharecheckpoint ( float fX, float fY, float fZ, float fA );

// game_jump_to_level_from_lua / game_finish_level_from_lua (M-Game.cpp), e.g. a win zone: returns
// true when co-op takes the level exit over - the whole team leaves together, when the host says
// (see "Level exits" in M-CoopNet.cpp) - and the level must not end now.
bool coopnet_levelexit ( bool bFinish, int iResetStates );

// loadingpageprogress / titleslua_blocking_run (M-Titles.cpp): a level loading or a storyboard
// screen between levels - keeps the co-op connections alive while the game loop is stopped.
void coopnet_loadingupdate ( void );

// physics_player_handledeath (M-Physics.cpp), when the player's lives are used up: returns true
// while the co-op partner is still in the game (spectate them instead of game over); false
// when the partner is out of lives too, or gone (game over as normal).
bool coopnet_outoflives ( void );

//
// Co-op settings: per project, edited in the editor (Co-op menu -> Co-op Settings) and saved to
// Files\projectbank\<project>\coopsettings.ini, which goes into the exported game with it.
// The host's gameplay switches are sent to everyone who joins.
//

#define COOPNET_SETTINGS_TEXT_MAX	128

struct sCoopNetSettings
{
	// players
	int iMaxPlayers;				// 2..4, host included
	// gameplay (the host's values are used by everyone)
	bool bSharePickups;				// keys / quest items picked up by one player vanish for all
	bool bSyncDoors;				// doors opened by one player open for all
	bool bShareCheckpoints;			// a checkpoint reached by one player counts for all
	bool bSpectateOutOfLives;		// out of lives: watch the team instead of game over
	bool bExitNeedsAll;				// level exit (win zone...): every player must reach it; false = the first one takes everyone
	// anti-cheat
	bool bFileCheck;				// refuse players whose scripts / weapon files differ from the host's
	bool bCheatCheck;				// stop co-op when a cheat tool runs or scripts change mid-game
	// launcher (written into coop.ini when exporting)
	char pTitle[COOPNET_SETTINGS_TEXT_MAX];		// "" = the game's name
	char pAccent[16];							// "#RRGGBB", "" = launcher default
	char pHostAddress[COOPNET_SETTINGS_TEXT_MAX];	// "Connect by address" box default
	int iPort;
	bool bExportCoopFiles;			// add Play.exe, coop.ini, Steam files, animation sets when exporting
};

void coopnet_settingsdefaults ( sCoopNetSettings* pSettings );
// project = Storyboard.gamename; false if the file doesn't exist (pSettings then holds the defaults)
bool coopnet_loadsettings ( const char* pProject, sCoopNetSettings* pSettings );
bool coopnet_savesettings ( const char* pProject, const sCoopNetSettings* pSettings );
// the file's full path (read location, or where it will be written)
void coopnet_settingsfile ( const char* pProject, char* pOut, bool bForWrite );
// Storyboard.gamename (M-CoopNetEditor.cpp, which has the editor headers)
const char* coopnet_projectname ( void );

// the co-op player characters: slot 0-3 = Player1..Player4, 4-7 = their ...Melee twins
#define COOPNET_PUPPET_SLOTS	8
const char* coopnet_puppetname ( int iSlot );
int coopnet_findpuppetentity ( int iSlot );	// entity element in the loaded level, 0 = none

// editor: the Co-op Settings window (M-CoopNetEditor.cpp), call every editor frame
void coopnet_editor_window ( void );
void coopnet_editor_open ( void );
// standalone export (M-MapFile.cpp): add the co-op files to the export's file collection
// (settings, animation sets), and copy the launcher files next to the exported exe
void coopnet_export_addfiles ( void );
void coopnet_export_copyextras ( const char* pExportFolder, const char* pExeName );
