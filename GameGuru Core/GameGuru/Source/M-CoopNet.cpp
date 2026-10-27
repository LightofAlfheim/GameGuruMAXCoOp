//----------------------------------------------------
//--- GAMEGURU - M-CoopNet
//--- 2-player co-op networking (ENet)
//---
//--- Phase 2 proof of concept: host + one client exchange the local player's
//--- position/rotation at 20 Hz and show what they receive on screen and in
//--- coopnet-host.log / coopnet-client.log (next to the exe).
//---
//--- Command line (must come BEFORE project=..., which eats the rest of the line):
//---   -nethost            host on the default port (27015)
//---   -nethost=PORT       host on PORT
//---   -netjoin=IP         join a host on the default port
//---   -netjoin=IP:PORT    join a host on PORT
//---   -netsteam=host      host over Steam: friends-only lobby, friends join from the launcher's list
//---   -netsteam=join      join over Steam: a Steam friend who is hosting, or a Steam invite
//---                       (M-CoopNetSteam.cpp - Steam P2P, no IP addresses or ports needed)
//----------------------------------------------------

// ENet must come first so winsock2.h is included before windows.h
#include <enet/enet.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

#include "stdafx.h"
#include <share.h>
#include <string>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>
#include "gameguru.h"
#include "M-CoopNet.h"
#include "M-CoopNetSteam.h"
#include "tracers/TracerManager.h"

// defined in M-LUA-Entity.cpp / DarkLUA.cpp, not in a header
int entity_lua_getanimationname ( int e, cstr FindThisName_s, float* fFoundStart, float* fFoundFinish );
float GetLUATerrainHeightEx ( float fX, float fZ );
void WickedCall_SetBip01Position ( sObject* pObject, sFrame* pFrame, int iUseMode, float fX, float fZ );

// v2: PLAYERSTATE gained action + fire/melee counters, new WEAPON message
// v3: PLAYERSTATE gained health, new LEVEL and ZOMBIES messages
// v4: PLAYERSTATE and ZOMBIES carry the sender clock; new PLAYERCMD message
// v5: HIT, DEATH, BLOODFX and LUACMD messages
// v6: PLAYERSTATE gained a shot counter (puppet muzzle flash / gunshot sound per shot)
// v7: PLAYERSTATE gained lives and flags (out of lives); CHECKPOINT message
// v8: ITEMGONE message (shared pickups and other destroyed/collected placed items)
// v9: DOOR message (door scripts opened/closed by either player)
// v10: up to 4 players - the host relays between clients; PLAYERSTATE/WEAPON carry the player
//      id, HELLO from the host assigns the client its id, PLAYERLEFT message
// v11: HELLO carries a fingerprint of the game's scripts and weapon files (the host refuses edited
//      ones); KICK message (host refusing / kicking / blocking a player)
// v12: live cheat checks (cheat tools running, scripts changed after launch); KICK both ways -
//      a client tells the host it stopped because of a cheat
// v13: PLAYERSTATE gained jump/crouch flags and a use counter + kind (puppet animations);
//      SETTINGS message (the host's gameplay switches); KICK reason "game full"
// v14: puppet action animations (jump / crouch / use...) removed again - PLAYERSTATE back to
//      the v12 layout
// v15: level changes for the whole team - LEVEL carries the level's path (joiners go to the host's
//      level), LEVELEXIT / LEVELGO / EXITSTATUS / LOADING messages, SETTINGS bit "all players exit"
#define COOPNET_PROTOCOL_VERSION	15
#define COOPNET_MAX_PLAYERS			4		// player 0 is the host, clients get 1..3 as they join
#define COOPNET_NO_PLAYER			0xFF
#define COOPNET_DEFAULT_PORT		27015
#define COOPNET_SEND_INTERVAL_MS	50		// 20 Hz
#define COOPNET_RECONNECT_MS		3000
#define COOPNET_STALE_MS			1000

// channel 0: unreliable + sequenced (ENet drops late packets) - continuous state
// channel 1: reliable + ordered - events (handshake now; damage/spawns later)
#define COOPNET_CHANNEL_STATE		0
#define COOPNET_CHANNEL_EVENTS		1
#define COOPNET_CHANNEL_COUNT		2

// disconnect reason codes (sent as ENet disconnect data)
#define COOPNET_DISCONNECT_NORMAL	0
#define COOPNET_DISCONNECT_VERSION	1

enum eCoopNetMode
{
	COOPNET_UNINITIALISED = 0,
	COOPNET_OFF,
	COOPNET_HOST,
	COOPNET_CLIENT
};

// Players: the host is player 0 and the hub - every client connects to the host only, and the
// host passes on what one client sends that the others need (positions, weapons, shared world
// events). Messages about a player carry its id; the rest come from/go to the host.
enum eCoopNetMsg
{
	COOPNET_MSG_HELLO = 1,			// u8 type, u16 protocol version, u8 id (host -> client = the client's player id, client -> host = COOPNET_NO_PLAYER), u64 files fingerprint
	COOPNET_MSG_KICK = 16,			// host -> client: u8 reason (eCoopNetKick) - you're out; don't reconnect. Client -> host: COOPNET_KICK_CHEAT, we stop. Reliable
	COOPNET_MSG_PLAYERSTATE = 2,	// u8 type, u8 player id, u32 seq, u32 sendtime, f32 x,y,z, f32 pitch,yaw, u8 action, u8 firecount, ... (relayed by the host)
	COOPNET_MSG_WEAPON = 3,			// u8 type, u8 player id, u8 length, chars: gun name ("" = unarmed), reliable, sent on change (relayed)
	COOPNET_MSG_PLAYERLEFT = 15,	// host -> clients: u8 player id that disconnected, reliable
	COOPNET_MSG_LEVEL = 4,			// u8 type, u32 fingerprint, u16 character count, str level file name, str its path ("mapbank\..."); reliable
	COOPNET_MSG_ZOMBIES = 5,		// host -> client: u8 type, u32 seq, u16 count, then per character (see coopnet_sendzombies)
	COOPNET_MSG_DAMAGE = 6,			// host -> client: u8 type, u16 attacker entity, i16 damage; reliable (a zombie hit the client)
	COOPNET_MSG_PLAYERCMD = 7,		// host -> client: u8 type, u8 command, u16 entity, f32 x,y,z, i32 value (a zombie freezing/grabbing the client)
	COOPNET_MSG_HIT = 8,				// client -> host: a shot hit a mirrored zombie (see coopnet_forwardhit), reliable
	COOPNET_MSG_DEATH = 9,			// host -> client: u16 entity, u8 impact direction (1-6), reliable
	COOPNET_MSG_BLOODFX = 10,		// both ways: f32 x,y,z of a blood splat at a bullet impact, reliable
	COOPNET_MSG_DOOR = 14,			// both ways: u16 entity, u8 1=open 2=close - a door script changed state here, reliable
	COOPNET_MSG_ITEMGONE = 13,		// both ways: u16 entity - a shared item was picked up / destroyed here, remove it there too, reliable
	COOPNET_MSG_CHECKPOINT = 12,	// both ways: f32 x,y,z,a - a player reached a checkpoint, the partner gets it too (shared), reliable
	COOPNET_MSG_LUACMD = 11,		// host -> client: u8 command, u16 entity, f32 value - replayed zombie-script command, reliable
	COOPNET_MSG_SETTINGS = 17,		// host -> client after HELLO: u8 COOPNET_SETTING_* bits - the host's gameplay switches, reliable
	// level changes (see "Level exits" below); str = u8 length + chars
	COOPNET_MSG_LEVELEXIT = 18,		// client -> host: u8 finish (0 = jump to a level), u8 reset states, str target, str advance-warning file - we reached a level exit, reliable
	COOPNET_MSG_LEVELGO = 19,		// host -> clients: same fields - everyone leaves the level this way now, reliable
	COOPNET_MSG_EXITSTATUS = 20,	// host -> clients: u8 players at the exit, u8 players needed (0 = nobody waiting), reliable
	COOPNET_MSG_LOADING = 21,		// both ways (one connection): the sender is loading a level - allow it a long silence, reliable
};
#define COOPNET_SETTING_PICKUPS		1
#define COOPNET_SETTING_DOORS		2
#define COOPNET_SETTING_CHECKPOINTS	4
#define COOPNET_SETTING_SPECTATE	8
#define COOPNET_SETTING_CHEATCHECK	16
#define COOPNET_SETTING_EXITALL		32	// every player must reach the level exit (else the first one takes everyone)

enum eCoopNetKick
{
	COOPNET_KICK_NONE = 0,
	COOPNET_KICK_FILES,			// game files (scripts / weapons) differ from the host's
	COOPNET_KICK_KICKED,		// the host kicked them
	COOPNET_KICK_BLOCKED,		// on the host's block list
	COOPNET_KICK_CHEAT,			// a cheat was found on this player's PC (they stop themselves)
	COOPNET_KICK_HOSTCHEAT,		// a cheat was found on the host's PC - the co-op game ended
	COOPNET_KICK_FULL,			// the host's game has its maximum number of players (Co-op Settings)
};

// what the player's weapon is doing (from t.gunmode, same ranges as Lua g_PlayerGunFired)
enum eCoopNetAction
{
	COOPNET_ACTION_NONE = 0,
	COOPNET_ACTION_FIRE,		// gunmode 101-109 with a gun
	COOPNET_ACTION_MELEE,		// gunmode 1020-1023, or "firing" a melee weapon
	COOPNET_ACTION_RELOAD,		// gunmode 121-126 (normal reload) or 700-707 (shotgun-style shell reload)
};
#define COOPNET_WEAPON_NAME_MAX		64
#define COOPNET_PLAYERFLAG_OUTOFLIVES	1	// lives used up: spectating until the partner is out too

struct sCoopNetPlayerState
{
	unsigned int seq;
	unsigned int sendTime;		// sender timeGetTime() when sent (smoothing timeline; seq*50 drifted when the sender fell below 20 Hz)
	float x, y, z;
	float pitch, yaw;
	unsigned char action;		// eCoopNetAction
	unsigned char fireCount;	// +1 each time firing starts (so a quick shot between samples is not lost)
	unsigned char meleeCount;	// +1 each time a melee attack starts
	unsigned char shotCount;	// +1 per round fired (counted from the gun's ammo going down)
	signed char lives;			// t.player[t.plrid].lives (clamped)
	unsigned char flags;		// COOPNET_PLAYERFLAG_*
	short health;				// t.player[t.plrid].health (host uses it to stop zombies attacking a dead client)
};

struct sCoopNetRemotePlayer
{
	bool bValid;
	int iPlayerID;
	sCoopNetPlayerState state;
	DWORD dwLastReceived;
	char pWeapon[COOPNET_WEAPON_NAME_MAX];	// gun name, "" = unarmed
};

static eCoopNetMode g_CoopNetMode = COOPNET_UNINITIALISED;
static ENetHost* g_pCoopNetHost = NULL;
static bool g_bCoopNetSteam = false;		// transport is Steam (M-CoopNetSteam.cpp) instead of a direct ENet connection
static bool g_bCoopNetSteamStarted = false;	// Steam was available (else retried: the player may start Steam late)
static int g_iCoopNetLocalID = 0;			// our player id: host 0, a client the id the host gave it (COOPNET_NO_PLAYER until then)
// session state over all connections: client = the host connection, host = at least one client
static bool g_bCoopNetConnected = false;	// another player is connected (either transport)
static unsigned int g_uCoopNetLastZombieSeq = 0;	// client: newest ZOMBIES packet (Steam's unreliable packets can arrive out of order)
static ENetAddress g_CoopNetAddress;		// host: bind address, client: host to join
static char g_pCoopNetJoinName[256] = "";
static bool g_bCoopNetHandshakeDone = false;	// ... and has completed the HELLO exchange
static bool g_bCoopNetHostBindFailed = false;
static char g_pCoopNetLastEvent[256] = "";
static DWORD g_dwCoopNetStartTime = 0;
static DWORD g_dwCoopNetNextSend = 0;
static DWORD g_dwCoopNetLastConnectAttempt = 0;
static unsigned int g_uCoopNetSendSeq = 0;
static sCoopNetPlayerState g_CoopNetLocal;
static bool g_bCoopNetLocalValid = false;
static char g_pCoopNetLocalWeapon[COOPNET_WEAPON_NAME_MAX] = "";
static unsigned char g_uCoopNetLastLocalAction = COOPNET_ACTION_NONE;
static int g_iCoopNetLastAmmoGun = 0, g_iCoopNetLastAmmo = -1;	// shot counting from ammo
static bool g_bCoopNetOutOfLives = false;	// lives used up in co-op: spectating instead of game over

// per-second stats
static DWORD g_dwCoopNetStatsWindow = 0;
static int g_iCoopNetTxCount = 0, g_iCoopNetRxCount = 0;
static int g_iCoopNetTxRate = 0, g_iCoopNetRxRate = 0;
static DWORD g_dwCoopNetNextRemoteLog = 0;

static FILE* g_pCoopNetLogFile = NULL;

// remote player puppet: drawn COOPNET_INTERP_DELAY_MS in the past, blending
// between the two received snapshots either side of that time
// two copies on one PC run at low frame rates, so packets are picked up in bursts
// (logged ping ~200 ms locally); snapshots are timed by sequence number, not arrival
#define COOPNET_INTERP_DELAY_MS		150
#define COOPNET_PUPPET_HIDE_MS		3000
#define COOPNET_SNAPSHOT_COUNT		16
// fallback grey box when the level has no Player2 entity; object IDs are hand
// assigned (Common.cpp offsets) and 69469 is free, just below the ragdoll range
#define COOPNET_PLACEHOLDER_OBJ		69469
#define COOPNET_PLACEHOLDER_FOR(id)	( COOPNET_PLACEHOLDER_OBJ - (id) )	// one box per player id: 69469..69466

struct sCoopNetSnapshot
{
	DWORD dwArrived;
	sCoopNetPlayerState state;
};

// Puppet entities: a level may contain characters named Player1..Player4. Player id n is shown
// as Player<n+1> where that exists, else on any free one (each machine never shows itself, so
// N players need any N-1 of them). Unused ones (and all in single player) are hidden.
// No standard MAX animation set has both gun poses and melee swings, so each may
// have a ...Melee twin (same character, melee animation set) that is shown instead
// while the remote holds a melee weapon or nothing. Slot n+4 is the twin of slot n.
#define COOPNET_PUPPET_NAME_COUNT	8
static const char* g_pCoopNetPuppetNames[COOPNET_PUPPET_NAME_COUNT] = { "Player1", "Player2", "Player3", "Player4", "Player1Melee", "Player2Melee", "Player3Melee", "Player4Melee" };
#define COOPNET_PUPPET_MELEE_TWIN	4
#define COOPNET_ENTITY_SEARCH_MS	1000
// player capsule centre -> character feet. Tested: 35 = feet on the floor, 50 = knee deep.
// (the "above terrain" log reads 48-51 in levels whose floors are above the terrain, so it
// is not a reliable measure; the remote eye looking low on the model is a size mismatch)
#define COOPNET_CAPSULE_TO_FEET		35.0f
// puppet animation choice by horizontal speed (units per second)
// measured: walking ~120-160, sprinting ~280 units/s. Separate up/down thresholds
// plus a minimum hold time stop the state flickering (each change restarts the anim)
#define COOPNET_ANIM_WALK_SPEED		30.0f
#define COOPNET_ANIM_WALK_STOP		15.0f
#define COOPNET_ANIM_RUN_SPEED		210.0f
#define COOPNET_ANIM_RUN_STOP		180.0f
#define COOPNET_ANIM_MIN_HOLD_MS	250
#define COOPNET_SPEED_WINDOW		4		// snapshots (~200 ms) used to measure speed

enum eCoopNetPuppetAnim
{
	COOPNET_ANIM_NONE = -1,
	COOPNET_ANIM_IDLE = 0,
	COOPNET_ANIM_WALK,
	COOPNET_ANIM_RUN,
	COOPNET_ANIM_IDLE_AIM,		// holding a gun (not a melee weapon)
	COOPNET_ANIM_WALK_AIM,
	COOPNET_ANIM_RUN_AIM,
	COOPNET_ANIM_SHOOT,			// standing and firing
	COOPNET_ANIM_MELEE,			// one-shot swing, plays to the end
	COOPNET_ANIM_RELOAD,		// standing and reloading a gun
	COOPNET_ANIM_DEATH,			// remote player died: one-shot death animation, held on the last frame
	COOPNET_ANIM_COUNT
};
static const char* g_pCoopNetAnimStateNames[COOPNET_ANIM_COUNT + 1] = { "none", "idle", "walk", "run", "idle aim", "walk aim", "run aim", "shoot", "melee", "reload", "death" };
#define COOPNET_FIRE_HOLD_MS		400		// keep the shoot pose this long after a shot
#define COOPNET_MELEE_MAX_MS		5000	// give up waiting for a swing to finish after this

struct sCoopNetClaimedEntity
{
	int e;				// entity element index (0 = not found in this level)
	bool bSetupDone;	// collision/physics already switched off
	bool bAnimsLogged;	// its animation names written to the log
	char pAnimSet[MAX_PATH];	// animation set file currently appended ("" = not set by us yet)
};
static sCoopNetClaimedEntity g_CoopNetClaimed[COOPNET_PUPPET_NAME_COUNT];
static DWORD g_dwCoopNetNextEntitySearch = 0;
static int g_iCoopNetCharactersLoggedFor = -1;	// entityelementlist size when the character list was last logged

struct sCoopNetPuppet
{
	int iObj;			// object being driven (0 = none)
	int iEntity;		// Player1/Player2 entity being driven (0 = placeholder box)
	int iSlot;			// its index in g_CoopNetClaimed (-1 = none)
	bool bVisible;
	bool bPlaceholderBlocked;	// placeholder ID already taken by something else
	sCoopNetPlayerState shown;	// interpolated state currently displayed
	float fLastX, fLastZ;		// for speed -> animation choice
	DWORD dwLastShown;
	float fSpeed;				// horizontal speed from recent snapshots, units per second
	DWORD dwAnimChanged;		// when the locomotion state last changed (min hold)
	int iLoco;					// COOPNET_ANIM_IDLE / WALK / RUN after hysteresis
	int iAnim;					// eCoopNetPuppetAnim currently looping
	int iGunShown;				// gun index currently held (0 = none)
	char pGunMissing[COOPNET_WEAPON_NAME_MAX];	// last name we failed to show (log once)
	bool bCountsSeen;			// fire/melee counters initialised (don't replay old swings on join)
	unsigned char uLastFireCount, uLastMeleeCount;
	DWORD dwFireUntil;			// show shooting until this time
	DWORD dwMeleeUntil;			// melee swing lock ends by this time at the latest
	int iMeleeToggle;			// alternate Attack_01 / Attack_02
	bool bRemoteWasDead;		// for spotting a respawn (teleport, don't glide there)
	bool bDeathAnimMissing;		// character has no Death_* animation: hide it while dead instead
	bool bDeathShown;			// death animation already played this death: on a re-pick (twin swap) start on its last frame
	bool bShotsSeen;			// shot counter initialised (don't replay old shots on join)
	unsigned char uLastShotCount;
	int iPendingShots;			// rounds received but not yet shown (one effect per frame at most)
	DWORD dwNextShotFx;
	int iFireSound;				// looping fire sound playing for an automatic weapon (0 = none)
	DWORD dwFireSoundExpiry;
	bool bAnimFound;			// diagnostics: last animation lookup
	float fAnimStart, fAnimFinish;
};
// One slot per player id. The connection fields are used for direct connections only (host: each
// client; client: the host as player 0 - other clients reach a client through the host).
struct sCoopNetPlayerSlot
{
	ENetPeer* pPeer;			// direct IP connection to this player (NULL over Steam / relayed)
	bool bConnected;			// connected straight to us
	bool bHandshakeDone;		// ... and HELLO exchanged
	bool bLoading;				// said it's loading a level (LOADING), nothing heard from it since
	bool bAtExit;				// host: reached the level exit (see "Level exits")
	sCoopNetRemotePlayer remote;
	sCoopNetSnapshot snapshots[COOPNET_SNAPSHOT_COUNT];	// ring buffer, oldest first from iSnapStart
	int iSnapStart, iSnapCount;
	// Snapshot times are on the sender's clock (sendTime in each packet). The offset to local time
	// follows the least-delayed packet, creeping up 1 ms per second only to follow clock drift
	// between the PCs (at 1 ms per packet the timeline itself sawtoothed by tens of ms and
	// everything drawn from it jerked), so bursty arrival no longer bunches snapshots.
	bool bTimeOffsetValid;
	DWORD dwTimeOffset;
	int iTimeOffsetCreep;
	sCoopNetPuppet puppet;
	int iPuppetBase;			// claimed entity slot used for this player this frame (-1 = placeholder box)
};
static sCoopNetPlayerSlot g_CoopNetPlayers[COOPNET_MAX_PLAYERS];

// The puppet/snapshot code works on one remote player at a time: g_iCoopNetCur selects it, and
// these names refer to that player's slot (set it before calling that code - see the loops in
// coopnet_update and the packet handlers).
static int g_iCoopNetCur = 1;
#define g_CoopNetRemote				( g_CoopNetPlayers[g_iCoopNetCur].remote )
#define g_CoopNetSnapshots			( g_CoopNetPlayers[g_iCoopNetCur].snapshots )
#define g_iCoopNetSnapshotStart		( g_CoopNetPlayers[g_iCoopNetCur].iSnapStart )
#define g_iCoopNetSnapshotCount		( g_CoopNetPlayers[g_iCoopNetCur].iSnapCount )
#define g_bCoopNetTimeOffsetValid	( g_CoopNetPlayers[g_iCoopNetCur].bTimeOffsetValid )
#define g_dwCoopNetTimeOffset		( g_CoopNetPlayers[g_iCoopNetCur].dwTimeOffset )
#define g_iCoopNetTimeOffsetCreep	( g_CoopNetPlayers[g_iCoopNetCur].iTimeOffsetCreep )
#define g_CoopNetPuppet				( g_CoopNetPlayers[g_iCoopNetCur].puppet )

static bool coopnet_isremoteplayer ( int id ) { return id >= 0 && id < COOPNET_MAX_PLAYERS && id != g_iCoopNetLocalID; }

static bool g_bCoopNetSpectating = false;	// dead and watching through a teammate's eyes (their puppet hidden; see coopnet_updatedeath)
static int g_iCoopNetSpectateID = -1;		// ... that teammate's player id

// held weapon models, made once per gun with the engine's own entity_createattachment
// (which also pre-spawns a hidden loot drop per call, so never call it per switch)
#define COOPNET_WEAPON_CACHE_MAX	32
struct sCoopNetWeaponCache
{
	int iEntity;	// per entity: swapping between a puppet and its melee twin must not remake guns
	int iGun;
	int iObj;
};
static sCoopNetWeaponCache g_CoopNetWeaponCache[COOPNET_WEAPON_CACHE_MAX];
static int g_iCoopNetWeaponCacheCount = 0;

static void coopnet_addsnapshot ( const sCoopNetPlayerState& s, DWORD dwArrived )
{
	DWORD dwSenderTime = s.sendTime;
	DWORD dwCandidate = dwArrived - dwSenderTime;
	if ( !g_bCoopNetTimeOffsetValid || (int)( dwCandidate - g_dwCoopNetTimeOffset ) < 0 )
		g_dwCoopNetTimeOffset = dwCandidate;
	else if ( ++g_iCoopNetTimeOffsetCreep >= 1000 / COOPNET_SEND_INTERVAL_MS )
	{
		g_iCoopNetTimeOffsetCreep = 0;
		g_dwCoopNetTimeOffset++;
	}
	g_bCoopNetTimeOffsetValid = true;
	dwArrived = dwSenderTime;

	// unreliable channel is already sequenced by ENet, but ignore anything not newer
	if ( g_iCoopNetSnapshotCount > 0 )
	{
		int iNewest = ( g_iCoopNetSnapshotStart + g_iCoopNetSnapshotCount - 1 ) % COOPNET_SNAPSHOT_COUNT;
		if ( (int)( s.seq - g_CoopNetSnapshots[iNewest].state.seq ) <= 0 ) return;
	}
	if ( g_iCoopNetSnapshotCount == COOPNET_SNAPSHOT_COUNT )
	{
		g_iCoopNetSnapshotStart = ( g_iCoopNetSnapshotStart + 1 ) % COOPNET_SNAPSHOT_COUNT;
		g_iCoopNetSnapshotCount--;
	}
	int iSlot = ( g_iCoopNetSnapshotStart + g_iCoopNetSnapshotCount ) % COOPNET_SNAPSHOT_COUNT;
	g_CoopNetSnapshots[iSlot].dwArrived = dwArrived;
	g_CoopNetSnapshots[iSlot].state = s;
	g_iCoopNetSnapshotCount++;
}

static float coopnet_lerpangle ( float a, float b, float f )
{
	// shortest way round, so 350 -> 10 turns 20 degrees, not 340
	float d = fmodf ( b - a + 540.0f, 360.0f ) - 180.0f;
	return a + d * f;
}

static bool coopnet_interpolate ( DWORD dwRenderTime, sCoopNetPlayerState* pOut )
{
	if ( g_iCoopNetSnapshotCount == 0 ) return false;
	const sCoopNetSnapshot* pOlder = NULL;
	for ( int i = 0; i < g_iCoopNetSnapshotCount; i++ )
	{
		const sCoopNetSnapshot* pSnap = &g_CoopNetSnapshots[( g_iCoopNetSnapshotStart + i ) % COOPNET_SNAPSHOT_COUNT];
		if ( (int)( pSnap->dwArrived - dwRenderTime ) > 0 )
		{
			// first snapshot after render time: blend from the one before (or hold if none)
			if ( pOlder == NULL ) { *pOut = pSnap->state; return true; }
			float fSpan = (float)( pSnap->dwArrived - pOlder->dwArrived );
			float f = fSpan > 0.0f ? (float)( dwRenderTime - pOlder->dwArrived ) / fSpan : 1.0f;
			pOut->seq = pSnap->state.seq;
			pOut->x = pOlder->state.x + ( pSnap->state.x - pOlder->state.x ) * f;
			pOut->y = pOlder->state.y + ( pSnap->state.y - pOlder->state.y ) * f;
			pOut->z = pOlder->state.z + ( pSnap->state.z - pOlder->state.z ) * f;
			pOut->pitch = pOlder->state.pitch + ( pSnap->state.pitch - pOlder->state.pitch ) * f;
			pOut->yaw = coopnet_lerpangle ( pOlder->state.yaw, pSnap->state.yaw, f );
			pOut->action = pSnap->state.action;
			pOut->fireCount = pSnap->state.fireCount;
			pOut->meleeCount = pSnap->state.meleeCount;
			pOut->health = pSnap->state.health;
			pOut->shotCount = pSnap->state.shotCount;
			pOut->lives = pSnap->state.lives;
			pOut->flags = pSnap->state.flags;
			return true;
		}
		pOlder = pSnap;
	}
	// render time is past the newest snapshot (late packets): hold the newest, no extrapolation
	*pOut = pOlder->state;
	return true;
}

//
// Logging
//

static void coopnet_log ( const char* pFormat, ... )
{
	char pLine[512];
	va_list args;
	va_start ( args, pFormat );
	vsnprintf ( pLine, sizeof(pLine), pFormat, args );
	va_end ( args );
	if ( g_pCoopNetLogFile )
	{
		DWORD dwElapsed = timeGetTime() - g_dwCoopNetStartTime;
		fprintf ( g_pCoopNetLogFile, "[%7.2f] %s\n", dwElapsed / 1000.0f, pLine );
		fflush ( g_pCoopNetLogFile );
	}
}

static void coopnet_event ( const char* pFormat, ... )
{
	// important events: coopnet log + engine log + shown on overlay
	va_list args;
	va_start ( args, pFormat );
	vsnprintf ( g_pCoopNetLastEvent, sizeof(g_pCoopNetLastEvent), pFormat, args );
	va_end ( args );
	coopnet_log ( "%s", g_pCoopNetLastEvent );
	char pEngineLog[300];
	sprintf ( pEngineLog, "coopnet: %s", g_pCoopNetLastEvent );
	timestampactivity ( 0, pEngineLog );
}

//
// Settings (per project, see sCoopNetSettings in M-CoopNet.h)
//

static sCoopNetSettings g_CoopNetSettings;			// in effect: our project's file, then the host's gameplay switches
static bool g_bCoopNetSettingsInit = false;
static char g_pCoopNetSettingsProject[256] = "";	// project the settings were read for

void coopnet_settingsdefaults ( sCoopNetSettings* p )
{
	memset ( p, 0, sizeof(*p) );
	p->iMaxPlayers = COOPNET_MAX_PLAYERS;
	p->bSharePickups = true;
	p->bSyncDoors = true;
	p->bShareCheckpoints = true;
	p->bSpectateOutOfLives = true;
	p->bExitNeedsAll = true;
	p->bFileCheck = true;
	p->bCheatCheck = true;
	p->iPort = COOPNET_DEFAULT_PORT;
	p->bExportCoopFiles = true;
}

void coopnet_settingsfile ( const char* pProject, char* pOut, bool bForWrite )
{
	// the project folder is in the writable (documents) area when it exists there, else under the exe
	char pFolder[MAX_PATH];
	snprintf ( pFolder, MAX_PATH, "%s\\Files\\projectbank\\%s", g.fpscrootdir_s.Get(), pProject );
	GG_GetRealPath ( pFolder, bForWrite ? 1 : 0 );
	snprintf ( pOut, MAX_PATH, "%s\\coopsettings.ini", pFolder );
}

bool coopnet_loadsettings ( const char* pProject, sCoopNetSettings* p )
{
	coopnet_settingsdefaults ( p );
	if ( pProject == NULL || pProject[0] == 0 ) return false;
	char pFile[MAX_PATH];
	coopnet_settingsfile ( pProject, pFile, false );
	if ( GetFileAttributesA ( pFile ) == INVALID_FILE_ATTRIBUTES ) return false;
	char pValue[COOPNET_SETTINGS_TEXT_MAX];
	#define COOPNET_INI_INT(sec,key,def) GetPrivateProfileIntA ( sec, key, def, pFile )
	#define COOPNET_INI_STR(sec,key,dest) { GetPrivateProfileStringA ( sec, key, dest, pValue, sizeof(pValue), pFile ); strncpy ( dest, pValue, sizeof(dest) - 1 ); dest[sizeof(dest) - 1] = 0; }
	p->iMaxPlayers = COOPNET_INI_INT ( "players", "max", p->iMaxPlayers );
	if ( p->iMaxPlayers < 2 ) p->iMaxPlayers = 2;
	if ( p->iMaxPlayers > COOPNET_MAX_PLAYERS ) p->iMaxPlayers = COOPNET_MAX_PLAYERS;
	p->bSharePickups = COOPNET_INI_INT ( "gameplay", "sharepickups", p->bSharePickups ) != 0;
	p->bSyncDoors = COOPNET_INI_INT ( "gameplay", "syncdoors", p->bSyncDoors ) != 0;
	p->bShareCheckpoints = COOPNET_INI_INT ( "gameplay", "sharecheckpoints", p->bShareCheckpoints ) != 0;
	p->bSpectateOutOfLives = COOPNET_INI_INT ( "gameplay", "spectateoutoflives", p->bSpectateOutOfLives ) != 0;
	p->bExitNeedsAll = COOPNET_INI_INT ( "gameplay", "exitneedsall", p->bExitNeedsAll ) != 0;
	p->bFileCheck = COOPNET_INI_INT ( "anticheat", "filecheck", p->bFileCheck ) != 0;
	p->bCheatCheck = COOPNET_INI_INT ( "anticheat", "cheatcheck", p->bCheatCheck ) != 0;
	COOPNET_INI_STR ( "launcher", "title", p->pTitle );
	COOPNET_INI_STR ( "launcher", "accent", p->pAccent );
	COOPNET_INI_STR ( "launcher", "host", p->pHostAddress );
	p->iPort = COOPNET_INI_INT ( "launcher", "port", p->iPort );
	if ( p->iPort <= 0 || p->iPort > 65535 ) p->iPort = COOPNET_DEFAULT_PORT;
	p->bExportCoopFiles = COOPNET_INI_INT ( "export", "coopfiles", p->bExportCoopFiles ) != 0;
	#undef COOPNET_INI_INT
	#undef COOPNET_INI_STR
	return true;
}

bool coopnet_savesettings ( const char* pProject, const sCoopNetSettings* p )
{
	if ( pProject == NULL || pProject[0] == 0 ) return false;
	char pFile[MAX_PATH];
	coopnet_settingsfile ( pProject, pFile, true );
	FILE* f = fopen ( pFile, "w" );
	if ( f == NULL ) return false;
	fprintf ( f, "; Co-op settings for this project - edit them in the editor: Co-op menu > Co-op Settings\n" );
	fprintf ( f, "[players]\nmax=%d\n\n", p->iMaxPlayers );
	fprintf ( f, "[gameplay]\nsharepickups=%d\nsyncdoors=%d\nsharecheckpoints=%d\nspectateoutoflives=%d\n",
		p->bSharePickups, p->bSyncDoors, p->bShareCheckpoints, p->bSpectateOutOfLives );
	fprintf ( f, "; level exit (win zone): 1 = every player must reach it, 0 = the first one takes everyone\nexitneedsall=%d\n\n", p->bExitNeedsAll );
	fprintf ( f, "[anticheat]\nfilecheck=%d\ncheatcheck=%d\n\n", p->bFileCheck, p->bCheatCheck );
	fprintf ( f, "[launcher]\ntitle=%s\naccent=%s\nhost=%s\nport=%d\n\n", p->pTitle, p->pAccent, p->pHostAddress, p->iPort );
	fprintf ( f, "[export]\ncoopfiles=%d\n", p->bExportCoopFiles );
	fclose ( f );
	return true;
}

bool CheckForWorkshopFile ( LPSTR VirtualFilename );	// SteamCheckForWorkshop.cpp: finds the _e_ (encrypted) copy

static bool coopnet_gamefileexists ( const char* pRelative )
{
	// a game file relative to Files (the current folder in game): in the writable area, under the
	// exe, or encrypted (_e_ name) in an exported game
	char pPath[MAX_PATH];
	strncpy ( pPath, pRelative, MAX_PATH - 1 ); pPath[MAX_PATH - 1] = 0;
	if ( CheckForWorkshopFile ( pPath ) ) return true;
	strncpy ( pPath, pRelative, MAX_PATH - 1 ); pPath[MAX_PATH - 1] = 0;
	GG_GetRealPath ( pPath, 0 );
	return GetFileAttributesA ( pPath ) != INVALID_FILE_ATTRIBUTES;
}

static int g_iCoopNetHostSettings = -1;	// client: the host's COOPNET_SETTING_* bits (-1 = not received)

static void coopnet_applyhostsettings ( void )
{
	if ( g_iCoopNetHostSettings < 0 ) return;
	int iBits = g_iCoopNetHostSettings;
	g_CoopNetSettings.bSharePickups = ( iBits & COOPNET_SETTING_PICKUPS ) != 0;
	g_CoopNetSettings.bSyncDoors = ( iBits & COOPNET_SETTING_DOORS ) != 0;
	g_CoopNetSettings.bShareCheckpoints = ( iBits & COOPNET_SETTING_CHECKPOINTS ) != 0;
	g_CoopNetSettings.bSpectateOutOfLives = ( iBits & COOPNET_SETTING_SPECTATE ) != 0;
	g_CoopNetSettings.bCheatCheck = ( iBits & COOPNET_SETTING_CHEATCHECK ) != 0;
	g_CoopNetSettings.bExitNeedsAll = ( iBits & COOPNET_SETTING_EXITALL ) != 0;
}

static void coopnet_updatesettings ( void )
{
	// read when the project is known (the storyboard loads after co-op starts), again if it changes
	const char* pProject = coopnet_projectname();
	if ( g_bCoopNetSettingsInit && strcmp ( g_pCoopNetSettingsProject, pProject ) == 0 ) return;
	bool bFirst = !g_bCoopNetSettingsInit;
	g_bCoopNetSettingsInit = true;
	strncpy ( g_pCoopNetSettingsProject, pProject, sizeof(g_pCoopNetSettingsProject) - 1 );
	bool bFound = coopnet_loadsettings ( g_pCoopNetSettingsProject, &g_CoopNetSettings );
	coopnet_applyhostsettings();	// joined already: the host's switches stay in effect
	if ( bFirst || bFound )
		coopnet_log ( "settings: %s (project '%s') - up to %d players, pickups %d, doors %d, checkpoints %d, spectate %d, exit needs all %d, file check %d, cheat check %d",
			bFound ? "coopsettings.ini" : "defaults", g_pCoopNetSettingsProject, g_CoopNetSettings.iMaxPlayers, g_CoopNetSettings.bSharePickups,
			g_CoopNetSettings.bSyncDoors, g_CoopNetSettings.bShareCheckpoints, g_CoopNetSettings.bSpectateOutOfLives,
			g_CoopNetSettings.bExitNeedsAll, g_CoopNetSettings.bFileCheck, g_CoopNetSettings.bCheatCheck );
}

static void coopnet_openlog ( const char* pRole )
{
	char pPath[MAX_PATH];
	GetModuleFileNameA ( NULL, pPath, MAX_PATH );
	char* pSlash = strrchr ( pPath, '\\' );
	if ( pSlash ) pSlash[1] = 0; else pPath[0] = 0;
	// several copies on one PC (testing 3-4 players): each takes the first log file no other copy
	// has open - coopnet-client.log, coopnet-client2.log, ...
	size_t uFolder = strlen ( pPath );
	for ( int n = 1; n <= COOPNET_MAX_PLAYERS && g_pCoopNetLogFile == NULL; n++ )
	{
		pPath[uFolder] = 0;
		char pName[64];
		if ( n == 1 ) sprintf ( pName, "coopnet-%s.log", pRole ); else sprintf ( pName, "coopnet-%s%d.log", pRole, n );
		strcat ( pPath, pName );
		g_pCoopNetLogFile = _fsopen ( pPath, "w", _SH_DENYWR );
	}
}

//
// Packet writing/reading (both ends are x86/x64 Windows, so native little-endian)
//

struct sCoopNetWriter
{
	unsigned char buf[8192];	// ZOMBIES packets carry every synced character (ENet fragments if needed)
	size_t len;
	bool bOverflow;
	sCoopNetWriter() { len = 0; bOverflow = false; }
	template <typename T> void put ( T v ) { if ( len + sizeof(T) > sizeof(buf) ) { bOverflow = true; return; } memcpy ( buf + len, &v, sizeof(T) ); len += sizeof(T); }
};

struct sCoopNetReader
{
	const unsigned char* buf;
	size_t len, pos;
	bool bOK;
	sCoopNetReader ( const unsigned char* b, size_t l ) { buf = b; len = l; pos = 0; bOK = true; }
	template <typename T> T get ( void )
	{
		T v = 0;
		if ( pos + sizeof(T) > len ) { bOK = false; return v; }
		memcpy ( &v, buf + pos, sizeof(T) ); pos += sizeof(T);
		return v;
	}
	void getraw ( void* p, size_t n )
	{
		if ( pos + n > len ) { bOK = false; memset ( p, 0, n ); return; }
		memcpy ( p, buf + pos, n ); pos += n;
	}
};

// str in the message comments: u8 length + chars (no terminator), cut to 255
static void coopnet_putstring ( sCoopNetWriter& w, const char* p )
{
	size_t n = strlen ( p );
	if ( n > 255 ) n = 255;
	w.put<unsigned char> ( (unsigned char)n );
	for ( size_t i = 0; i < n; i++ ) w.put<char> ( p[i] );
}

static void coopnet_getstring ( sCoopNetReader& r, char* pOut, size_t uSize )
{
	unsigned char n = r.get<unsigned char>();
	size_t i = 0;
	for ( ; i < n && r.bOK; i++ )
	{
		char c = r.get<char>();
		if ( i < uSize - 1 ) pOut[i] = c;
	}
	pOut[i < uSize - 1 ? i : uSize - 1] = 0;
}

static int g_iCoopNetFromPlayer = -1;	// player id whose packet is being handled (-1 = none; see coopnet_levelsmatch)

static void coopnet_sendto ( int iTo, const void* pData, size_t uLen, enet_uint8 channel, enet_uint32 flags, int iExcept = -1, bool bNeedHandshake = true )
{
	// iTo = a player id, or -1 = every player connected straight to us (a client's only
	// connection is the host, which passes things on) except iExcept
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		if ( ( iTo >= 0 && id != iTo ) || id == iExcept || !coopnet_isremoteplayer ( id ) ) continue;
		sCoopNetPlayerSlot& p = g_CoopNetPlayers[id];
		if ( !p.bConnected || ( bNeedHandshake && !p.bHandshakeDone ) ) continue;
		if ( g_bCoopNetSteam )
		{
			// Steam: reliable = ordered stream; unreliable state packets carry their own sequence numbers
			if ( coopsteam_send ( id, pData, (int)uLen, ( flags & ENET_PACKET_FLAG_RELIABLE ) != 0 ) ) g_iCoopNetTxCount++;
			continue;
		}
		if ( p.pPeer == NULL ) continue;
		ENetPacket* pPacket = enet_packet_create ( pData, uLen, flags );
		if ( enet_peer_send ( p.pPeer, channel, pPacket ) == 0 )
			g_iCoopNetTxCount++;
		else
			enet_packet_destroy ( pPacket );
	}
}

static void coopnet_send ( sCoopNetWriter& w, enet_uint8 channel, enet_uint32 flags )
{
	// to everyone (a client: to the host)
	if ( !w.bOverflow ) coopnet_sendto ( -1, w.buf, w.len, channel, flags );
}

static void coopnet_sendtoplayer ( int id, sCoopNetWriter& w, enet_uint8 channel, enet_uint32 flags )
{
	if ( !w.bOverflow ) coopnet_sendto ( id, w.buf, w.len, channel, flags );
}

//
// Anti-cheat, light: a fingerprint of the files a player could edit to cheat (Lua scripts,
// weapon specs - plain text next to the game) goes in HELLO and the host refuses a mismatch;
// the host can kick players and block them (coop-blocklist.txt next to the exe)
//

static unsigned long long g_uCoopNetFilesHash = 0;
static int g_iCoopNetFilesCount = 0;
static int g_iCoopNetKickedReason = COOPNET_KICK_NONE;		// client: why the host sent us away (stop trying)
static DWORD g_dwCoopNetKickAt[COOPNET_MAX_PLAYERS];		// host: drop this player then (0 = no) - after the KICK got there

static void coopnet_collectfiles ( const std::string& sFolder, const std::string& sRel, const char* pExt, std::vector<std::string>& files )
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA ( ( sFolder + "\\" + sRel + "*" ).c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE ) return;
	do
	{
		std::string n = fd.cFileName;
		if ( n == "." || n == ".." ) continue;
		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) { coopnet_collectfiles ( sFolder, sRel + n + "\\", pExt, files ); continue; }
		size_t e = n.size(), x = strlen ( pExt );
		if ( e > x && _stricmp ( n.c_str() + e - x, pExt ) == 0 ) files.push_back ( sRel + n );
	} while ( FindNextFileA ( h, &fd ) );
	FindClose ( h );
}

static void coopnet_hashfiles ( void )
{
	// FNV-1a 64 over every file's path (lower case) and bytes, in path order: Files\scriptbank\*.lua
	// and Files\gamecore\guns\*.txt next to the exe (same files on every machine running the same build)
	char pExe[MAX_PATH];
	GetModuleFileNameA ( NULL, pExe, MAX_PATH );
	char* pSlash = strrchr ( pExe, '\\' );
	if ( pSlash ) *pSlash = 0;
	static const char* pFolders[][2] = { { "Files\\scriptbank", ".lua" }, { "Files\\gamecore\\guns", ".txt" } };
	unsigned long long h = 14695981039346656037ull;
	int iCount = 0;
	std::vector<char> data;
	for ( int f = 0; f < 2; f++ )
	{
		std::string sFolder = std::string ( pExe ) + "\\" + pFolders[f][0];
		std::vector<std::string> files;
		coopnet_collectfiles ( sFolder, "", pFolders[f][1], files );
		for ( size_t i = 0; i < files.size(); i++ ) for ( size_t c = 0; c < files[i].size(); c++ ) files[i][c] = (char)tolower ( (unsigned char)files[i][c] );
		std::sort ( files.begin(), files.end() );
		for ( size_t i = 0; i < files.size(); i++ )
		{
			std::string sPath = std::string ( pFolders[f][0] ) + "\\" + files[i];
			for ( size_t c = 0; c < sPath.size(); c++ ) { h ^= (unsigned char)sPath[c]; h *= 1099511628211ull; }
			FILE* fp = fopen ( ( std::string ( pExe ) + "\\" + sPath ).c_str(), "rb" );
			if ( fp == NULL ) continue;
			fseek ( fp, 0, SEEK_END ); long n = ftell ( fp ); fseek ( fp, 0, SEEK_SET );
			if ( n > 0 ) { data.resize ( n ); n = (long)fread ( &data[0], 1, n, fp ); for ( long b = 0; b < n; b++ ) { h ^= (unsigned char)data[b]; h *= 1099511628211ull; } }
			fclose ( fp );
			iCount++;
		}
	}
	g_uCoopNetFilesHash = h;
	g_iCoopNetFilesCount = iCount;
}

//
// Live checks during a co-op session (anyone, host included): known cheat tools running (the Mod
// Menu - its window, process or the files it leaves in scriptbank - and Cheat Engine), and the
// fingerprinted files changing after launch (a cheat script added once the startup fingerprint was
// taken). Either way co-op stops for this player until they restart the game, since a cheat
// script stays loaded once the level has run it. Single player is never checked.
//

static HANDLE g_hCoopNetWatch[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };	// change notifications: scriptbank, guns
static DWORD g_dwCoopNetRehashAt = 0;				// re-fingerprint then (0 = no) - a moment after a change
static DWORD g_dwCoopNetNextToolCheck = 0;
static char g_pCoopNetCheat[128] = "";				// what was found ("" = nothing)

static std::string coopnet_exefolder ( void )
{
	char pExe[MAX_PATH];
	GetModuleFileNameA ( NULL, pExe, MAX_PATH );
	char* pSlash = strrchr ( pExe, '\\' );
	if ( pSlash ) *pSlash = 0;
	return pExe;
}

static void coopnet_startfilewatch ( void )
{
	static const char* pFolders[2] = { "Files\\scriptbank", "Files\\gamecore\\guns" };
	for ( int f = 0; f < 2; f++ )
		g_hCoopNetWatch[f] = FindFirstChangeNotificationA ( ( coopnet_exefolder() + "\\" + pFolders[f] ).c_str(), TRUE,
			FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE );
}

static bool coopnet_findcheattool ( char* pWhat )
{
	// the Mod Menu's window, or a process named like a known cheat tool
	if ( FindWindowA ( NULL, "Mod Menu" ) ) { strcpy ( pWhat, "the Mod Menu is running" ); return true; }
	HANDLE hSnap = CreateToolhelp32Snapshot ( TH32CS_SNAPPROCESS, 0 );
	if ( hSnap != INVALID_HANDLE_VALUE )
	{
		PROCESSENTRY32W pe;
		pe.dwSize = sizeof(pe);
		bool bFound = false;
		for ( BOOL b = Process32FirstW ( hSnap, &pe ); b && !bFound; b = Process32NextW ( hSnap, &pe ) )
		{
			_wcslwr_s ( pe.szExeFile );
			if ( wcsstr ( pe.szExeFile, L"modmenu" ) ) { strcpy ( pWhat, "the Mod Menu is running" ); bFound = true; }
			else if ( wcsstr ( pe.szExeFile, L"cheatengine" ) || wcsstr ( pe.szExeFile, L"cheat engine" ) ) { strcpy ( pWhat, "Cheat Engine is running" ); bFound = true; }
		}
		CloseHandle ( hSnap );
		if ( bFound ) return true;
	}
	// what the Mod Menu leaves next to the game's scripts
	static const char* pFiles[] = { "modmenu.lua", "modmenu_cmd.txt", "modmenu_status.txt", "modmenu_entities.txt" };
	for ( int i = 0; i < 4; i++ )
		if ( GetFileAttributesA ( ( coopnet_exefolder() + "\\Files\\scriptbank\\" + pFiles[i] ).c_str() ) != INVALID_FILE_ATTRIBUTES )
		{
			strcpy ( pWhat, "Mod Menu files are in the game folder" );
			return true;
		}
	return false;
}

static void coopnet_sendhello ( int iTo )
{
	// host: tells the client which player it is; client: asks to join. Both: our files fingerprint
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_HELLO );
	w.put<unsigned short> ( COOPNET_PROTOCOL_VERSION );
	w.put<unsigned char> ( (unsigned char)( g_CoopNetMode == COOPNET_HOST ? iTo : COOPNET_NO_PLAYER ) );
	w.put<unsigned long long> ( g_uCoopNetFilesHash );
	coopnet_sendto ( iTo, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE, -1, false );
}

static void coopnet_sendplayerstate ( void )
{
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_PLAYERSTATE );
	w.put<unsigned char> ( (unsigned char)g_iCoopNetLocalID );
	w.put<unsigned int> ( ++g_uCoopNetSendSeq );
	w.put<unsigned int> ( timeGetTime() );
	w.put<float> ( g_CoopNetLocal.x );
	w.put<float> ( g_CoopNetLocal.y );
	w.put<float> ( g_CoopNetLocal.z );
	w.put<float> ( g_CoopNetLocal.pitch );
	w.put<float> ( g_CoopNetLocal.yaw );
	w.put<unsigned char> ( g_CoopNetLocal.action );
	w.put<unsigned char> ( g_CoopNetLocal.fireCount );
	w.put<unsigned char> ( g_CoopNetLocal.meleeCount );
	w.put<unsigned char> ( g_CoopNetLocal.shotCount );
	w.put<signed char> ( g_CoopNetLocal.lives );
	w.put<unsigned char> ( g_CoopNetLocal.flags );
	w.put<short> ( g_CoopNetLocal.health );
	coopnet_send ( w, COOPNET_CHANNEL_STATE, 0 );
}

static void coopnet_sendweaponof ( int id, const char* pWeapon, int iTo )
{
	// player id's weapon to iTo (-1 = everyone)
	sCoopNetWriter w;
	size_t n = strlen ( pWeapon );
	w.put<unsigned char> ( COOPNET_MSG_WEAPON );
	w.put<unsigned char> ( (unsigned char)id );
	w.put<unsigned char> ( (unsigned char)n );
	for ( size_t i = 0; i < n; i++ ) w.put<char> ( pWeapon[i] );
	coopnet_sendto ( iTo, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
}

static void coopnet_sendweapon ( void )
{
	coopnet_sendweaponof ( g_iCoopNetLocalID, g_pCoopNetLocalWeapon, -1 );
	coopnet_log ( "sent weapon '%s'", g_pCoopNetLocalWeapon );
}

// zombie sync data (used from the level check and peer reset below; code further down)
// must cover well over COOPNET_INTERP_DELAY_MS plus packet bunching: with 4 (150 ms) the
// render time sat at or before the oldest snapshot and zombies froze, then jumped
#define COOPNET_ZOMBIE_SNAPSHOTS	16
#define COOPNET_ZOMBIE_FRAME_SNAP	20.0f	// animation frames off: below this nudge playback speed, above it jump
#define COOPNET_ZOMBIE_FLAG_LOOP	1
#define COOPNET_ZOMBIE_FLAG_VISIBLE	2

struct sCoopNetZombieState
{
	DWORD dwTime;					// sender clock (sendTime in the packet), as player snapshots
	float x, y, z, ry;
	unsigned short animStart, animEnd;
	unsigned char flags;
	float frame;
	short speed;
};

struct sCoopNetZombie
{
	int iCount;						// snapshots held (newest last)
	sCoopNetZombieState snaps[COOPNET_ZOMBIE_SNAPSHOTS];
	bool bAnimApplied;
	unsigned short appliedStart, appliedEnd;
	unsigned char appliedLoop;
	short appliedSpeed;
	bool bDead;						// host said it died: the local engine death sequence owns it now
};
static std::vector<sCoopNetZombie> g_CoopNetZombies;	// indexed by entity
static std::vector<int> g_CoopNetHostHealth;			// host: last health seen per entity (death detection)
static bool g_bCoopNetReplaying = false;				// applying something the other side sent: don't send it back
static int g_iCoopNetZombiesSent = 0;		// host: characters in the last packet
static int g_iCoopNetZombiesMirrored = 0;	// client: characters mirrored this frame
// client diagnostics, per second: zombie-frames drawn before the oldest / after the newest
// snapshot (frozen), interpolated normally, and animation frame resyncs
static int g_iCoopNetZombieStatEarly = 0, g_iCoopNetZombieStatLate = 0, g_iCoopNetZombieStatOK = 0, g_iCoopNetZombieStatResync = 0, g_iCoopNetZombieStatAnimChange = 0;
static DWORD g_dwCoopNetNextZombieAnimLog = 0;

//
// Level check: zombies are addressed by entity index, so both sides must be in the same
// level with the same characters, or zombie sync stays off
//

struct sCoopNetLevelInfo
{
	bool bValid;
	unsigned int uFingerprint;		// FNV-1a over level file name + (index, type) of every character
	unsigned short uCharacters;
	char pName[64];					// level file name without folder (paths differ between PCs)
	char pPath[MAX_PATH];			// from "mapbank\" on, as level jumps take it ("" = not known)
};
static sCoopNetLevelInfo g_CoopNetLocalLevel;
static sCoopNetLevelInfo g_CoopNetRemoteLevels[COOPNET_MAX_PLAYERS];	// per direct connection (a client only compares with the host)
static DWORD g_dwCoopNetNextLevelCheck = 0;

static int coopnet_levelentitylimit ( void )
{
	// only entities placed in the level: ones spawned during play (SpawnNewEntity) get
	// indices above this, which can differ between the two machines
	int n = t.storedentityelementlist;
	if ( n <= 0 || n > g.entityelementlist ) n = g.entityelementlist;
	return n;
}

static void coopnet_fnv ( unsigned int* pHash, const void* pData, size_t n, bool bLower )
{
	const unsigned char* p = (const unsigned char*)pData;
	for ( size_t i = 0; i < n; i++ ) { *pHash ^= bLower ? (unsigned char)tolower ( p[i] ) : p[i]; *pHash *= 16777619u; }
}

static void coopnet_computelevel ( sCoopNetLevelInfo* pInfo )
{
	const char* pFull = g.projectfilename_s.Get();
	const char* pBase = pFull;
	for ( const char* p = pFull; *p; p++ ) if ( *p == '\\' || *p == '/' ) pBase = p + 1;
	strncpy ( pInfo->pName, pBase, sizeof(pInfo->pName) - 1 );
	pInfo->pName[sizeof(pInfo->pName) - 1] = 0;
	const char* pMapbank = pestrcasestr ( pFull, "mapbank\\" );
	strncpy ( pInfo->pPath, pMapbank ? pMapbank : "", sizeof(pInfo->pPath) - 1 );
	pInfo->pPath[sizeof(pInfo->pPath) - 1] = 0;
	unsigned int h = 2166136261u;
	coopnet_fnv ( &h, pInfo->pName, strlen ( pInfo->pName ), true );
	unsigned short uCount = 0;
	for ( int e = 1; e <= coopnet_levelentitylimit(); e++ )
	{
		int entid = t.entityelement[e].bankindex;
		if ( entid <= 0 || t.entityprofile[entid].ischaracter != 1 ) continue;
		coopnet_fnv ( &h, &e, sizeof(e), false );
		if ( entid < (int)t.entitybank_s.size() )
			coopnet_fnv ( &h, t.entitybank_s[entid].Get(), strlen ( t.entitybank_s[entid].Get() ), true );
		uCount++;
	}
	pInfo->uFingerprint = h;
	pInfo->uCharacters = uCount;
	pInfo->bValid = true;
}

static bool coopnet_levelmatchfor ( int id )
{
	const sCoopNetLevelInfo& r = g_CoopNetRemoteLevels[id];
	return g_CoopNetLocalLevel.bValid && r.bValid && g_CoopNetPlayers[id].bHandshakeDone
		&& g_CoopNetLocalLevel.uFingerprint == r.uFingerprint && g_CoopNetLocalLevel.uCharacters == r.uCharacters;
}

static bool coopnet_levelsmatch ( void )
{
	// handling a packet: the sender's connection; otherwise anyone connected straight to us is
	// on our level (a client: the host)
	if ( g_iCoopNetFromPlayer >= 0 ) return coopnet_levelmatchfor ( g_iCoopNetFromPlayer );
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		if ( coopnet_isremoteplayer ( id ) && coopnet_levelmatchfor ( id ) ) return true;
	return false;
}

static void coopnet_sendlevel ( int iTo = -1 )
{
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_LEVEL );
	w.put<unsigned int> ( g_CoopNetLocalLevel.uFingerprint );
	w.put<unsigned short> ( g_CoopNetLocalLevel.uCharacters );
	coopnet_putstring ( w, g_CoopNetLocalLevel.pName );
	coopnet_putstring ( w, g_CoopNetLocalLevel.pPath );
	coopnet_sendto ( iTo, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
}

static void coopnet_newlevel ( void );	// level exits section below

static void coopnet_updatelevel ( DWORD dwNow )
{
	// level changes reuse the session, so re-check once a second and tell the peer
	if ( (int)( dwNow - g_dwCoopNetNextLevelCheck ) < 0 ) return;
	g_dwCoopNetNextLevelCheck = dwNow + 1000;
	sCoopNetLevelInfo info;
	coopnet_computelevel ( &info );
	bool bChanged = !g_CoopNetLocalLevel.bValid || info.uFingerprint != g_CoopNetLocalLevel.uFingerprint;
	g_CoopNetLocalLevel = info;
	if ( bChanged )
	{
		g_CoopNetZombies.clear();	// new level: old snapshots refer to the previous entities
		g_CoopNetHostHealth.clear();
		coopnet_newlevel();
		coopnet_log ( "level '%s' characters %d fingerprint %08x", info.pName, info.uCharacters, info.uFingerprint );
		if ( g_bCoopNetConnected && g_bCoopNetHandshakeDone ) coopnet_sendlevel();
	}
}

static void coopnet_zombies_receive ( sCoopNetReader& r );	// zombie sync section below
static void coopnet_takedamage ( int e, int iDamage );		// targeting section below
static void coopnet_applyplayercommand ( int iCmd, int e, float fX, float fY, float fZ, int iValue );	// targeting section below
static void coopnet_undoremotefreeze ( void );					// targeting section below
static void coopnet_applyitemgone ( int e );							// items section below
static void coopnet_applydoor ( int e, int iOp );						// doors section below
static void coopnet_setframekeepplaying ( int iObj, float fFrame );		// zombie sync section below
struct sCoopNetHitInputs;
static void coopnet_receivehit ( sCoopNetReader& r );							// hits section below
static void coopnet_applyhit ( const sCoopNetHitInputs& h );					// hits section below
static void coopnet_applydeath ( int e, int iDir );							// zombie sync section below
static void coopnet_applybloodimpact ( float fX, float fY, float fZ );		// hits section below
static void coopnet_applyluacommand ( int iCmd, int e, float fValue );	// hits section below

static void coopnet_dropconnection ( int id, enet_uint32 uReason );	// connection management below
static void coopnet_kick ( int id, int iReason );					// connection management below
static void coopnet_leave ( void );									// connection management below
static void coopnet_updatesession ( void );							// connection management below
static void coopnet_resetplayer ( int id );							// connection management below
static void coopnet_settimeoutlong ( int id, bool bLong );			// connection management below

// level exits (section below coopnet_outoflives)
struct sCoopNetExitRequest
{
	bool bFinish;				// FinishLevel: the storyboard's next level, or the end of the game
	int iResetStates;
	char pTarget[MAX_PATH];		// JumpToLevel's level (t.tleveltojump_s)
	char pAdvance[MAX_PATH];	// t.game.pAdvanceWarningOfLevelFilename (set when that level exists)
};
static int g_iCoopNetExitAt = 0, g_iCoopNetExitNeeded = 0;	// players waiting at the level exit / needed (0 = nobody waiting)
static void coopnet_exitrequest ( int id, const sCoopNetExitRequest& q );
static void coopnet_exitgo ( const sCoopNetExitRequest& q );

// While we load a level, packets wait here (the level's entities are half built) and are handled
// in order once the game loop runs again. Position / zombie snapshots are dropped: newer ones follow.
struct sCoopNetHeldPacket { int iFrom; std::vector<unsigned char> data; };
static std::vector<sCoopNetHeldPacket> g_CoopNetHeldPackets;
static bool g_bCoopNetLoading = false;
static bool g_bCoopNetReplayingHeld = false;
#define COOPNET_HELD_MAX			4000
#define COOPNET_LOADING_TIMEOUT_MS	90000	// a player loading a level may go this long without answering

static void coopnet_receive ( const unsigned char* pData, size_t uLength, int iFrom )
{
	// iFrom = player id of the connection it came in on (a client: always the host, 0)
	g_iCoopNetRxCount++;
	sCoopNetReader r ( pData, uLength );
	unsigned char msg = r.get<unsigned char>();
	if ( msg == COOPNET_MSG_LOADING )
	{
		// the other end of this connection is loading a level: don't time it out meanwhile
		if ( !g_CoopNetPlayers[iFrom].bLoading ) coopnet_log ( "player %d is loading a level", iFrom + 1 );
		g_CoopNetPlayers[iFrom].bLoading = true;
		coopnet_settimeoutlong ( iFrom, true );
		return;
	}
	if ( g_bCoopNetLoading && !g_bCoopNetReplayingHeld )
	{
		if ( msg == COOPNET_MSG_PLAYERSTATE || msg == COOPNET_MSG_ZOMBIES || g_CoopNetHeldPackets.size() >= COOPNET_HELD_MAX ) return;
		sCoopNetHeldPacket h;
		h.iFrom = iFrom;
		h.data.assign ( pData, pData + uLength );
		g_CoopNetHeldPackets.push_back ( h );
		return;
	}
	if ( g_CoopNetPlayers[iFrom].bLoading )
	{
		// it's back
		g_CoopNetPlayers[iFrom].bLoading = false;
		coopnet_settimeoutlong ( iFrom, g_bCoopNetLoading );
	}
	// host: until a client has passed the HELLO checks (version, files) - or once it's being
	// kicked - nothing it sends counts
	if ( g_CoopNetMode == COOPNET_HOST && msg != COOPNET_MSG_HELLO && ( !g_CoopNetPlayers[iFrom].bHandshakeDone || g_dwCoopNetKickAt[iFrom] != 0 ) ) return;
	if ( g_CoopNetMode == COOPNET_HOST && msg == COOPNET_MSG_HELLO && g_dwCoopNetKickAt[iFrom] != 0 ) return;
	g_iCoopNetFromPlayer = iFrom;
	switch ( msg )
	{
		case COOPNET_MSG_HELLO:
		{
			unsigned short version = r.get<unsigned short>();
			unsigned char playerid = r.get<unsigned char>();
			if ( !r.bOK ) break;
			if ( version != COOPNET_PROTOCOL_VERSION )
			{
				coopnet_event ( "protocol mismatch (ours %d, theirs %d) - disconnecting", COOPNET_PROTOCOL_VERSION, version );
				coopnet_dropconnection ( iFrom, COOPNET_DISCONNECT_VERSION );
				break;
			}
			unsigned long long uFiles = r.get<unsigned long long>();
			if ( !r.bOK ) break;
			if ( g_CoopNetMode == COOPNET_HOST && !g_CoopNetPlayers[iFrom].bHandshakeDone )
			{
				// the project's player limit (Co-op Settings), host included
				int iPlayers = 1;
				for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ ) if ( g_CoopNetPlayers[id].bHandshakeDone ) iPlayers++;
				if ( iPlayers >= g_CoopNetSettings.iMaxPlayers )
				{
					coopnet_event ( "player %d refused - the game is full (%d players)", iFrom + 1, g_CoopNetSettings.iMaxPlayers );
					coopnet_kick ( iFrom, COOPNET_KICK_FULL );
					break;
				}
			}
			if ( g_CoopNetMode == COOPNET_HOST && g_CoopNetSettings.bFileCheck && uFiles != g_uCoopNetFilesHash )
			{
				// edited scripts / weapon files (or a different build of the game): not in our game
				coopnet_event ( "player %d's game files don't match ours (edited, or a different build) - refused", iFrom + 1 );
				coopnet_kick ( iFrom, COOPNET_KICK_FILES );
				break;
			}
			if ( g_CoopNetMode == COOPNET_HOST && g_pCoopNetCheat[0] )
			{
				// a cheat was found here - co-op is over until the host restarts the game
				coopnet_kick ( iFrom, COOPNET_KICK_HOSTCHEAT );
				break;
			}
			if ( g_CoopNetMode == COOPNET_CLIENT )
			{
				// the host tells us which player we are
				if ( playerid == 0 || playerid >= COOPNET_MAX_PLAYERS ) break;
				g_iCoopNetLocalID = playerid;
				coopnet_event ( "joined as player %d", playerid + 1 );
			}
			else
				coopnet_event ( "player %d joined", iFrom + 1 );
			g_CoopNetPlayers[iFrom].bHandshakeDone = true;
			coopnet_updatesession();
			// the new connection needs our current weapon and level (otherwise only sent on change),
			// and a new client the other clients' weapons too
			coopnet_sendweaponof ( g_iCoopNetLocalID, g_pCoopNetLocalWeapon, iFrom );
			if ( g_CoopNetLocalLevel.bValid ) coopnet_sendlevel ( iFrom );
			if ( g_CoopNetMode == COOPNET_HOST )
			{
				for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ )
					if ( id != iFrom && g_CoopNetPlayers[id].bHandshakeDone ) coopnet_sendweaponof ( id, g_CoopNetPlayers[id].remote.pWeapon, iFrom );
				// our gameplay switches are the game's
				sCoopNetWriter w;
				w.put<unsigned char> ( COOPNET_MSG_SETTINGS );
				w.put<unsigned char> ( (unsigned char)( ( g_CoopNetSettings.bSharePickups ? COOPNET_SETTING_PICKUPS : 0 )
					| ( g_CoopNetSettings.bSyncDoors ? COOPNET_SETTING_DOORS : 0 )
					| ( g_CoopNetSettings.bShareCheckpoints ? COOPNET_SETTING_CHECKPOINTS : 0 )
					| ( g_CoopNetSettings.bSpectateOutOfLives ? COOPNET_SETTING_SPECTATE : 0 )
					| ( g_CoopNetSettings.bCheatCheck ? COOPNET_SETTING_CHEATCHECK : 0 )
					| ( g_CoopNetSettings.bExitNeedsAll ? COOPNET_SETTING_EXITALL : 0 ) ) );
				coopnet_sendtoplayer ( iFrom, w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
			}
			break;
		}
		case COOPNET_MSG_SETTINGS:
		{
			int iBits = r.get<unsigned char>();
			if ( !r.bOK || g_CoopNetMode != COOPNET_CLIENT ) break;
			g_iCoopNetHostSettings = iBits;
			coopnet_applyhostsettings();
			coopnet_log ( "host settings: pickups %d, doors %d, checkpoints %d, spectate %d, exit needs all %d, cheat check %d", g_CoopNetSettings.bSharePickups,
				g_CoopNetSettings.bSyncDoors, g_CoopNetSettings.bShareCheckpoints, g_CoopNetSettings.bSpectateOutOfLives, g_CoopNetSettings.bExitNeedsAll,
				g_CoopNetSettings.bCheatCheck );
			break;
		}
		case COOPNET_MSG_LEVELEXIT:
		case COOPNET_MSG_LEVELGO:
		{
			sCoopNetExitRequest q;
			q.bFinish = r.get<unsigned char>() != 0;
			q.iResetStates = r.get<unsigned char>();
			coopnet_getstring ( r, q.pTarget, sizeof(q.pTarget) );
			coopnet_getstring ( r, q.pAdvance, sizeof(q.pAdvance) );
			if ( !r.bOK || !g_bCoopNetHandshakeDone ) break;
			if ( msg == COOPNET_MSG_LEVELEXIT && g_CoopNetMode == COOPNET_HOST ) coopnet_exitrequest ( iFrom, q );
			// (only for the level it was meant for - not one held while we loaded another)
			if ( msg == COOPNET_MSG_LEVELGO && g_CoopNetMode == COOPNET_CLIENT && coopnet_levelsmatch() ) coopnet_exitgo ( q );
			break;
		}
		case COOPNET_MSG_EXITSTATUS:
		{
			int iAt = r.get<unsigned char>(), iNeeded = r.get<unsigned char>();
			if ( !r.bOK || g_CoopNetMode != COOPNET_CLIENT ) break;
			g_iCoopNetExitAt = iAt;
			g_iCoopNetExitNeeded = iNeeded;
			break;
		}
		case COOPNET_MSG_KICK:
		{
			// the host sent us away: say why on screen and stop trying to join
			int iReason = r.get<unsigned char>();
			if ( !r.bOK ) break;
			if ( g_CoopNetMode == COOPNET_HOST )
			{
				// a client found a cheat on its PC and is leaving (its disconnect follows)
				if ( iReason == COOPNET_KICK_CHEAT ) coopnet_event ( "player %d left: a cheat was found on their PC", iFrom + 1 );
				break;
			}
			g_iCoopNetKickedReason = iReason != COOPNET_KICK_NONE ? iReason : COOPNET_KICK_KICKED;
			coopnet_event ( "the host %s us (reason %d)", iReason == COOPNET_KICK_FILES ? "refused" : "removed", iReason );
			g_iCoopNetFromPlayer = -1;
			coopnet_leave();
			return;
		}
		case COOPNET_MSG_PLAYERLEFT:
		{
			int id = r.get<unsigned char>();
			if ( !r.bOK || g_CoopNetMode != COOPNET_CLIENT || !coopnet_isremoteplayer ( id ) || id == 0 ) break;
			coopnet_resetplayer ( id );
			coopnet_event ( "player %d left", id + 1 );
			break;
		}
		case COOPNET_MSG_HIT:
		{
			if ( !g_bCoopNetHandshakeDone || g_CoopNetMode != COOPNET_HOST || !coopnet_levelsmatch() ) break;
			coopnet_receivehit ( r );
			break;
		}
		case COOPNET_MSG_DEATH:
		{
			int e = r.get<unsigned short>();
			int iDir = r.get<unsigned char>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || g_CoopNetMode != COOPNET_CLIENT || !coopnet_levelsmatch() ) break;
			coopnet_applydeath ( e, iDir );
			break;
		}
		case COOPNET_MSG_BLOODFX:
		{
			float fX = r.get<float>(), fY = r.get<float>(), fZ = r.get<float>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || !coopnet_levelsmatch() ) break;
			coopnet_applybloodimpact ( fX, fY, fZ );
			break;
		}
		case COOPNET_MSG_ITEMGONE:
		{
			int e = r.get<unsigned short>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || !coopnet_levelsmatch() || !g_CoopNetSettings.bSharePickups ) break;
			coopnet_applyitemgone ( e );
			break;
		}
		case COOPNET_MSG_DOOR:
		{
			int e = r.get<unsigned short>();
			int iOp = r.get<unsigned char>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || !coopnet_levelsmatch() || !g_CoopNetSettings.bSyncDoors ) break;
			coopnet_applydoor ( e, iOp );
			break;
		}
		case COOPNET_MSG_CHECKPOINT:
		{
			float fX = r.get<float>(), fY = r.get<float>(), fZ = r.get<float>(), fA = r.get<float>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || !coopnet_levelsmatch() || !g_CoopNetSettings.bShareCheckpoints ) break;
			// a teammate reached a checkpoint: it is ours too (respawn there), as entity_lua_checkpoint records it
			t.playercheckpoint.stored = 1;
			t.playercheckpoint.x = fX; t.playercheckpoint.y = fY; t.playercheckpoint.z = fZ; t.playercheckpoint.a = fA;
			coopnet_event ( "a teammate reached a checkpoint - shared" );
			break;
		}
		case COOPNET_MSG_LUACMD:
		{
			int iCmd = r.get<unsigned char>();
			int e = r.get<unsigned short>();
			float fValue = r.get<float>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || g_CoopNetMode != COOPNET_CLIENT || !coopnet_levelsmatch() ) break;
			coopnet_applyluacommand ( iCmd, e, fValue );
			break;
		}
		case COOPNET_MSG_PLAYERCMD:
		{
			int iCmd = r.get<unsigned char>();
			int e = r.get<unsigned short>();
			float fX = r.get<float>(), fY = r.get<float>(), fZ = r.get<float>();
			int iValue = r.get<int>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || g_CoopNetMode != COOPNET_CLIENT ) break;
			coopnet_applyplayercommand ( iCmd, e, fX, fY, fZ, iValue );
			break;
		}
		case COOPNET_MSG_DAMAGE:
		{
			int e = r.get<unsigned short>();
			int iDamage = r.get<short>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || g_CoopNetMode != COOPNET_CLIENT ) break;
			coopnet_takedamage ( e, iDamage );
			break;
		}
		case COOPNET_MSG_ZOMBIES:
			if ( g_bCoopNetHandshakeDone ) coopnet_zombies_receive ( r );
			break;
		case COOPNET_MSG_LEVEL:
		{
			sCoopNetLevelInfo info;
			info.uFingerprint = r.get<unsigned int>();
			info.uCharacters = r.get<unsigned short>();
			coopnet_getstring ( r, info.pName, sizeof(info.pName) );
			coopnet_getstring ( r, info.pPath, sizeof(info.pPath) );
			if ( !r.bOK || !g_bCoopNetHandshakeDone ) break;
			info.bValid = true;
			g_CoopNetRemoteLevels[iFrom] = info;
			if ( coopnet_levelsmatch() )
				coopnet_event ( "player %d is on our level ('%s', %d characters) - zombie sync on", iFrom + 1, info.pName, info.uCharacters );
			else
				coopnet_event ( "LEVEL MISMATCH: player %d '%s' (%d characters, %08x) vs ours '%s' (%d, %08x) - zombie sync off",
					iFrom + 1, info.pName, info.uCharacters, info.uFingerprint, g_CoopNetLocalLevel.pName, g_CoopNetLocalLevel.uCharacters, g_CoopNetLocalLevel.uFingerprint );
			break;
		}
		case COOPNET_MSG_WEAPON:
		{
			int id = r.get<unsigned char>();
			if ( g_CoopNetMode == COOPNET_HOST ) id = iFrom;	// a client only speaks for itself
			unsigned char n = r.get<unsigned char>();
			char pName[COOPNET_WEAPON_NAME_MAX];
			int i = 0;
			for ( ; i < n && r.bOK; i++ )
			{
				char c = r.get<char>();
				if ( i < COOPNET_WEAPON_NAME_MAX - 1 ) pName[i] = c;
			}
			pName[i < COOPNET_WEAPON_NAME_MAX - 1 ? i : COOPNET_WEAPON_NAME_MAX - 1] = 0;
			if ( !r.bOK || !g_bCoopNetHandshakeDone || !coopnet_isremoteplayer ( id ) ) break;
			strcpy ( g_CoopNetPlayers[id].remote.pWeapon, pName );
			coopnet_log ( "player %d weapon '%s'", id + 1, pName );
			break;
		}
		case COOPNET_MSG_PLAYERSTATE:
		{
			int id = r.get<unsigned char>();
			if ( g_CoopNetMode == COOPNET_HOST ) id = iFrom;
			sCoopNetPlayerState s;
			s.seq = r.get<unsigned int>();
			s.sendTime = r.get<unsigned int>();
			s.x = r.get<float>();
			s.y = r.get<float>();
			s.z = r.get<float>();
			s.pitch = r.get<float>();
			s.yaw = r.get<float>();
			s.action = r.get<unsigned char>();
			s.fireCount = r.get<unsigned char>();
			s.meleeCount = r.get<unsigned char>();
			s.shotCount = r.get<unsigned char>();
			s.lives = r.get<signed char>();
			s.flags = r.get<unsigned char>();
			s.health = r.get<short>();
			if ( !r.bOK || !g_bCoopNetHandshakeDone || !coopnet_isremoteplayer ( id ) ) break;
			int iStoreCur = g_iCoopNetCur;
			g_iCoopNetCur = id;
			g_CoopNetRemote.state = s;
			g_CoopNetRemote.bValid = true;
			g_CoopNetRemote.dwLastReceived = timeGetTime();
			coopnet_addsnapshot ( s, g_CoopNetRemote.dwLastReceived );
			g_iCoopNetCur = iStoreCur;
			break;
		}
		default:
			coopnet_log ( "unknown message type %d (%d bytes)", msg, (int)uLength );
			break;
	}
	g_iCoopNetFromPlayer = -1;

	// host: pass on to the other clients what they need from this one - its position and weapon,
	// and shared world events (only when it is on our level, as we just applied them)
	if ( g_CoopNetMode != COOPNET_HOST || !g_CoopNetPlayers[iFrom].bHandshakeDone || uLength < 2 ) return;
	bool bRelay = msg == COOPNET_MSG_PLAYERSTATE || msg == COOPNET_MSG_WEAPON;
	if ( msg == COOPNET_MSG_BLOODFX || msg == COOPNET_MSG_ITEMGONE || msg == COOPNET_MSG_DOOR || msg == COOPNET_MSG_CHECKPOINT )
		bRelay = coopnet_levelmatchfor ( iFrom );
	if ( !bRelay ) return;
	unsigned char buf[2048];
	if ( uLength > sizeof(buf) ) return;
	memcpy ( buf, pData, uLength );
	if ( msg == COOPNET_MSG_PLAYERSTATE || msg == COOPNET_MSG_WEAPON ) buf[1] = (unsigned char)iFrom;
	bool bReliable = msg != COOPNET_MSG_PLAYERSTATE;
	coopnet_sendto ( -1, buf, uLength, bReliable ? COOPNET_CHANNEL_EVENTS : COOPNET_CHANNEL_STATE, bReliable ? ENET_PACKET_FLAG_RELIABLE : 0, iFrom );
}

//
// Connection management
//

static void coopnet_updatesession ( void )
{
	// session flags from the connections: a client has only the host, the host any of its clients
	g_bCoopNetConnected = false;
	g_bCoopNetHandshakeDone = false;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		if ( !coopnet_isremoteplayer ( id ) ) continue;
		if ( g_CoopNetPlayers[id].bConnected ) g_bCoopNetConnected = true;
		if ( g_CoopNetPlayers[id].bHandshakeDone ) g_bCoopNetHandshakeDone = true;
	}
}

static void coopnet_resetplayer ( int id )
{
	// forget one player (left, or never joined); their puppet hides on the next update
	if ( id < 0 || id >= COOPNET_MAX_PLAYERS ) return;
	sCoopNetPlayerSlot& p = g_CoopNetPlayers[id];
	p.pPeer = NULL;
	p.bConnected = false;
	p.bHandshakeDone = false;
	p.remote.bValid = false;
	p.remote.pWeapon[0] = 0;
	p.puppet.bCountsSeen = false;
	p.puppet.bShotsSeen = false;
	p.puppet.iPendingShots = 0;
	p.iSnapStart = 0;
	p.iSnapCount = 0;
	p.bTimeOffsetValid = false;
	p.bLoading = false;
	p.bAtExit = false;
	g_CoopNetRemoteLevels[id].bValid = false;
	g_dwCoopNetKickAt[id] = 0;
	if ( g_iCoopNetSpectateID == id ) g_iCoopNetSpectateID = -1;
	coopnet_updatesession();
}

static void coopnet_settimeoutlong ( int id, bool bLong )
{
	// loading a level stops a game loop for many seconds: allow that player a long silence, and
	// go back to the normal limit afterwards
	if ( id < 0 || id >= COOPNET_MAX_PLAYERS ) return;
	if ( g_bCoopNetSteam )
	{
		if ( g_bCoopNetSteamStarted ) coopsteam_settimeout ( id, bLong ? COOPNET_LOADING_TIMEOUT_MS : 0 );
	}
	else if ( g_CoopNetPlayers[id].pPeer )
		enet_peer_timeout ( g_CoopNetPlayers[id].pPeer, 0, bLong ? COOPNET_LOADING_TIMEOUT_MS / 2 : 0, bLong ? COOPNET_LOADING_TIMEOUT_MS : 0 );
}

static void coopnet_resetsession ( void )
{
	// the whole session is gone (a client lost the host, or shutdown)
	coopnet_undoremotefreeze();
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ ) coopnet_resetplayer ( id );
	if ( g_CoopNetMode == COOPNET_CLIENT ) g_iCoopNetLocalID = COOPNET_NO_PLAYER;
	g_uCoopNetLastZombieSeq = 0;
	g_CoopNetZombies.clear();
	g_CoopNetHostHealth.clear();
	g_iCoopNetExitAt = g_iCoopNetExitNeeded = 0;
	g_CoopNetHeldPackets.clear();	// from the session that just ended
	// a client goes back to its own settings file (the next host sends its switches again)
	if ( g_iCoopNetHostSettings >= 0 ) { g_iCoopNetHostSettings = -1; g_bCoopNetSettingsInit = false; }
}

static void coopnet_playergone ( int id )
{
	// a connection closed: the host drops that client and tells the others; a client lost the host
	if ( g_CoopNetMode == COOPNET_CLIENT ) { coopnet_resetsession(); return; }
	bool bWasIn = g_CoopNetPlayers[id].bHandshakeDone;
	coopnet_resetplayer ( id );
	if ( !bWasIn ) return;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_PLAYERLEFT );
	w.put<unsigned char> ( (unsigned char)id );
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
}

static std::string coopnet_playeridentity ( int id )
{
	// what the block list stores for a player: their Steam ID, or (direct IP) their address
	if ( g_bCoopNetSteam )
	{
		unsigned long long u = coopsteam_partnersteamid ( id );
		char p[32]; sprintf ( p, "steam:%llu", u );
		return u ? p : "";
	}
	ENetPeer* pPeer = g_CoopNetPlayers[id].pPeer;
	if ( pPeer == NULL ) return "";
	char pIP[64] = "";
	enet_address_get_host_ip ( &pPeer->address, pIP, sizeof(pIP) );
	return std::string ( "ip:" ) + pIP;
}

static std::string coopnet_playername ( int id )
{
	if ( g_bCoopNetSteam ) { const char* p = coopsteam_partnername ( id ); if ( p && p[0] ) return p; }
	char p[96];
	std::string sIdent = coopnet_playeridentity ( id );
	sprintf ( p, "Player %d%s%s", id + 1, sIdent.empty() ? "" : "  ", sIdent.empty() ? "" : sIdent.c_str() + ( g_bCoopNetSteam ? 0 : 3 ) );
	return p;
}

static std::string coopnet_blocklistpath ( void )
{
	char p[MAX_PATH];
	GetModuleFileNameA ( NULL, p, MAX_PATH );
	char* pSlash = strrchr ( p, '\\' );
	if ( pSlash ) pSlash[1] = 0; else p[0] = 0;
	return std::string ( p ) + "coop-blocklist.txt";
}

static bool coopnet_isblocked ( int id )
{
	// coop-blocklist.txt: one "steam:<id>" or "ip:<address>" per line (anything after it is a note)
	std::string sIdent = coopnet_playeridentity ( id );
	if ( sIdent.empty() ) return false;
	FILE* fp = fopen ( coopnet_blocklistpath().c_str(), "r" );
	if ( fp == NULL ) return false;
	char pLine[256];
	bool bBlocked = false;
	while ( !bBlocked && fgets ( pLine, sizeof(pLine), fp ) )
	{
		char* p = pLine;
		while ( *p == ' ' || *p == '\t' ) p++;
		size_t n = strcspn ( p, " \t;\r\n" );
		if ( n == sIdent.size() && _strnicmp ( p, sIdent.c_str(), n ) == 0 ) bBlocked = true;
	}
	fclose ( fp );
	return bBlocked;
}

static void coopnet_block ( int id )
{
	std::string sIdent = coopnet_playeridentity ( id );
	if ( sIdent.empty() || coopnet_isblocked ( id ) ) return;
	FILE* fp = fopen ( coopnet_blocklistpath().c_str(), "a" );
	if ( fp == NULL ) return;
	fprintf ( fp, "%s ; %s\n", sIdent.c_str(), coopnet_playername ( id ).c_str() );
	fclose ( fp );
}

static void coopnet_kick ( int id, int iReason )
{
	// host: tell the player why, stop listening to them, and hang up a moment later (so the KICK
	// arrives first - see coopnet_update); the others are told they left
	if ( g_CoopNetMode != COOPNET_HOST || id <= 0 || id >= COOPNET_MAX_PLAYERS || !g_CoopNetPlayers[id].bConnected ) return;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_KICK );
	w.put<unsigned char> ( (unsigned char)iReason );
	coopnet_sendto ( id, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE, -1, false );
	bool bWasIn = g_CoopNetPlayers[id].bHandshakeDone;
	g_CoopNetPlayers[id].bHandshakeDone = false;
	g_CoopNetPlayers[id].remote.bValid = false;
	g_dwCoopNetKickAt[id] = timeGetTime() + 1000;
	if ( g_dwCoopNetKickAt[id] == 0 ) g_dwCoopNetKickAt[id] = 1;
	coopnet_updatesession();
	if ( bWasIn )
	{
		sCoopNetWriter l;
		l.put<unsigned char> ( COOPNET_MSG_PLAYERLEFT );
		l.put<unsigned char> ( (unsigned char)id );
		coopnet_send ( l, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	}
}

static void coopnet_leave ( void )
{
	// client: the host sent us away - hang up and stop trying (coopnet_update stops reconnecting
	// while g_iCoopNetKickedReason is set)
	if ( g_bCoopNetSteam ) coopsteam_shutdown();
	else if ( g_CoopNetPlayers[0].pPeer ) enet_peer_disconnect_now ( g_CoopNetPlayers[0].pPeer, COOPNET_DISCONNECT_NORMAL );
	coopnet_resetsession();
}

static void coopnet_cheatcheck ( DWORD dwNow )
{
	// see "Live checks" above; once something is found it stays found (restart the game)
	if ( g_pCoopNetCheat[0] || !g_CoopNetSettings.bCheatCheck ) return;
	for ( int f = 0; f < 2; f++ )
	{
		if ( g_hCoopNetWatch[f] == INVALID_HANDLE_VALUE || WaitForSingleObject ( g_hCoopNetWatch[f], 0 ) != WAIT_OBJECT_0 ) continue;
		FindNextChangeNotification ( g_hCoopNetWatch[f] );
		g_dwCoopNetRehashAt = dwNow + 1000;		// once the writing is done
		if ( g_dwCoopNetRehashAt == 0 ) g_dwCoopNetRehashAt = 1;
	}
	if ( g_dwCoopNetRehashAt != 0 && (int)( dwNow - g_dwCoopNetRehashAt ) >= 0 )
	{
		// something changed in scriptbank / guns: do the fingerprinted files still match?
		g_dwCoopNetRehashAt = 0;
		unsigned long long uStartup = g_uCoopNetFilesHash;
		int iStartupCount = g_iCoopNetFilesCount;
		coopnet_hashfiles();
		if ( g_uCoopNetFilesHash != uStartup ) strcpy ( g_pCoopNetCheat, "game scripts were changed while the game was running" );
		g_uCoopNetFilesHash = uStartup;		// HELLO keeps sending the startup fingerprint
		g_iCoopNetFilesCount = iStartupCount;
	}
	if ( !g_pCoopNetCheat[0] && (int)( dwNow - g_dwCoopNetNextToolCheck ) >= 0 )
	{
		g_dwCoopNetNextToolCheck = dwNow + 3000;
		coopnet_findcheattool ( g_pCoopNetCheat );
	}
	if ( !g_pCoopNetCheat[0] ) return;

	coopnet_event ( "cheat found: %s - co-op stopped", g_pCoopNetCheat );
	if ( g_CoopNetMode == COOPNET_HOST )
	{
		// everyone goes home (and later joiners are refused - see HELLO)
		for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ ) coopnet_kick ( id, COOPNET_KICK_HOSTCHEAT );
		return;
	}
	// client: tell the host why, then hang up and stop trying
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_KICK );
	w.put<unsigned char> ( COOPNET_KICK_CHEAT );
	coopnet_sendto ( 0, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE, -1, false );
	if ( g_pCoopNetHost ) enet_host_flush ( g_pCoopNetHost );
	g_iCoopNetKickedReason = COOPNET_KICK_CHEAT;
	coopnet_leave();
}

static void coopnet_dropconnection ( int id, enet_uint32 uReason )
{
	// hang up on a player (the disconnect event then resets them)
	if ( g_bCoopNetSteam ) { coopsteam_disconnect ( id ); return; }
	if ( id >= 0 && id < COOPNET_MAX_PLAYERS && g_CoopNetPlayers[id].pPeer ) enet_peer_disconnect ( g_CoopNetPlayers[id].pPeer, uReason );
}

static int coopnet_ping ( int id )
{
	// round trip to a player connected straight to us, -1 if not
	if ( g_bCoopNetSteam ) return coopsteam_ping ( id );
	ENetPeer* pPeer = id >= 0 && id < COOPNET_MAX_PLAYERS ? g_CoopNetPlayers[id].pPeer : NULL;
	return pPeer ? (int)pPeer->roundTripTime : -1;
}

static void coopnet_steamlog ( const char* pLine ) { coopnet_log ( "%s", pLine ); }

static void coopnet_clientconnect ( void )
{
	g_dwCoopNetLastConnectAttempt = timeGetTime();
	g_CoopNetPlayers[0].pPeer = enet_host_connect ( g_pCoopNetHost, &g_CoopNetAddress, COOPNET_CHANNEL_COUNT, 0 );
	if ( g_CoopNetPlayers[0].pPeer )
		coopnet_log ( "connecting to %s:%d ...", g_pCoopNetJoinName, g_CoopNetAddress.port );
	else
		coopnet_event ( "enet_host_connect failed" );
}

static bool coopnet_parsecommandline ( void )
{
	// only look at the part before project= (the engine treats the rest as the project name)
	char pCmd[2048];
	strncpy ( pCmd, GetCommandLineA(), sizeof(pCmd) - 1 );
	pCmd[sizeof(pCmd) - 1] = 0;
	_strlwr ( pCmd );
	char* pProject = strstr ( pCmd, "project=" );
	if ( pProject ) *pProject = 0;

	char* pHost = strstr ( pCmd, "-nethost" );
	char* pJoin = strstr ( pCmd, "-netjoin=" );
	char* pSteam = strstr ( pCmd, "-netsteam=" );
	if ( pSteam || strstr ( pCmd, "+connect_lobby" ) )
	{
		// Steam (M-CoopNetSteam.cpp); Steam adds +connect_lobby when it starts the game from an invite
		g_bCoopNetSteam = true;
		g_CoopNetMode = ( pSteam && strncmp ( pSteam + 10, "host", 4 ) == 0 ) ? COOPNET_HOST : COOPNET_CLIENT;
		return true;
	}
	if ( pHost )
	{
		g_CoopNetMode = COOPNET_HOST;
		g_CoopNetAddress.host = ENET_HOST_ANY;
		g_CoopNetAddress.port = COOPNET_DEFAULT_PORT;
		if ( pHost[8] == '=' ) g_CoopNetAddress.port = (enet_uint16)atoi ( pHost + 9 );
		return true;
	}
	if ( pJoin )
	{
		g_CoopNetMode = COOPNET_CLIENT;
		char* pArg = pJoin + 9;
		size_t n = strcspn ( pArg, " \t\"" );
		if ( n >= sizeof(g_pCoopNetJoinName) ) n = sizeof(g_pCoopNetJoinName) - 1;
		memcpy ( g_pCoopNetJoinName, pArg, n );
		g_pCoopNetJoinName[n] = 0;
		g_CoopNetAddress.port = COOPNET_DEFAULT_PORT;
		char* pColon = strchr ( g_pCoopNetJoinName, ':' );
		if ( pColon ) { *pColon = 0; g_CoopNetAddress.port = (enet_uint16)atoi ( pColon + 1 ); }
		if ( enet_address_set_host ( &g_CoopNetAddress, g_pCoopNetJoinName ) != 0 )
		{
			coopnet_event ( "could not resolve host '%s'", g_pCoopNetJoinName );
		}
		return true;
	}
	return false;
}

static void coopnet_init ( void )
{
	g_CoopNetMode = COOPNET_OFF;
	memset ( g_CoopNetPlayers, 0, sizeof(g_CoopNetPlayers) );
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ ) g_CoopNetPlayers[id].iPuppetBase = -1;
	g_iCoopNetLocalID = 0;
	g_iCoopNetSpectateID = -1;
	memset ( &g_CoopNetLocal, 0, sizeof(g_CoopNetLocal) );
	g_iCoopNetWeaponCacheCount = 0;
	memset ( g_CoopNetClaimed, 0, sizeof(g_CoopNetClaimed) );
	g_pCoopNetLocalWeapon[0] = 0;
	memset ( &g_CoopNetLocalLevel, 0, sizeof(g_CoopNetLocalLevel) );
	memset ( g_CoopNetRemoteLevels, 0, sizeof(g_CoopNetRemoteLevels) );
	g_dwCoopNetNextLevelCheck = 0;
	g_uCoopNetLastLocalAction = COOPNET_ACTION_NONE;
	g_iCoopNetLastAmmoGun = 0; g_iCoopNetLastAmmo = -1;
	g_bCoopNetOutOfLives = false;
	g_dwCoopNetNextEntitySearch = 0;
	g_iCoopNetCharactersLoggedFor = -1;
	g_pCoopNetLastEvent[0] = 0;
	g_bCoopNetHostBindFailed = false;
	g_bCoopNetSteam = false;
	g_bCoopNetSteamStarted = false;
	coopnet_resetsession();
	coopnet_settingsdefaults ( &g_CoopNetSettings );	// until the project's file is read (coopnet_updatesettings)

	if ( enet_initialize() != 0 )
	{
		timestampactivity ( 0, "coopnet: enet_initialize failed" );
		return;
	}
	if ( !coopnet_parsecommandline() )
	{
		// no networking requested - stay completely inactive
		g_CoopNetMode = COOPNET_OFF;
		enet_deinitialize();
		return;
	}

	g_dwCoopNetStartTime = timeGetTime();
	g_dwCoopNetNextSend = g_dwCoopNetStartTime;
	g_dwCoopNetStatsWindow = g_dwCoopNetStartTime;
	g_dwCoopNetNextRemoteLog = g_dwCoopNetStartTime;
	coopnet_openlog ( g_CoopNetMode == COOPNET_HOST ? "host" : "client" );
	g_iCoopNetLocalID = g_CoopNetMode == COOPNET_HOST ? 0 : COOPNET_NO_PLAYER;	// a client learns its id from the host
	g_iCoopNetKickedReason = COOPNET_KICK_NONE;
	memset ( g_dwCoopNetKickAt, 0, sizeof(g_dwCoopNetKickAt) );
	DWORD dwHashStart = timeGetTime();
	coopnet_hashfiles();
	coopnet_log ( "game files fingerprint %016llx (%d scripts and weapon files, %d ms)", g_uCoopNetFilesHash, g_iCoopNetFilesCount, (int)( timeGetTime() - dwHashStart ) );
	g_pCoopNetCheat[0] = 0;
	g_dwCoopNetRehashAt = 0;
	g_dwCoopNetNextToolCheck = 0;
	coopnet_startfilewatch();

	if ( g_bCoopNetSteam )
	{
		g_bCoopNetSteamStarted = coopsteam_start ( g_CoopNetMode == COOPNET_HOST, coopnet_steamlog );
		if ( g_bCoopNetSteamStarted )
			coopnet_event ( g_CoopNetMode == COOPNET_HOST ? "hosting over Steam" : "joining over Steam" );
		else
			coopnet_event ( "%s", coopsteam_status() );	// retried every few seconds (coopnet_service)
		g_dwCoopNetLastConnectAttempt = timeGetTime();
		return;
	}

	if ( g_CoopNetMode == COOPNET_HOST )
	{
		// up to 3 clients = 4 players
		g_pCoopNetHost = enet_host_create ( &g_CoopNetAddress, COOPNET_MAX_PLAYERS - 1, COOPNET_CHANNEL_COUNT, 0, 0 );
		if ( g_pCoopNetHost )
		{
			coopnet_event ( "hosting on UDP port %d, waiting for players", g_CoopNetAddress.port );
		}
		else
		{
			g_bCoopNetHostBindFailed = true;
			coopnet_event ( "FAILED to host on UDP port %d (already in use?)", g_CoopNetAddress.port );
		}
	}
	else
	{
		g_pCoopNetHost = enet_host_create ( NULL, 1, COOPNET_CHANNEL_COUNT, 0, 0 );
		if ( g_pCoopNetHost )
			coopnet_clientconnect();
		else
			coopnet_event ( "FAILED to create client socket" );
	}
}

//
// Per-frame
//

static void coopnet_readlocalplayer ( void )
{
	// player = physics capsule object, view = camera 0 (same sources lua_loop_begin uses)
	int iObj = t.aisystem.objectstartindex;
	g_bCoopNetLocalValid = ( iObj > 0 && ObjectExist ( iObj ) == 1 );
	if ( !g_bCoopNetLocalValid ) return;
	g_CoopNetLocal.x = ObjectPositionX ( iObj );
	g_CoopNetLocal.y = ObjectPositionY ( iObj );
	g_CoopNetLocal.z = ObjectPositionZ ( iObj );
	// while dead, keep the angles we died with: the camera follows the partner when spectating
	if ( t.playercontrol.deadtime == 0 && t.player[t.plrid].health > 0 )
	{
		g_CoopNetLocal.pitch = CameraAngleX ( 0 );
		g_CoopNetLocal.yaw = CameraAngleY ( 0 );
	}

	// current weapon: t.gunid (0 = unarmed); melee weapons are weapontype 51+ or ismelee
	char pWeapon[COOPNET_WEAPON_NAME_MAX] = "";
	bool bMeleeWeapon = false;
	if ( t.gunid > 0 )
	{
		strncpy ( pWeapon, t.gun[t.gunid].name_s.Get(), COOPNET_WEAPON_NAME_MAX - 1 );
		pWeapon[COOPNET_WEAPON_NAME_MAX - 1] = 0;
		bMeleeWeapon = t.gun[t.gunid].weapontype >= 51 || t.gun[t.gunid].settings.ismelee != 0;
	}
	if ( strcmp ( pWeapon, g_pCoopNetLocalWeapon ) != 0 )
	{
		strcpy ( g_pCoopNetLocalWeapon, pWeapon );
		if ( g_bCoopNetConnected && g_bCoopNetHandshakeDone ) coopnet_sendweapon();
	}

	// what the weapon is doing (gunmode ranges from G-Gun.cpp / lua_loop_begin)
	unsigned char action = COOPNET_ACTION_NONE;
	if ( t.gunid > 0 )
	{
		if ( t.gunmode >= 101 && t.gunmode <= 120 ) action = bMeleeWeapon ? COOPNET_ACTION_MELEE : COOPNET_ACTION_FIRE;
		else if ( t.gunmode >= 1020 && t.gunmode <= 1023 ) action = COOPNET_ACTION_MELEE;
		else if ( ( t.gunmode >= 121 && t.gunmode <= 126 ) || ( t.gunmode >= 700 && t.gunmode <= 707 ) ) action = COOPNET_ACTION_RELOAD;
	}
	if ( action != g_uCoopNetLastLocalAction )
	{
		if ( action == COOPNET_ACTION_FIRE ) g_CoopNetLocal.fireCount++;
		if ( action == COOPNET_ACTION_MELEE ) g_CoopNetLocal.meleeCount++;
		g_uCoopNetLastLocalAction = action;
	}
	g_CoopNetLocal.action = action;

	// rounds fired = the gun's total ammo going down (loaded + spare, as the HUD's 'Gun Ammo
	// Remaining'; a reload moves rounds between the two without changing it, pickups raise it)
	int iAmmo = -1;
	if ( t.gunid > 0 && !bMeleeWeapon ) iAmmo = t.slidersmenuvalue[t.slidersmenunames.weapon][1].value + t.slidersmenuvalue[t.slidersmenunames.weapon][2].value;
	if ( iAmmo >= 0 && t.gunid == g_iCoopNetLastAmmoGun && g_iCoopNetLastAmmo >= 0 && iAmmo < g_iCoopNetLastAmmo )
	{
		int iShots = g_iCoopNetLastAmmo - iAmmo;
		if ( iShots > 10 ) iShots = 10;	// a bigger drop is not a burst of fire (script took ammo etc.)
		g_CoopNetLocal.shotCount += (unsigned char)iShots;
	}
	g_iCoopNetLastAmmoGun = t.gunid;
	g_iCoopNetLastAmmo = iAmmo;
	int iHealth = t.player[t.plrid].health;
	g_CoopNetLocal.health = (short)( iHealth > 32767 ? 32767 : ( iHealth < -32768 ? -32768 : iHealth ) ); // god mode uses 99999
	int iLives = t.player[t.plrid].lives;
	g_CoopNetLocal.lives = (signed char)( iLives > 127 ? 127 : ( iLives < 0 ? 0 : iLives ) );
	g_CoopNetLocal.flags = g_bCoopNetOutOfLives ? COOPNET_PLAYERFLAG_OUTOFLIVES : 0;
}

static void coopnet_servicesteam ( void )
{
	if ( !g_bCoopNetSteamStarted )
	{
		// Steam was not running at launch: keep trying (the screen says to start it)
		if ( timeGetTime() - g_dwCoopNetLastConnectAttempt < 5000 ) return;
		g_dwCoopNetLastConnectAttempt = timeGetTime();
		g_bCoopNetSteamStarted = coopsteam_start ( g_CoopNetMode == COOPNET_HOST, coopnet_steamlog );
		if ( !g_bCoopNetSteamStarted ) return;
		coopnet_event ( g_CoopNetMode == COOPNET_HOST ? "hosting over Steam" : "joining over Steam" );
	}
	coopsteam_update();
	// Steam numbers its connections by player id too (host: clients 1..3, client: the host 0)
	int id = 0;
	for ( int iEvent = coopsteam_pollevent ( &id ); iEvent != COOPSTEAM_EVENT_NONE; iEvent = coopsteam_pollevent ( &id ) )
	{
		if ( id < 0 || id >= COOPNET_MAX_PLAYERS ) continue;
		if ( iEvent == COOPSTEAM_EVENT_CONNECTED )
		{
			g_CoopNetPlayers[id].bConnected = true;
			coopnet_updatesession();
			if ( g_CoopNetMode == COOPNET_HOST && coopnet_isblocked ( id ) )
			{
				coopnet_event ( "%s is on the block list - refused", coopsteam_partnername ( id ) );
				coopnet_kick ( id, COOPNET_KICK_BLOCKED );
				continue;
			}
			if ( g_CoopNetMode == COOPNET_HOST )
				coopnet_event ( "%s joined over Steam - player %d", coopsteam_partnername ( id ), id + 1 );
			else
				coopnet_event ( "connected to %s over Steam", coopsteam_partnername ( id ) );
			coopnet_sendhello ( id );
		}
		else if ( iEvent == COOPSTEAM_EVENT_DISCONNECTED )
		{
			if ( g_CoopNetPlayers[id].bHandshakeDone )
				coopnet_event ( g_CoopNetMode == COOPNET_HOST ? "player %d left or timed out" : "lost the host (player %d left or timed out)", id + 1 );
			coopnet_playergone ( id );
		}
		else if ( iEvent == COOPSTEAM_EVENT_REFUSED )
			coopnet_event ( "that friend's game is full" );
	}
	static unsigned char buf[16384];
	int iLen = 0;
	while ( coopsteam_receive ( buf, sizeof(buf), &iLen, &id ) )
		if ( id >= 0 && id < COOPNET_MAX_PLAYERS && g_CoopNetPlayers[id].bConnected && iLen > 0 ) coopnet_receive ( buf, (size_t)iLen, id );
}

static int coopnet_enetplayer ( ENetPeer* pPeer )
{
	// which player a direct connection belongs to (-1 = none)
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		if ( pPeer != NULL && g_CoopNetPlayers[id].pPeer == pPeer ) return id;
	return -1;
}

static void coopnet_service ( void )
{
	if ( g_bCoopNetSteam ) { coopnet_servicesteam(); return; }
	ENetEvent ev;
	while ( g_pCoopNetHost && enet_host_service ( g_pCoopNetHost, &ev, 0 ) > 0 )
	{
		switch ( ev.type )
		{
			case ENET_EVENT_TYPE_CONNECT:
			{
				// host: give the new client the first free player id; client: the host is player 0
				int id = g_CoopNetMode == COOPNET_CLIENT ? 0 : -1;
				for ( int i = 1; id < 0 && i < COOPNET_MAX_PLAYERS; i++ )
					if ( !g_CoopNetPlayers[i].bConnected && g_CoopNetPlayers[i].pPeer == NULL ) id = i;
				if ( id < 0 ) { enet_peer_disconnect_now ( ev.peer, COOPNET_DISCONNECT_NORMAL ); break; }	// full (ENet allows 3 anyway)
				g_CoopNetPlayers[id].pPeer = ev.peer;
				g_CoopNetPlayers[id].bConnected = true;
				coopnet_updatesession();
				char pIP[64] = "?";
				enet_address_get_host_ip ( &ev.peer->address, pIP, sizeof(pIP) );
				if ( g_CoopNetMode == COOPNET_HOST && coopnet_isblocked ( id ) )
				{
					coopnet_event ( "connection from %s is on the block list - refused", pIP );
					coopnet_kick ( id, COOPNET_KICK_BLOCKED );
					break;
				}
				if ( g_CoopNetMode == COOPNET_HOST )
					coopnet_event ( "connection from %s:%d - player %d", pIP, ev.peer->address.port, id + 1 );
				else
					coopnet_event ( "connected to %s:%d", pIP, ev.peer->address.port );
				coopnet_sendhello ( id );
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
			{
				int id = coopnet_enetplayer ( ev.peer );
				if ( id >= 0 && g_CoopNetPlayers[id].bConnected ) coopnet_receive ( ev.packet->data, ev.packet->dataLength, id );
				enet_packet_destroy ( ev.packet );
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				int id = coopnet_enetplayer ( ev.peer );
				if ( id < 0 ) break;
				if ( ev.data == COOPNET_DISCONNECT_VERSION )
					coopnet_event ( "disconnected: protocol version mismatch" );
				else if ( g_CoopNetPlayers[id].bHandshakeDone )
					coopnet_event ( g_CoopNetMode == COOPNET_HOST ? "player %d left or timed out" : "lost the host (player %d left or timed out)", id + 1 );
				else if ( g_CoopNetMode == COOPNET_CLIENT )
					coopnet_log ( "no answer from %s:%d, retrying", g_pCoopNetJoinName, g_CoopNetAddress.port );
				coopnet_playergone ( id );
				break;
			}
			default:
				break;
		}
	}
}

//
// Remote player puppet
//

static void coopnet_puppet_hide ( void )
{
	if ( g_CoopNetPuppet.iFireSound > 0 && SoundExist ( g_CoopNetPuppet.iFireSound ) == 1 ) StopSound ( g_CoopNetPuppet.iFireSound );
	g_CoopNetPuppet.iFireSound = 0;
	// check real visibility: a Player1/Player2 entity starts visible where it was placed
	if ( g_CoopNetPuppet.iObj > 0 && ObjectExist ( g_CoopNetPuppet.iObj ) == 1 && GetVisible ( g_CoopNetPuppet.iObj ) == 1 )
		HideObject ( g_CoopNetPuppet.iObj );
	g_CoopNetPuppet.bVisible = false;
}

static bool coopnet_puppet_ensureplaceholder ( void )
{
	// level objects can be freed under us (level change), so check every frame
	if ( g_CoopNetPuppet.iObj > 0 && ObjectExist ( g_CoopNetPuppet.iObj ) == 1 ) return true;
	g_CoopNetPuppet.iObj = 0;
	g_CoopNetPuppet.bVisible = false;
	if ( g_CoopNetPuppet.bPlaceholderBlocked ) return false;
	if ( ObjectExist ( COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) ) == 1 )
	{
		g_CoopNetPuppet.bPlaceholderBlocked = true;
		coopnet_event ( "puppet: placeholder object %d already in use, no puppet shown", COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) );
		return false;
	}
	// roughly player-capsule sized, centred on its origin like the capsule
	MakeObjectBox ( COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ), 24.0f, 72.0f, 24.0f );
	SetObjectCollisionOff ( COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) );
	HideObject ( COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) );
	g_CoopNetPuppet.iObj = COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur );
	coopnet_log ( "puppet: created placeholder box (object %d)", COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) );
	return true;
}

//
// Player1 / Player2 entities
//

static bool coopnet_namematches ( const char* pEntityName, const char* pWanted )
{
	// ignore case, spaces and underscores: "Player2 Melee" / "player2_melee" == "Player2Melee"
	const char* a = pEntityName;
	const char* b = pWanted;
	for ( ;; )
	{
		while ( *a == ' ' || *a == '_' ) a++;
		while ( *b == ' ' || *b == '_' ) b++;
		if ( tolower ( (unsigned char)*a ) != tolower ( (unsigned char)*b ) ) return false;
		if ( *a == 0 ) return true;
		a++; b++;
	}
}

static int coopnet_findentitybyname ( const char* pName )
{
	for ( int e = 1; e <= g.entityelementlist; e++ )
	{
		if ( t.entityelement[e].bankindex <= 0 || t.entityelement[e].obj <= 0 ) continue;
		if ( coopnet_namematches ( t.entityelement[e].eleprof.name_s.Get(), pName ) ) return e;
	}
	return 0;
}

int coopnet_findpuppetentity ( int iSlot )
{
	if ( iSlot < 0 || iSlot >= COOPNET_PUPPET_NAME_COUNT ) return 0;
	return coopnet_findentitybyname ( g_pCoopNetPuppetNames[iSlot] );
}

const char* coopnet_puppetname ( int iSlot )
{
	return iSlot >= 0 && iSlot < COOPNET_PUPPET_NAME_COUNT ? g_pCoopNetPuppetNames[iSlot] : "";
}

static void coopnet_logcharacters ( void )
{
	// diagnostics: every character in the level, its placed Name and its character type
	int iCount = 0;
	for ( int e = 1; e <= g.entityelementlist; e++ )
	{
		int entid = t.entityelement[e].bankindex;
		if ( entid <= 0 || t.entityprofile[entid].ischaracter != 1 ) continue;
		coopnet_log ( "level character e=%d name='%s' type='%s'", e, t.entityelement[e].eleprof.name_s.Get(),
			entid < (int)t.entitybank_s.size() ? t.entitybank_s[entid].Get() : "?" );
		iCount++;
	}
	coopnet_log ( "level characters: %d", iCount );
}

static bool coopnet_claimedstillvalid ( int iSlot )
{
	int e = g_CoopNetClaimed[iSlot].e;
	if ( e <= 0 || e > g.entityelementlist ) return false;
	if ( t.entityelement[e].bankindex <= 0 || t.entityelement[e].obj <= 0 ) return false;
	if ( ObjectExist ( t.entityelement[e].obj ) == 0 ) return false;
	return coopnet_namematches ( t.entityelement[e].eleprof.name_s.Get(), g_pCoopNetPuppetNames[iSlot] );
}

static void coopnet_takeoverentity ( sCoopNetClaimedEntity* pClaim )
{
	int e = pClaim->e;
	if ( !pClaim->bSetupDone )
	{
		// physics and collision off, same as the Lua CollisionOff command
		int storee = t.e, storeentid = t.entid, storetobj = t.tobj, storetphyobj = t.tphyobj;
		t.e = e;
		entity_lua_collisionoff();
		t.e = storee; t.entid = storeentid; t.tobj = storetobj; t.tphyobj = storetphyobj;
		pClaim->bSetupDone = true;
	}

	// every frame, as other systems can undo these:
	// skip its Lua script (the flag darkai uses for out-of-range characters)
	t.entityelement[e].lua.outofrangefreeze = 1;

	// keep it out of the character AI system, which otherwise sends unseen characters
	// dormant, resets their animation and sets animation speed every frame
	// (darkai_refresh_characters re-adds characters whenever something spawns)
	for ( int n = 1; n <= g.charanimindexmax; n++ )
	{
		if ( t.charanimstates[n].e == e )
		{
			t.charanimstates[n].e = 0;
			t.charanimstates[n].obj = 0;
		}
	}
}

static int coopnet_findgun ( const char* pName );

static bool coopnet_remotewantsmeleeset ( void )
{
	// unarmed, a melee weapon, or a gun not in this level (shown unarmed)
	int iGun = coopnet_findgun ( g_CoopNetRemote.pWeapon );
	return iGun == 0 || t.gun[iGun].weapontype >= 51 || t.gun[iGun].settings.ismelee != 0;
}

static bool coopnet_playerpresent ( int id )
{
	// a remote player we have heard from recently
	if ( !coopnet_isremoteplayer ( id ) || !g_bCoopNetHandshakeDone ) return false;
	const sCoopNetRemotePlayer& r = g_CoopNetPlayers[id].remote;
	return r.bValid && (int)( timeGetTime() - r.dwLastReceived ) < COOPNET_PUPPET_HIDE_MS;
}

static void coopnet_assignpuppets ( void )
{
	// player id n is shown as Player<n+1> when the level has it; players without theirs borrow
	// a free one (we never show ourselves, so N players need any N-1 of them)
	bool bTaken[COOPNET_MAX_PLAYERS] = { false };
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		g_CoopNetPlayers[id].iPuppetBase = -1;
		if ( coopnet_playerpresent ( id ) && g_CoopNetClaimed[id].e > 0 ) { g_CoopNetPlayers[id].iPuppetBase = id; bTaken[id] = true; }
	}
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		if ( !coopnet_playerpresent ( id ) || g_CoopNetPlayers[id].iPuppetBase >= 0 ) continue;
		for ( int s = 0; s < COOPNET_MAX_PLAYERS; s++ )
			if ( !bTaken[s] && g_CoopNetClaimed[s].e > 0 ) { g_CoopNetPlayers[id].iPuppetBase = s; bTaken[s] = true; break; }
	}
}

static int coopnet_puppetentityslot ( void )
{
	// which named entity represents the current remote player on this machine (-1 = none:
	// the placeholder box); its ...Melee twin while it holds a melee weapon or nothing
	int iBase = g_CoopNetPlayers[g_iCoopNetCur].iPuppetBase;
	if ( iBase < 0 ) return -1;
	int iTwin = iBase + COOPNET_PUPPET_MELEE_TWIN;
	if ( g_CoopNetClaimed[iTwin].e > 0 && coopnet_remotewantsmeleeset() ) return iTwin;
	return iBase;
}

static void coopnet_claimentities ( DWORD dwNow )
{
	// level changes free and reuse entity slots, so re-validate every frame
	bool bAnyMissing = false;
	for ( int i = 0; i < COOPNET_PUPPET_NAME_COUNT; i++ )
	{
		if ( g_CoopNetClaimed[i].e > 0 && !coopnet_claimedstillvalid ( i ) )
		{
			// gone (level change): its held weapon objects went with the level
			for ( int c = g_iCoopNetWeaponCacheCount - 1; c >= 0; c-- )
				if ( g_CoopNetWeaponCache[c].iEntity == g_CoopNetClaimed[i].e )
					g_CoopNetWeaponCache[c] = g_CoopNetWeaponCache[--g_iCoopNetWeaponCacheCount];
			g_CoopNetClaimed[i].e = 0;
			g_CoopNetClaimed[i].bSetupDone = false;
			g_CoopNetClaimed[i].bAnimsLogged = false;
			g_CoopNetClaimed[i].pAnimSet[0] = 0;
		}
		if ( g_CoopNetClaimed[i].e == 0 ) bAnyMissing = true;
	}
	if ( bAnyMissing && (int)( dwNow - g_dwCoopNetNextEntitySearch ) >= 0 )
	{
		g_dwCoopNetNextEntitySearch = dwNow + COOPNET_ENTITY_SEARCH_MS;
		// once per level, list the level's characters so naming problems are visible in the log
		if ( g_pCoopNetLogFile && g_iCoopNetCharactersLoggedFor != g.entityelementlist )
		{
			g_iCoopNetCharactersLoggedFor = g.entityelementlist;
			coopnet_logcharacters();
		}
		for ( int i = 0; i < COOPNET_PUPPET_NAME_COUNT; i++ )
		{
			if ( g_CoopNetClaimed[i].e != 0 ) continue;
			g_CoopNetClaimed[i].e = coopnet_findentitybyname ( g_pCoopNetPuppetNames[i] );
			if ( g_CoopNetClaimed[i].e > 0 )
				coopnet_log ( "found %s entity (e=%d, obj=%d)", g_pCoopNetPuppetNames[i], g_CoopNetClaimed[i].e, t.entityelement[g_CoopNetClaimed[i].e].obj );
		}
	}

	// take over every named entity; hide all except the active puppets (which puppet_update shows/hides)
	coopnet_assignpuppets();
	bool bActive[COOPNET_PUPPET_NAME_COUNT] = { false };
	int iStoreCur = g_iCoopNetCur;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		if ( !coopnet_playerpresent ( id ) ) continue;
		g_iCoopNetCur = id;
		int iSlot = coopnet_puppetentityslot();
		if ( iSlot >= 0 ) bActive[iSlot] = true;
	}
	g_iCoopNetCur = iStoreCur;
	for ( int i = 0; i < COOPNET_PUPPET_NAME_COUNT; i++ )
	{
		if ( g_CoopNetClaimed[i].e == 0 ) continue;
		coopnet_takeoverentity ( &g_CoopNetClaimed[i] );
		if ( bActive[i] ) continue;
		int iObj = t.entityelement[g_CoopNetClaimed[i].e].obj;
		if ( GetVisible ( iObj ) == 1 ) HideObject ( iObj );
	}
}

//
// Puppet animation
//

static bool coopnet_hasanimation ( int iObj, const char* pName )
{
	// exact name only: the engine's lookup also matches loosely (Walk_Aim_Loop picked a
	// different clip on a character that has no aim animations)
	sObject* pObject = GetObjectData ( iObj );
	if ( pObject == NULL ) return false;
	for ( sAnimationSet* pSet = pObject->pAnimationSet; pSet; pSet = pSet->pNext )
		if ( _stricmp ( pSet->szName, pName ) == 0 ) return true;
	return false;
}

static bool coopnet_findanimation ( int e, const char* const* pNames, int iCount, float* pStart, float* pFinish )
{
	for ( int i = 0; i < iCount; i++ )
	{
		if ( !coopnet_hasanimation ( t.entityelement[e].obj, pNames[i] ) ) continue;
		float fStart = -1.0f, fFinish = -1.0f;
		if ( entity_lua_getanimationname ( e, cstr ( (char*)pNames[i] ), &fStart, &fFinish ) > 0 && fFinish > fStart )
		{
			*pStart = fStart; *pFinish = fFinish;
			return true;
		}
	}
	return false;
}

static void coopnet_puppet_lockrootmotion ( int iObj )
{
	// MAX walk/run anims move the root (Bip01) forward; darkai cancels that every frame
	// for moving characters so the entity position alone drives travel. Do the same,
	// or the model walks ahead of the network position and snaps back.
	sObject* pObject = GetObjectData ( iObj );
	if ( pObject == NULL || pObject->ppFrameList == NULL || pObject->dwSpineCenterLimbIndex <= 0 ) return;
	sFrame* pFrame = pObject->ppFrameList[pObject->dwSpineCenterLimbIndex];
	if ( pFrame ) WickedCall_SetBip01Position ( pObject, pFrame, 3, 0, 0 );
}

static void coopnet_puppet_loganimationnames ( int iObj )
{
	// one-off list of the character's animation names (they live inside the model file)
	sObject* pObject = GetObjectData ( iObj );
	if ( pObject == NULL ) return;
	char pLine[480] = "";
	int iCount = 0;
	for ( sAnimationSet* pSet = pObject->pAnimationSet; pSet; pSet = pSet->pNext )
	{
		if ( strlen ( pLine ) + strlen ( pSet->szName ) + 3 >= sizeof(pLine) )
		{
			coopnet_log ( "puppet anims: %s", pLine );
			pLine[0] = 0;
		}
		if ( pLine[0] ) strcat ( pLine, ", " );
		strcat ( pLine, pSet->szName );
		iCount++;
	}
	if ( pLine[0] ) coopnet_log ( "puppet anims: %s", pLine );
	coopnet_log ( "puppet anims: %d total", iCount );
}

static void coopnet_puppet_animate ( int e, int iObj, DWORD dwNow )
{
	coopnet_puppet_lockrootmotion ( iObj );

	// horizontal speed from the received snapshots themselves (newest vs ~200 ms earlier,
	// timed by the sender clock), not from frame-to-frame display deltas
	g_CoopNetPuppet.fSpeed = 0.0f;
	if ( g_iCoopNetSnapshotCount >= 2 )
	{
		int iBack = g_iCoopNetSnapshotCount - 1 < COOPNET_SPEED_WINDOW ? g_iCoopNetSnapshotCount - 1 : COOPNET_SPEED_WINDOW;
		const sCoopNetPlayerState& a = g_CoopNetSnapshots[( g_iCoopNetSnapshotStart + g_iCoopNetSnapshotCount - 1 - iBack ) % COOPNET_SNAPSHOT_COUNT].state;
		const sCoopNetPlayerState& b = g_CoopNetSnapshots[( g_iCoopNetSnapshotStart + g_iCoopNetSnapshotCount - 1 ) % COOPNET_SNAPSHOT_COUNT].state;
		float fSeconds = (int)( b.sendTime - a.sendTime ) / 1000.0f;
		if ( fSeconds > 0.0f )
			g_CoopNetPuppet.fSpeed = sqrtf ( ( b.x - a.x ) * ( b.x - a.x ) + ( b.z - a.z ) * ( b.z - a.z ) ) / fSeconds;
	}

	// locomotion with hysteresis and a minimum hold, as every change restarts the animation
	int iLoco = g_CoopNetPuppet.iLoco;
	float fSpeed = g_CoopNetPuppet.fSpeed;
	int iNewLoco = iLoco;
	if ( iLoco == COOPNET_ANIM_RUN )
	{
		if ( fSpeed < COOPNET_ANIM_RUN_STOP ) iNewLoco = fSpeed >= COOPNET_ANIM_WALK_STOP ? COOPNET_ANIM_WALK : COOPNET_ANIM_IDLE;
	}
	else if ( iLoco == COOPNET_ANIM_WALK )
	{
		if ( fSpeed >= COOPNET_ANIM_RUN_SPEED ) iNewLoco = COOPNET_ANIM_RUN;
		else if ( fSpeed < COOPNET_ANIM_WALK_STOP ) iNewLoco = COOPNET_ANIM_IDLE;
	}
	else
	{
		if ( fSpeed >= COOPNET_ANIM_RUN_SPEED ) iNewLoco = COOPNET_ANIM_RUN;
		else if ( fSpeed >= COOPNET_ANIM_WALK_SPEED ) iNewLoco = COOPNET_ANIM_WALK;
	}
	if ( iNewLoco != iLoco && (int)( dwNow - g_CoopNetPuppet.dwAnimChanged ) >= COOPNET_ANIM_MIN_HOLD_MS )
	{
		g_CoopNetPuppet.iLoco = iNewLoco;
		g_CoopNetPuppet.dwAnimChanged = dwNow;
	}

	// partner dead: one death animation, held on its last frame until they respawn
	if ( g_CoopNetPuppet.shown.health <= 0 )
	{
		if ( g_CoopNetPuppet.iAnim != COOPNET_ANIM_DEATH )
		{
			static const char* pDeath[] = { "Death_Front", "Death_Back", "Death_Front_Knees", "Death_Left", "Death_Right" };
			float fStart = 0, fFinish = 0;
			bool bFound = coopnet_findanimation ( e, pDeath, 5, &fStart, &fFinish );
			g_CoopNetPuppet.iAnim = COOPNET_ANIM_DEATH;
			g_CoopNetPuppet.bAnimFound = bFound;
			g_CoopNetPuppet.fAnimStart = fStart;
			g_CoopNetPuppet.fAnimFinish = fFinish;
			g_CoopNetPuppet.bDeathAnimMissing = !bFound;
			if ( bFound )
			{
				PlayObject ( iObj, (int)fStart, (int)fFinish );
				SetObjectSpeed ( iObj, (int)( 0.65f * t.entityelement[e].eleprof.animspeed * 2.5f ) );
				// the weapon drop on death swaps to the melee twin etc.: don't replay the fall, lie there
				if ( g_CoopNetPuppet.bDeathShown ) coopnet_setframekeepplaying ( iObj, fFinish );
			}
			if ( !g_CoopNetPuppet.bDeathShown ) coopnet_log ( "partner died - puppet death animation %s", bFound ? "playing" : "missing, hiding puppet" );
			g_CoopNetPuppet.bDeathShown = true;
		}
		else if ( g_CoopNetPuppet.bAnimFound && GetFrame ( iObj ) >= g_CoopNetPuppet.fAnimFinish - 1.0f )
		{
			// PlayObject(start, end) does not stop at end in this engine - it runs on into the next
			// animations (here the other Death_* ones) - so hold the last frame ourselves
			SetObjectSpeed ( iObj, 0 );
			coopnet_setframekeepplaying ( iObj, g_CoopNetPuppet.fAnimFinish - 1.0f );
		}
		return;
	}
	g_CoopNetPuppet.bDeathAnimMissing = false;
	g_CoopNetPuppet.bDeathShown = false;

	// weapon events: counters go up each time the remote starts a shot / swing
	const sCoopNetPlayerState& s = g_CoopNetPuppet.shown;
	if ( !g_CoopNetPuppet.bCountsSeen )
	{
		g_CoopNetPuppet.uLastFireCount = s.fireCount;
		g_CoopNetPuppet.uLastMeleeCount = s.meleeCount;
		g_CoopNetPuppet.bCountsSeen = true;
	}
	bool bNewSwing = ( s.meleeCount != g_CoopNetPuppet.uLastMeleeCount );
	g_CoopNetPuppet.uLastMeleeCount = s.meleeCount;
	if ( s.fireCount != g_CoopNetPuppet.uLastFireCount || s.action == COOPNET_ACTION_FIRE )
		g_CoopNetPuppet.dwFireUntil = dwNow + COOPNET_FIRE_HOLD_MS;
	g_CoopNetPuppet.uLastFireCount = s.fireCount;

	// a swing in progress plays to its last frame (unless another swing starts);
	// GetPlaying() is not usable here - it reads 0 even while an animation runs
	if ( !bNewSwing && g_CoopNetPuppet.iAnim == COOPNET_ANIM_MELEE && g_CoopNetPuppet.bAnimFound
		&& (int)( g_CoopNetPuppet.dwMeleeUntil - dwNow ) > 0 )
	{
		float fFrame = GetFrame ( iObj );
		if ( fFrame >= g_CoopNetPuppet.fAnimStart - 1.0f && fFrame < g_CoopNetPuppet.fAnimFinish - 1.0f ) return;
	}

	// movement, then the armed variant when holding a gun (melee weapons use the plain set)
	int iWant = g_CoopNetPuppet.iLoco;
	int iGun = g_CoopNetPuppet.iGunShown;
	bool bHoldingGun = iGun > 0 && t.gun[iGun].weapontype < 51 && t.gun[iGun].settings.ismelee == 0;
	if ( bHoldingGun )
	{
		bool bShooting = (int)( g_CoopNetPuppet.dwFireUntil - dwNow ) > 0;
		if ( bShooting && iWant == COOPNET_ANIM_IDLE ) iWant = COOPNET_ANIM_SHOOT;
		else if ( s.action == COOPNET_ACTION_RELOAD && iWant == COOPNET_ANIM_IDLE ) iWant = COOPNET_ANIM_RELOAD;
		else iWant += COOPNET_ANIM_IDLE_AIM;
	}
	if ( bNewSwing ) iWant = COOPNET_ANIM_MELEE;
	if ( iWant == g_CoopNetPuppet.iAnim && !bNewSwing ) return;

	// MAX character animation names (exact), each with fallbacks to something that exists.
	// No single standard set has both: "melee" has Attack_01/02 but no gun poses, "rifle"
	// has the aim set + Reload but only Elbow_Attack_Aim for melee.
	static const char* pIdle[] = { "Idle" };
	static const char* pWalk[] = { "Walk_Loop", "Idle" };
	static const char* pRun[] = { "Run_Loop", "Walk_Loop", "Idle" };
	static const char* pIdleAim[] = { "Idle_Aim", "Idle" };
	static const char* pWalkAim[] = { "Walk_Aim_Loop", "Walk_Loop", "Idle_Aim", "Idle" };
	static const char* pRunAim[] = { "Run_Aim_Loop", "Run_Loop", "Walk_Aim_Loop", "Idle" };
	static const char* pShoot[] = { "Aim_Shoot", "Idle_Aim", "Idle" };
	static const char* pReload[] = { "Reload_Aim", "Reload", "Idle_Aim", "Idle" };
	// melee has no idle fallback: a swing the character can't play is simply skipped
	static const char* pMelee1[] = { "Attack_01", "Attack_02", "Elbow_Attack_Aim", "Attack_Kick_Slow" };
	static const char* pMelee2[] = { "Attack_02", "Attack_01", "Elbow_Attack_Aim", "Attack_Kick_Slow" };
	const char* const* pNames = pIdle; int iCount = 1;
	switch ( iWant )
	{
		case COOPNET_ANIM_WALK:		pNames = pWalk; iCount = 2; break;
		case COOPNET_ANIM_RUN:		pNames = pRun; iCount = 3; break;
		case COOPNET_ANIM_IDLE_AIM:	pNames = pIdleAim; iCount = 2; break;
		case COOPNET_ANIM_WALK_AIM:	pNames = pWalkAim; iCount = 4; break;
		case COOPNET_ANIM_RUN_AIM:	pNames = pRunAim; iCount = 4; break;
		case COOPNET_ANIM_SHOOT:	pNames = pShoot; iCount = 3; break;
		case COOPNET_ANIM_RELOAD:	pNames = pReload; iCount = 4; break;
		case COOPNET_ANIM_MELEE:
			pNames = ( g_CoopNetPuppet.iMeleeToggle++ & 1 ) ? pMelee2 : pMelee1; iCount = 4; break;
		default: break;
	}
	float fStart = 0, fFinish = 0;
	bool bFound = coopnet_findanimation ( e, pNames, iCount, &fStart, &fFinish );
	if ( !bFound && iWant == COOPNET_ANIM_MELEE )
	{
		coopnet_log ( "puppet anim -> melee skipped (character has no melee animation)" );
		return;
	}
	g_CoopNetPuppet.iAnim = iWant;
	g_CoopNetPuppet.bAnimFound = bFound;
	g_CoopNetPuppet.fAnimStart = fStart;
	g_CoopNetPuppet.fAnimFinish = fFinish;
	coopnet_log ( "puppet anim -> %s (%s frames %.0f-%.0f)", g_pCoopNetAnimStateNames[iWant + 1], bFound ? "found" : "NOT FOUND", fStart, fFinish );
	if ( !bFound )
	{
		// no usable animation names: leave the model still rather than play everything
		StopObject ( iObj );
		return;
	}
	if ( iWant == COOPNET_ANIM_MELEE )
	{
		PlayObject ( iObj, (int)fStart, (int)fFinish );
		g_CoopNetPuppet.dwMeleeUntil = dwNow + COOPNET_MELEE_MAX_MS;
	}
	else
	{
		LoopObject ( iObj, (int)fStart, (int)fFinish );
	}
	// same speed darkai would apply: speedmodulator(1) * 0.65 * animspeed * 2.5
	SetObjectSpeed ( iObj, (int)( 0.65f * t.entityelement[e].eleprof.animspeed * 2.5f ) );
}

//
// Puppet held weapon
//

static int coopnet_findgun ( const char* pName )
{
	if ( pName[0] == 0 ) return 0;
	for ( int g_i = 1; g_i <= g.gunmax; g_i++ )
	{
		if ( t.gun[g_i].activeingame == 1 && _stricmp ( t.gun[g_i].name_s.Get(), pName ) == 0 ) return g_i;
	}
	return 0;
}

static int coopnet_weaponobject ( int e, int iGun )
{
	// cached?
	for ( int i = 0; i < g_iCoopNetWeaponCacheCount; i++ )
	{
		if ( g_CoopNetWeaponCache[i].iGun != iGun || g_CoopNetWeaponCache[i].iEntity != e ) continue;
		if ( ObjectExist ( g_CoopNetWeaponCache[i].iObj ) == 1 ) return g_CoopNetWeaponCache[i].iObj;
		g_CoopNetWeaponCache[i] = g_CoopNetWeaponCache[--g_iCoopNetWeaponCacheCount];
		break;
	}
	if ( g_iCoopNetWeaponCacheCount >= COOPNET_WEAPON_CACHE_MAX ) return 0;

	// same path an NPC character uses to get its weapon: load vweap, place per gunspec
	// hand settings and glue it to the character's firespot (hand) limb
	int storee = t.e, storeentid = t.entid;
	t.e = e;
	t.entid = t.entityelement[e].bankindex;
	t.entityelement[e].eleprof.hasweapon = iGun;
	t.entityelement[e].eleprof.hasweapon_s = t.gun[iGun].name_s;
	t.entityelement[e].attachmentobj = 0;
	entity_createattachment();
	int iObj = t.entityelement[e].attachmentobj;
	t.e = storee; t.entid = storeentid;
	if ( iObj <= 0 || ObjectExist ( iObj ) == 0 ) return 0;

	g_CoopNetWeaponCache[g_iCoopNetWeaponCacheCount].iEntity = e;
	g_CoopNetWeaponCache[g_iCoopNetWeaponCacheCount].iGun = iGun;
	g_CoopNetWeaponCache[g_iCoopNetWeaponCacheCount].iObj = iObj;
	g_iCoopNetWeaponCacheCount++;
	coopnet_log ( "puppet: made held weapon '%s' (object %d) newsystem=%d handpos %.1f %.1f %.1f handrot %.1f %.1f %.1f scale %.1f firespotlimb %d",
		t.gun[iGun].name_s.Get(), iObj, t.gun[iGun].handusesnewweaponsystem,
		t.gun[iGun].handposx_f, t.gun[iGun].handposy_f, t.gun[iGun].handposz_f,
		t.gun[iGun].handrotx_f, t.gun[iGun].handroty_f, t.gun[iGun].handrotz_f,
		t.gun[iGun].handscale_f, t.entityprofile[t.entityelement[e].bankindex].firespotlimb );
	return iObj;
}

static void coopnet_puppet_updateweapon ( int e, bool bShow )
{
	int iWantGun = bShow ? coopnet_findgun ( g_CoopNetRemote.pWeapon ) : 0;
	if ( bShow && iWantGun == 0 && g_CoopNetRemote.pWeapon[0] && strcmp ( g_CoopNetPuppet.pGunMissing, g_CoopNetRemote.pWeapon ) != 0 )
	{
		strcpy ( g_CoopNetPuppet.pGunMissing, g_CoopNetRemote.pWeapon );
		coopnet_event ( "puppet: weapon '%s' is not used in this level, shown unarmed", g_CoopNetRemote.pWeapon );
	}

	int iCurrentObj = t.entityelement[e].attachmentobj;
	if ( iWantGun == g_CoopNetPuppet.iGunShown )
	{
		// glued children do not follow the parent's hide/show, so mirror it
		if ( iCurrentObj > 0 && ObjectExist ( iCurrentObj ) == 1 && GetVisible ( iCurrentObj ) != ( bShow ? 1 : 0 ) )
		{
			if ( bShow ) ShowObject ( iCurrentObj ); else HideObject ( iCurrentObj );
		}
		return;
	}

	if ( iCurrentObj > 0 && ObjectExist ( iCurrentObj ) == 1 ) HideObject ( iCurrentObj );
	t.entityelement[e].attachmentobj = 0;
	t.entityelement[e].eleprof.hasweapon = 0;
	g_CoopNetPuppet.iGunShown = 0;
	if ( iWantGun == 0 ) return;

	int iObj = coopnet_weaponobject ( e, iWantGun );
	if ( iObj == 0 )
	{
		if ( strcmp ( g_CoopNetPuppet.pGunMissing, g_CoopNetRemote.pWeapon ) != 0 )
		{
			strcpy ( g_CoopNetPuppet.pGunMissing, g_CoopNetRemote.pWeapon );
			coopnet_event ( "puppet: could not attach '%s' (character has no hand limb, or no weapon model)", g_CoopNetRemote.pWeapon );
		}
		return;
	}
	t.entityelement[e].attachmentobj = iObj;
	t.entityelement[e].eleprof.hasweapon = iWantGun;
	t.entityelement[e].eleprof.hasweapon_s = t.gun[iWantGun].name_s;
	ShowObject ( iObj );
	g_CoopNetPuppet.iGunShown = iWantGun;
}

//
// Puppet animation set (which animations the character model has)
//

// defined in M-Importer.cpp: weapon -> animation set suffix ("-melee", "" = pistol, "-rifle", "-axe"...)
LPSTR animsystem_getweapontype ( LPSTR pSelectedWeapon, LPSTR pAnimSetOverride );
void WickedCall_RefreshObjectAnimations ( sObject* pObject, void* pstate );

static bool coopnet_animsetfor ( int e, int iGun, char* pOut )
{
	// same choice darkai makes for an NPC at level start (darkai_refresh_characters):
	// the entity's own override set, else the set for the weapon it holds
	pOut[0] = 0;
	LPSTR pOverride = t.entityelement[e].eleprof.overrideanimset_s.Get();
	if ( strlen ( pOverride ) > 1 )
	{
		if ( coopnet_gamefileexists ( pOverride ) ) { strcpy ( pOut, pOverride ); return true; }
		return false;
	}
	if ( strlen ( pOverride ) == 1 ) return false; // "-" = keep the object's own animations
	int entid = t.entityelement[e].bankindex;
	int iBase = t.entityprofile[entid].characterbasetype;
	if ( iBase != 0 && iBase != 1 ) return false;	// darkai only swaps sets for adult male/female
	LPSTR pWeaponType = animsystem_getweapontype ( iGun > 0 ? t.gun[iGun].name_s.Get() : (LPSTR)"",
		iGun > 0 ? t.gun[iGun].animsetoverride.Get() : (LPSTR)"" );
	sprintf ( pOut, "charactercreatorplus\\animations\\sets\\%s\\default animations%s.dbo", iBase == 0 ? "adult male" : "adult female", pWeaponType );
	return coopnet_gamefileexists ( pOut );
}

static void coopnet_puppet_updateanimset ( int iSlot, int e, int iObj )
{
	// The puppet held nothing at level start, so darkai gave it the melee set. Give it the
	// set for the remote player's weapon instead (once per change, per entity, so swapping
	// between a puppet and its melee twin never reloads anything).
	char pWant[MAX_PATH];
	if ( !coopnet_animsetfor ( e, coopnet_findgun ( g_CoopNetRemote.pWeapon ), pWant ) ) return;
	if ( _stricmp ( pWant, g_CoopNetClaimed[iSlot].pAnimSet ) == 0 ) return;
	sObject* pObject = GetObjectData ( iObj );
	if ( pObject == NULL ) return;
	AppendObject ( pWant, iObj, 0 );
	WickedCall_RefreshObjectAnimations ( pObject, pObject->wickedloaderstateptr );
	strcpy ( g_CoopNetClaimed[iSlot].pAnimSet, pWant );
	g_CoopNetPuppet.iAnim = COOPNET_ANIM_NONE;	// frame ranges changed, pick the animation again
	coopnet_log ( "puppet: %s animation set -> %s", g_pCoopNetPuppetNames[iSlot], pWant );
	coopnet_puppet_loganimationnames ( iObj );
}

//
// Puppet gunfire: muzzle flash, gunshot sound and tracer for each round the remote player
// fires. The cosmetic half of darkai_shoottarget / darkai_shooteffect (M-DAINew.cpp), which
// is how NPC characters fire - without the part that decides whether the player is hit.
//

#define COOPNET_SHOT_FX_GAP_MS		60		// minimum time between two shown shots (bursts arrive together)
#define COOPNET_SHOT_PENDING_MAX	6
#define COOPNET_TRACER_LENGTH		3000.0f

static void coopnet_puppet_muzzleposition ( int e, int iAttach, float* pX, float* pY, float* pZ )
{
	// as darkai_shooteffect: the firespot on the held weapon, else the weapon, else the chest
	int iLimb = t.entityelement[e].attachmentobjfirespotlimb;
	if ( iAttach > 0 && ObjectExist ( iAttach ) == 1 )
	{
		if ( iLimb == -1 )
		{
			int iDebugFirespotObj = g.entityattachments2offset + ( iAttach - g.entityattachmentsoffset );
			if ( ObjectExist ( iDebugFirespotObj ) == 1 )
			{
				*pX = LimbPositionX ( iDebugFirespotObj, 0 ); *pY = LimbPositionY ( iDebugFirespotObj, 0 ); *pZ = LimbPositionZ ( iDebugFirespotObj, 0 );
				return;
			}
		}
		else if ( iLimb != 0 )
		{
			*pX = LimbPositionX ( iAttach, iLimb ); *pY = LimbPositionY ( iAttach, iLimb ); *pZ = LimbPositionZ ( iAttach, iLimb );
			return;
		}
		*pX = ObjectPositionX ( iAttach ); *pY = ObjectPositionY ( iAttach ); *pZ = ObjectPositionZ ( iAttach );
		return;
	}
	int iObj = t.entityelement[e].obj;
	*pX = ObjectPositionX ( iObj ); *pY = ObjectPositionY ( iObj ) + 50.0f; *pZ = ObjectPositionZ ( iObj );
}

static void coopnet_puppet_shot ( int e, int iGun, DWORD dwNow )
{
	int iAttach = t.entityelement[e].attachmentobj;
	float fX, fY, fZ;
	coopnet_puppet_muzzleposition ( e, iAttach, &fX, &fY, &fZ );

	// light flash (lighting_spotflash_forenemies reads t.tx_f/ty_f/tz_f and t.tcolr/g/b)
	t.te = e; t.tgunid = iGun;
	t.tx_f = fX; t.ty_f = fY; t.tz_f = fZ;
	t.tcolr = g.firemodes[iGun][0].settings.muzzlecolorr / 5;
	t.tcolg = g.firemodes[iGun][0].settings.muzzlecolorg / 5;
	t.tcolb = g.firemodes[iGun][0].settings.muzzlecolorb / 5;
	lighting_spotflash_forenemies();

	// muzzle flash decal, set up as darkai_shooteffect does for characters
	t.decalid = g.firemodes[iGun][0].decalid;
	if ( t.decalid > 0 )
	{
		g.decalx = fX; g.decaly = fY; g.decalz = fZ;
		t.decalscalemodx = 0; t.decalorient = 11;
		t.originatore = -1;
		t.originatorobj = iAttach;
		t.decalforward = g.firemodes[iGun][0].settings.decalforward;
		if ( t.decalforward == 0 ) t.decalforward = 100;
		t.decalforward = t.decalforward * 2;
		t.decalburstloop = 0;
		decalelement_create();
		t.decalburstloop = 0;
	}

	// gunshot: one of the gun's third-person ("companion") fire sounds, as darkai_shoottarget
	int iSound = 0;
	if ( iGun < (int)t.gunsoundcompanion.size() && t.gunsoundcompanion[iGun].size() > 1 )
	{
		int iFirst = rand() % 2;
		for ( int tt = iFirst; tt <= iFirst + 1 && tt < (int)t.gunsoundcompanion[iGun][1].size(); tt++ )
		{
			int iSnd = t.gunsoundcompanion[iGun][1][tt].soundid;
			if ( iSnd > 0 && SoundExist ( iSnd ) == 1 && ( SoundPlaying ( iSnd ) == 0 || tt == iFirst + 1 ) ) { iSound = iSnd; break; }
		}
	}
	if ( iSound > 0 )
	{
		int iLoopEnd = g.firemodes[iGun][0].sound.fireloopend;
		if ( iLoopEnd > 0 )
		{
			// automatic weapon with a looping fire sound: keep it going while rounds keep coming
			if ( g_CoopNetPuppet.iFireSound != iSound || SoundPlaying ( iSound ) == 0 )
			{
				if ( g_CoopNetPuppet.iFireSound > 0 && g_CoopNetPuppet.iFireSound != iSound && SoundExist ( g_CoopNetPuppet.iFireSound ) == 1 ) StopSound ( g_CoopNetPuppet.iFireSound );
				PlaySoundOffset ( iSound, iLoopEnd );
				LoopSound ( iSound, 0, iLoopEnd );
			}
			g_CoopNetPuppet.iFireSound = iSound;
			g_CoopNetPuppet.dwFireSoundExpiry = dwNow + 200 + rand() % 200;
		}
		else if ( iLoopEnd < 0 )
		{
			LoopSound ( iSound, 0, 5000 );
			g_CoopNetPuppet.iFireSound = iSound;
			g_CoopNetPuppet.dwFireSoundExpiry = dwNow + 500;
		}
		else
		{
			PlaySound ( iSound );
		}
		PositionSound ( iSound, t.entityelement[e].x, t.entityelement[e].y, t.entityelement[e].z );
		SetSoundVolume ( iSound, (int)soundtruevolume ( 95 ) );
		SetSoundSpeed ( iSound, 43000 + rand() % 2000 );
	}

	// tracer, if the gun has them, along where the remote player is looking
	if ( t.gun[iGun].settings.tracer_active )
	{
		float fYaw = g_CoopNetPuppet.shown.yaw * 0.0174533f, fPitch = g_CoopNetPuppet.shown.pitch * 0.0174533f;
		XMFLOAT3 from ( fX, fY, fZ );
		XMFLOAT3 to ( fX + sinf ( fYaw ) * cosf ( fPitch ) * COOPNET_TRACER_LENGTH,
			fY - sinf ( fPitch ) * COOPNET_TRACER_LENGTH,
			fZ + cosf ( fYaw ) * cosf ( fPitch ) * COOPNET_TRACER_LENGTH );
		Tracers::AddTracer ( from, to, t.gun[iGun].settings.tracer_lifetime,
			XMFLOAT4 ( t.gun[iGun].settings.tracer_colorR, t.gun[iGun].settings.tracer_colorG, t.gun[iGun].settings.tracer_colorB, 1 ),
			t.gun[iGun].settings.tracer_glow, t.gun[iGun].settings.tracer_scrollV, t.gun[iGun].settings.tracer_scaleV,
			t.gun[iGun].settings.tracer_width, t.gun[iGun].settings.tracer_maxlength, iGun );
	}
}

static void coopnet_puppet_fireeffects ( int e, DWORD dwNow )
{
	// count rounds from the remote shot counter (at the displayed time, like the animation)
	unsigned char uShots = g_CoopNetPuppet.shown.shotCount;
	if ( !g_CoopNetPuppet.bShotsSeen ) { g_CoopNetPuppet.uLastShotCount = uShots; g_CoopNetPuppet.bShotsSeen = true; }
	int iNew = (unsigned char)( uShots - g_CoopNetPuppet.uLastShotCount );
	g_CoopNetPuppet.uLastShotCount = uShots;
	g_CoopNetPuppet.iPendingShots += iNew;
	if ( g_CoopNetPuppet.iPendingShots > COOPNET_SHOT_PENDING_MAX ) g_CoopNetPuppet.iPendingShots = COOPNET_SHOT_PENDING_MAX;

	// a looping automatic fire sound stops shortly after the rounds stop
	if ( g_CoopNetPuppet.iFireSound > 0 && (int)( dwNow - g_CoopNetPuppet.dwFireSoundExpiry ) > 0 )
	{
		if ( SoundExist ( g_CoopNetPuppet.iFireSound ) == 1 ) StopSound ( g_CoopNetPuppet.iFireSound );
		g_CoopNetPuppet.iFireSound = 0;
	}

	int iGun = g_CoopNetPuppet.iGunShown;
	bool bGun = iGun > 0 && t.gun[iGun].weapontype < 51 && t.gun[iGun].settings.ismelee == 0;
	if ( !bGun || !g_CoopNetPuppet.bVisible ) { g_CoopNetPuppet.iPendingShots = 0; return; }
	if ( g_CoopNetPuppet.iPendingShots > 0 && (int)( dwNow - g_CoopNetPuppet.dwNextShotFx ) >= 0 )
	{
		coopnet_puppet_shot ( e, iGun, dwNow );
		g_CoopNetPuppet.iPendingShots--;
		g_CoopNetPuppet.dwNextShotFx = dwNow + COOPNET_SHOT_FX_GAP_MS;
	}
}

static void coopnet_puppet_update ( DWORD dwNow )
{
	// switch between the named entity and the placeholder box as the level allows
	int iSlot = coopnet_puppetentityslot();
	int iWantEntity = iSlot >= 0 ? g_CoopNetClaimed[iSlot].e : 0;
	if ( iWantEntity != g_CoopNetPuppet.iEntity )
	{
		// old entity still in this level (melee twin swap, or Player1 appeared later):
		// put its weapon away; its cached weapon objects stay for when it is used again
		int iOldE = g_CoopNetPuppet.iEntity;
		for ( int i = 0; i < COOPNET_PUPPET_NAME_COUNT; i++ )
			if ( iOldE > 0 && g_CoopNetClaimed[i].e == iOldE ) coopnet_puppet_updateweapon ( iOldE, false );
		g_CoopNetPuppet.iGunShown = 0;
		g_CoopNetPuppet.pGunMissing[0] = 0;
		coopnet_puppet_hide();
		if ( ObjectExist ( COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) ) == 1 && g_CoopNetPuppet.iObj == COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) ) DeleteObject ( COOPNET_PLACEHOLDER_FOR ( g_iCoopNetCur ) );
		g_CoopNetPuppet.iEntity = iWantEntity;
		g_CoopNetPuppet.iSlot = iWantEntity > 0 ? iSlot : -1;
		g_CoopNetPuppet.iObj = iWantEntity > 0 ? t.entityelement[iWantEntity].obj : 0;
		// new model must start its animation; walk/run state carries over a twin swap
		g_CoopNetPuppet.iAnim = COOPNET_ANIM_NONE;
		if ( iWantEntity > 0 )
		{
			coopnet_log ( "puppet: using %s entity (e=%d)", g_pCoopNetPuppetNames[iSlot], iWantEntity );
			if ( !g_CoopNetClaimed[iSlot].bAnimsLogged )
			{
				coopnet_puppet_loganimationnames ( g_CoopNetPuppet.iObj );
				g_CoopNetClaimed[iSlot].bAnimsLogged = true;
			}
		}
	}

	bool bHaveRemote = g_bCoopNetHandshakeDone && g_CoopNetRemote.bValid
		&& (int)( dwNow - g_CoopNetRemote.dwLastReceived ) < COOPNET_PUPPET_HIDE_MS;	// signed: packets serviced after dwNow was read are "in the future"

	// the partner respawned (dead -> alive): jump to the respawn point instead of gliding there
	bool bRemoteDead = g_CoopNetRemote.bValid && g_CoopNetRemote.state.health <= 0;
	if ( g_CoopNetPuppet.bRemoteWasDead && !bRemoteDead && g_iCoopNetSnapshotCount > 1 )
	{
		g_iCoopNetSnapshotStart = ( g_iCoopNetSnapshotStart + g_iCoopNetSnapshotCount - 1 ) % COOPNET_SNAPSHOT_COUNT;
		g_iCoopNetSnapshotCount = 1;
		g_CoopNetPuppet.iAnim = COOPNET_ANIM_NONE;
	}
	g_CoopNetPuppet.bRemoteWasDead = bRemoteDead;

	// render time on the sender's timeline (see coopnet_addsnapshot)
	DWORD dwRenderTime = dwNow - g_dwCoopNetTimeOffset - COOPNET_INTERP_DELAY_MS;
	bool bShown = bHaveRemote && coopnet_interpolate ( dwRenderTime, &g_CoopNetPuppet.shown );
	// hidden while we spectate from its eyes, or while dead if it has no death animation
	bool bDeadHidden = g_CoopNetPuppet.shown.health <= 0 && g_CoopNetPuppet.bDeathAnimMissing && g_CoopNetPuppet.iEntity > 0;
	bool bWatching = g_bCoopNetSpectating && g_iCoopNetSpectateID == g_iCoopNetCur;
	if ( !bShown || bWatching || bDeadHidden )
	{
		if ( g_CoopNetPuppet.iEntity > 0 ) coopnet_puppet_updateweapon ( g_CoopNetPuppet.iEntity, false );
		coopnet_puppet_hide();
		return;
	}

	int e = g_CoopNetPuppet.iEntity;
	if ( e > 0 )
	{
		// character: feet on the ground under the remote capsule, facing where they look
		int iObj = g_CoopNetPuppet.iObj;
		t.entityelement[e].x = g_CoopNetPuppet.shown.x;
		t.entityelement[e].y = g_CoopNetPuppet.shown.y - COOPNET_CAPSULE_TO_FEET;
		t.entityelement[e].z = g_CoopNetPuppet.shown.z;
		t.entityelement[e].ry = g_CoopNetPuppet.shown.yaw;
		PositionObject ( iObj, t.entityelement[e].x, t.entityelement[e].y, t.entityelement[e].z );
		RotateObject ( iObj, 0.0f, t.entityelement[e].ry, 0.0f );
		if ( g_CoopNetPuppet.iSlot >= 0 ) coopnet_puppet_updateanimset ( g_CoopNetPuppet.iSlot, e, iObj );
		coopnet_puppet_animate ( e, iObj, dwNow );
		coopnet_puppet_updateweapon ( e, true );
		coopnet_puppet_fireeffects ( e, dwNow );
	}
	else
	{
		// placeholder box, centred like the capsule
		if ( !coopnet_puppet_ensureplaceholder() ) return;
		PositionObject ( g_CoopNetPuppet.iObj, g_CoopNetPuppet.shown.x, g_CoopNetPuppet.shown.y, g_CoopNetPuppet.shown.z );
		RotateObject ( g_CoopNetPuppet.iObj, 0.0f, g_CoopNetPuppet.shown.yaw, 0.0f );
	}
	if ( !g_CoopNetPuppet.bVisible || GetVisible ( g_CoopNetPuppet.iObj ) == 0 )
	{
		ShowObject ( g_CoopNetPuppet.iObj );
		g_CoopNetPuppet.bVisible = true;
	}
}

static void coopnet_puppet_free ( void )
{
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		sCoopNetPuppet& p = g_CoopNetPlayers[id].puppet;
		if ( p.iObj == COOPNET_PLACEHOLDER_FOR ( id ) && ObjectExist ( COOPNET_PLACEHOLDER_FOR ( id ) ) == 1 )
			DeleteObject ( COOPNET_PLACEHOLDER_FOR ( id ) );
		memset ( &p, 0, sizeof(p) );
	}
	g_iCoopNetWeaponCacheCount = 0;
}

//
// Zombie sync: the host runs every character's AI and sends its state; the client
// stops running its own copies and mirrors the host's
//


static bool coopnet_isclaimedentity ( int e )
{
	for ( int i = 0; i < COOPNET_PUPPET_NAME_COUNT; i++ )
		if ( g_CoopNetClaimed[i].e == e ) return true;
	return false;
}

static bool coopnet_issynccharacter ( int e )
{
	if ( e <= 0 || e > coopnet_levelentitylimit() ) return false;
	int entid = t.entityelement[e].bankindex;
	if ( entid <= 0 || t.entityprofile[entid].ischaracter != 1 ) return false;
	int iObj = t.entityelement[e].obj;
	if ( iObj <= 0 || ObjectExist ( iObj ) == 0 ) return false;
	return !coopnet_isclaimedentity ( e );	// Player1/Player2 entities are puppets on both sides
}

static void coopnet_senddeath ( int e )
{
	// impact direction the engine recorded for its ragdoll-plus death (1 front, 2 back, 3 right,
	// 4 left, 5 back, 6 any; +10 once entity_loop has picked it up); none = "any"
	int iDir = t.entityelement[e].ragdollplusactivate;
	if ( iDir >= 10 && iDir < 20 ) iDir -= 10;
	if ( iDir < 1 || iDir > 6 ) iDir = 6;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_DEATH );
	w.put<unsigned short> ( (unsigned short)e );
	w.put<unsigned char> ( (unsigned char)iDir );
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	coopnet_log ( "zombie e=%d died, direction %d", e, iDir );
}

static void coopnet_sendzombies ( void )
{
	// host, 20 Hz, same tick and sequence number as PLAYERSTATE (so one shared timeline)
	// deaths first: a character whose health just reached zero
	if ( (int)g_CoopNetHostHealth.size() < coopnet_levelentitylimit() + 1 ) g_CoopNetHostHealth.resize ( coopnet_levelentitylimit() + 1, 1 );
	for ( int e = 1; e <= coopnet_levelentitylimit(); e++ )
	{
		if ( !coopnet_issynccharacter ( e ) ) continue;
		int iHealth = t.entityelement[e].health;
		if ( iHealth <= 0 && g_CoopNetHostHealth[e] > 0 ) coopnet_senddeath ( e );
		g_CoopNetHostHealth[e] = iHealth;
	}
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_ZOMBIES );
	w.put<unsigned int> ( g_uCoopNetSendSeq );
	w.put<unsigned int> ( timeGetTime() );
	size_t countPos = w.len;
	w.put<unsigned short> ( 0 );
	unsigned short uCount = 0;
	for ( int e = 1; e <= coopnet_levelentitylimit(); e++ )
	{
		if ( !coopnet_issynccharacter ( e ) ) continue;
		int iObj = t.entityelement[e].obj;
		sObject* pObject = GetObjectData ( iObj );
		unsigned char flags = 0;
		if ( pObject && pObject->bAnimLooping ) flags |= COOPNET_ZOMBIE_FLAG_LOOP;
		if ( GetVisible ( iObj ) == 1 ) flags |= COOPNET_ZOMBIE_FLAG_VISIBLE;
		w.put<unsigned short> ( (unsigned short)e );
		w.put<float> ( ObjectPositionX ( iObj ) );
		w.put<float> ( ObjectPositionY ( iObj ) );
		w.put<float> ( ObjectPositionZ ( iObj ) );
		w.put<float> ( ObjectAngleY ( iObj ) );
		w.put<unsigned short> ( pObject ? (unsigned short)pObject->fAnimLoopStart : 0 );
		w.put<unsigned short> ( pObject ? (unsigned short)pObject->fAnimFrameEnd : 0 );
		w.put<unsigned char> ( flags );
		w.put<float> ( GetFrame ( iObj ) );
		w.put<short> ( (short)GetSpeed ( iObj ) );
		if ( w.bOverflow ) break;
		uCount++;
	}
	if ( w.bOverflow ) return;	// 8 KB = ~290 characters; never expected in a GameGuru level
	memcpy ( w.buf + countPos, &uCount, sizeof(uCount) );
	g_iCoopNetZombiesSent = uCount;
	coopnet_send ( w, COOPNET_CHANNEL_STATE, 0 );
}

static void coopnet_zombies_receive ( sCoopNetReader& r )
{
	unsigned int seq = r.get<unsigned int>();
	unsigned int sendTime = r.get<unsigned int>();
	unsigned short uCount = r.get<unsigned short>();
	if ( !r.bOK || g_CoopNetMode != COOPNET_CLIENT || !coopnet_levelsmatch() ) return;
	if ( (int)g_CoopNetZombies.size() < coopnet_levelentitylimit() + 1 ) g_CoopNetZombies.resize ( coopnet_levelentitylimit() + 1 );
	DWORD dwTime = sendTime;
	if ( g_uCoopNetLastZombieSeq != 0 && (int)( seq - g_uCoopNetLastZombieSeq ) <= 0 ) return;	// older than one already applied
	g_uCoopNetLastZombieSeq = seq;
	for ( unsigned short i = 0; i < uCount && r.bOK; i++ )
	{
		sCoopNetZombieState s;
		int e = r.get<unsigned short>();
		s.dwTime = dwTime;
		s.x = r.get<float>(); s.y = r.get<float>(); s.z = r.get<float>(); s.ry = r.get<float>();
		s.animStart = r.get<unsigned short>(); s.animEnd = r.get<unsigned short>();
		s.flags = r.get<unsigned char>();
		s.frame = r.get<float>();
		s.speed = r.get<short>();
		if ( !r.bOK || e <= 0 || e >= (int)g_CoopNetZombies.size() ) continue;
		sCoopNetZombie& z = g_CoopNetZombies[e];
		if ( z.iCount > 0 && (int)( s.dwTime - z.snaps[z.iCount - 1].dwTime ) <= 0 ) continue;	// not newer
		if ( z.iCount == COOPNET_ZOMBIE_SNAPSHOTS )
		{
			memmove ( &z.snaps[0], &z.snaps[1], sizeof(z.snaps[0]) * ( COOPNET_ZOMBIE_SNAPSHOTS - 1 ) );
			z.iCount--;
		}
		z.snaps[z.iCount++] = s;
	}
}

void WickedCall_SetObjectFrameEx ( sObject* pObject, float fFrame );

static void coopnet_setframekeepplaying ( int iObj, float fFrame )
{
	// NOT SetObjectFrame: in the Wicked build that stops the animation and turns looping off
	// (WickedCall_SetObjectFrame), which froze mirrored zombies. The Ex variant is the one the
	// engine's own transitions use to set a frame while playing (smoothanimupdate).
	sObject* pObject = GetObjectData ( iObj );
	if ( pObject ) WickedCall_SetObjectFrameEx ( pObject, fFrame );
}

static void coopnet_zombie_takeover ( int e )
{
	// every frame (as for the puppet): no Lua script, out of darkai. Physics stays on,
	// so the client still collides with zombies; the capsule is steered to the host position.
	t.entityelement[e].lua.outofrangefreeze = 1;
	for ( int n = 1; n <= g.charanimindexmax; n++ )
	{
		if ( t.charanimstates[n].e == e )
		{
			t.charanimstates[n].e = 0;
			t.charanimstates[n].obj = 0;
		}
	}
}

static void coopnet_applydeath ( int e, int iDir )
{
	// client: the host's zombie died. Stop mirroring and let this machine's engine run its own
	// death sequence on its copy, from the same direction (ragdoll-plus in entity_loop: fit a
	// Death_* animation if there is room, then ragdoll) - bone physics can't be mirrored.
	if ( !coopnet_issynccharacter ( e ) ) return;
	if ( (int)g_CoopNetZombies.size() < e + 1 ) g_CoopNetZombies.resize ( e + 1 );
	if ( g_CoopNetZombies[e].bDead ) return;
	g_CoopNetZombies[e].bDead = true;
	t.entityelement[e].health = 0;
	// as entity_applydamage does for a dead character: never call its script's main again, but
	// let the Lua tables update (corpse faders etc. read g_Entity[e].health)
	t.entityelement[e].eleprof.aimain = 0;
	t.entityelement[e].lua.outofrangefreeze = 0;
	t.entityelement[e].lua.flagschanged = 1;
	int entid = t.entityelement[e].bankindex;
	if ( t.entityprofile[entid].ragdoll == 1 && t.entityelement[e].ragdollplusactivate == 0 )
		t.entityelement[e].ragdollplusactivate = iDir;
	coopnet_log ( "host zombie e=%d died (direction %d) - local death sequence", e, iDir );
}

static void coopnet_zombies_update ( DWORD dwNow )
{
	// client: mirror every synced character from the host's snapshots
	g_iCoopNetZombiesMirrored = 0;
	// zombie snapshots are on the host's clock: use the host's (player 0) time offset
	if ( g_CoopNetMode != COOPNET_CLIENT || !coopnet_levelsmatch() || !g_CoopNetPlayers[0].bTimeOffsetValid ) return;
	DWORD dwRenderTime = dwNow - g_CoopNetPlayers[0].dwTimeOffset - COOPNET_INTERP_DELAY_MS;
	int iLimit = coopnet_levelentitylimit();
	if ( (int)g_CoopNetZombies.size() < iLimit + 1 ) return;
	for ( int e = 1; e <= iLimit; e++ )
	{
		sCoopNetZombie& z = g_CoopNetZombies[e];
		if ( z.iCount == 0 || !coopnet_issynccharacter ( e ) ) continue;
		if ( z.bDead )
		{
			// local death sequence owns it; only follow the host hiding the corpse (in case this
			// machine has nothing that fades corpses - the engine's own fade needs darkai)
			const sCoopNetZombieState& last = z.snaps[z.iCount - 1];
			int iObj = t.entityelement[e].obj;
			if ( ( last.flags & COOPNET_ZOMBIE_FLAG_VISIBLE ) == 0 && GetVisible ( iObj ) == 1 ) HideObject ( iObj );
			continue;
		}
		coopnet_zombie_takeover ( e );

		// position/angle between the two snapshots either side of the render time
		const sCoopNetZombieState* a = &z.snaps[0];
		const sCoopNetZombieState* b = &z.snaps[z.iCount - 1];
		for ( int i = 1; i < z.iCount; i++ )
		{
			if ( (int)( z.snaps[i].dwTime - dwRenderTime ) > 0 ) { a = &z.snaps[i - 1]; b = &z.snaps[i]; break; }
			a = &z.snaps[i]; b = &z.snaps[i];
		}
		float f = 1.0f;
		if ( b->dwTime != a->dwTime )
		{
			f = (float)(int)( dwRenderTime - a->dwTime ) / (float)(int)( b->dwTime - a->dwTime );
			if ( f < 0.0f ) f = 0.0f; if ( f > 1.0f ) f = 1.0f;
		}
		if ( (int)( dwRenderTime - z.snaps[0].dwTime ) < 0 ) g_iCoopNetZombieStatEarly++;
		else if ( (int)( dwRenderTime - z.snaps[z.iCount - 1].dwTime ) > 0 ) g_iCoopNetZombieStatLate++;
		else g_iCoopNetZombieStatOK++;
		float x = a->x + ( b->x - a->x ) * f;
		float y = a->y + ( b->y - a->y ) * f;
		float zz = a->z + ( b->z - a->z ) * f;
		float ry = coopnet_lerpangle ( a->ry, b->ry, f );

		int iObj = t.entityelement[e].obj;
		t.entityelement[e].x = x; t.entityelement[e].y = y; t.entityelement[e].z = zz; t.entityelement[e].ry = ry;
		// place the physics body and model exactly at the host's (interpolated) position every
		// frame. Steering the capsule with a velocity (entity_updatepos, darkai's mover) could
		// not keep up and zombies trailed far behind. The body still collides with the player.
		if ( t.entityelement[e].usingphysicsnow == 1 )
		{
			ODESetBodyPosition ( iObj, x, y, zz );
			ODESetLinearVelocity ( iObj, 0, 0, 0 );
		}
		PositionObject ( iObj, x, y, zz );
		RotateObject ( iObj, 0.0f, ry, 0.0f );
		coopnet_puppet_lockrootmotion ( iObj );

		// animation at the same moment as the position (render time, not the newest snapshot):
		// the range/mode from the snapshot being shown, and the host's frame at that moment
		const sCoopNetZombieState& n = ( f < 0.5f ) ? *a : *b;
		bool bLoop = ( n.flags & COOPNET_ZOMBIE_FLAG_LOOP ) != 0;
		float fLoopLength = (float)( n.animEnd - n.animStart );
		float fHostFrame = n.frame;
		if ( a->animStart == b->animStart && a->animEnd == b->animEnd && a != b )
		{
			float fb = b->frame;
			if ( bLoop && fLoopLength > 0.0f && fb < a->frame ) fb += fLoopLength;	// wrapped between the two
			fHostFrame = a->frame + ( fb - a->frame ) * f;
			if ( bLoop && fLoopLength > 0.0f && fHostFrame >= n.animEnd ) fHostFrame -= fLoopLength;
		}
		int iSpeed = n.speed;
		if ( n.animEnd > n.animStart && ( !z.bAnimApplied || n.animStart != z.appliedStart || n.animEnd != z.appliedEnd || bLoop != ( z.appliedLoop != 0 ) ) )
		{
			if ( bLoop ) LoopObject ( iObj, n.animStart, n.animEnd ); else PlayObject ( iObj, n.animStart, n.animEnd );
			coopnet_setframekeepplaying ( iObj, fHostFrame );
			z.bAnimApplied = true; z.appliedStart = n.animStart; z.appliedEnd = n.animEnd; z.appliedLoop = bLoop ? 1 : 0;
			g_iCoopNetZombieStatAnimChange++;
		}
		else if ( z.bAnimApplied )
		{
			// signed drift, round the loop so the wrap from last frame to first isn't a drift.
			// Small drift: nudge playback speed (up to +/-50%) until it lines up - a hard frame
			// set is a visible stutter. Only jump if far out (e.g. after a network stall).
			float fDrift = fHostFrame - GetFrame ( iObj );
			if ( bLoop && fLoopLength > 0.0f )
			{
				if ( fDrift > fLoopLength * 0.5f ) fDrift -= fLoopLength;
				if ( fDrift < -fLoopLength * 0.5f ) fDrift += fLoopLength;
			}
			if ( fabsf ( fDrift ) > COOPNET_ZOMBIE_FRAME_SNAP )
			{
				coopnet_setframekeepplaying ( iObj, fHostFrame );
				g_iCoopNetZombieStatResync++;
			}
			else
			{
				float fCorrect = fDrift / COOPNET_ZOMBIE_FRAME_SNAP;	// -1..1
				iSpeed = (int)( n.speed * ( 1.0f + 0.5f * fCorrect ) );
			}
		}
		if ( iSpeed != z.appliedSpeed ) { SetObjectSpeed ( iObj, iSpeed ); z.appliedSpeed = (short)iSpeed; }

		// diagnostics: once a second, what the first mirrored zombie's animation is doing
		if ( g_iCoopNetZombiesMirrored == 0 && (int)( dwNow - g_dwCoopNetNextZombieAnimLog ) >= 0 )
		{
			g_dwCoopNetNextZombieAnimLog = dwNow + 1000;
			sObject* pObj = GetObjectData ( iObj );
			coopnet_log ( "zombie e=%d host: range %d-%d loop %d frame %.1f speed %d | client: frame %.1f speed %.0f set %d playing %d looping %d range %.0f-%.0f",
				e, n.animStart, n.animEnd, bLoop ? 1 : 0, fHostFrame, n.speed, GetFrame ( iObj ), GetSpeed ( iObj ), iSpeed,
				GetPlaying ( iObj ), GetLooping ( iObj ), pObj ? pObj->fAnimLoopStart : -1.0f, pObj ? pObj->fAnimFrameEnd : -1.0f );
		}

		bool bVisible = ( n.flags & COOPNET_ZOMBIE_FLAG_VISIBLE ) != 0;
		if ( bVisible != ( GetVisible ( iObj ) == 1 ) ) { if ( bVisible ) ShowObject ( iObj ); else HideObject ( iObj ); }
		g_iCoopNetZombiesMirrored++;
	}
}

//
// Nearest-player targeting (host): each character chases the nearer of the two players.
// Engine hooks ask coopnet_targetremote(); it is read from the visibility worker thread,
// so the per-entity table never reallocates and the remote values are plain copies.
//

#define COOPNET_TARGET_MAX_ENTITY	65536
#define COOPNET_TARGET_EVAL_MS		250		// re-pick targets this often
#define COOPNET_TARGET_SWITCH		0.85f	// the other player must be 15% nearer to switch
#define COOPNET_EYE_ABOVE_CAPSULE	28.5f	// measured: camera 28.5 above the player capsule

// host: the player each character chases - 0 = the host player itself, 1..3 = that client
static unsigned char g_CoopNetTarget[COOPNET_TARGET_MAX_ENTITY];
static volatile bool g_bCoopNetTargetingActive = false;
static volatile bool g_bCoopNetTargetLive[COOPNET_MAX_PLAYERS];	// client in play on our level (index 0 unused)
static volatile float g_fCoopNetTargetX[COOPNET_MAX_PLAYERS], g_fCoopNetTargetY[COOPNET_MAX_PLAYERS], g_fCoopNetTargetZ[COOPNET_MAX_PLAYERS];
static volatile int g_iCoopNetTargetHealth[COOPNET_MAX_PLAYERS];
static DWORD g_dwCoopNetNextTargetEval = 0;
static int g_iCoopNetChasingRemote = 0;		// overlay: characters currently chasing a client
static bool g_bCoopNetLuaSwapped = false;
static int g_iCoopNetLuaEntity = 0;			// entity whose script is running with a client as its target
static int g_iCoopNetLuaTarget = 0;			// ... that client's player id
static bool g_bCoopNetFrozenByRemote = false;	// client: a host zombie froze us (undo if the host goes away)
static int g_iCoopNetGunBeforeGrab = 0;		// client: our own weapon when a host zombie took it away

static int coopnet_targetof ( int e )
{
	// host: player id character e chases (0 = the host player, also when targeting is off)
	if ( !g_bCoopNetTargetingActive || e <= 0 || e >= COOPNET_TARGET_MAX_ENTITY ) return 0;
	int id = g_CoopNetTarget[e];
	return id > 0 && id < COOPNET_MAX_PLAYERS && g_bCoopNetTargetLive[id] ? id : 0;
}

bool coopnet_targetremote ( int e, float* pX, float* pY, float* pZ, float* pEyeY, int* pHealth )
{
	int id = coopnet_targetof ( e );
	if ( id == 0 ) return false;
	if ( pX ) *pX = g_fCoopNetTargetX[id];
	if ( pY ) *pY = g_fCoopNetTargetY[id];
	if ( pZ ) *pZ = g_fCoopNetTargetZ[id];
	if ( pEyeY ) *pEyeY = g_fCoopNetTargetY[id] + COOPNET_EYE_ABOVE_CAPSULE;
	if ( pHealth ) *pHealth = g_iCoopNetTargetHealth[id];
	return true;
}

static int g_iCoopNetLuaSyncRunning = 0;	// host: synced character whose script is running (its world side effects are replayed on the client)

void coopnet_lua_beginentity ( int e )
{
	if ( g_CoopNetMode == COOPNET_HOST && g_bCoopNetConnected && g_bCoopNetHandshakeDone && coopnet_levelsmatch() && coopnet_issynccharacter ( e ) )
		g_iCoopNetLuaSyncRunning = e;
	float x, y, z; int iHealth;
	if ( !coopnet_targetremote ( e, &x, &y, &z, NULL, &iHealth ) ) return;
	LuaSetFloat ( "g_PlayerPosX", x );
	LuaSetFloat ( "g_PlayerPosY", y );
	LuaSetFloat ( "g_PlayerPosZ", z );
	LuaSetInt ( "g_PlayerHealth", iHealth );
	g_bCoopNetLuaSwapped = true;
	g_iCoopNetLuaEntity = e;
	g_iCoopNetLuaTarget = coopnet_targetof ( e );
}

void coopnet_lua_endentity ( void )
{
	g_iCoopNetLuaSyncRunning = 0;
	if ( !g_bCoopNetLuaSwapped ) return;
	g_bCoopNetLuaSwapped = false;
	// same values lua_loop_begin set for the local player this frame
	int iObj = t.aisystem.objectstartindex;
	if ( ObjectExist ( iObj ) == 1 )
	{
		LuaSetFloat ( "g_PlayerPosX", ObjectPositionX ( iObj ) );
		LuaSetFloat ( "g_PlayerPosY", ObjectPositionY ( iObj ) );
		LuaSetFloat ( "g_PlayerPosZ", ObjectPositionZ ( iObj ) );
	}
	LuaSetInt ( "g_PlayerHealth", t.player[t.plrid].health );
}

bool coopnet_hurtremoteplayer ( int e, int iDamage )
{
	// host: a character chasing a client hits them, not us. Decided by whose script is running,
	// not e: the behaviours' 'hurt with shake' action calls HurtPlayer(0, damage).
	int id = g_bCoopNetLuaSwapped ? g_iCoopNetLuaTarget : coopnet_targetof ( e );
	if ( id == 0 || !g_bCoopNetConnected ) return false;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_DAMAGE );
	w.put<unsigned short> ( (unsigned short)e );
	w.put<short> ( (short)( iDamage > 32767 ? 32767 : iDamage ) );
	coopnet_sendtoplayer ( id, w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	coopnet_log ( "player %d hurt by e=%d for %d", id + 1, e, iDamage );
	return true;
}

bool coopnet_redirectplayercommand ( int iCmd, int iValue, float fX, float fY, float fZ )
{
	// host: only while a character chasing a client runs its script (zombie grab/bite)
	if ( !g_bCoopNetLuaSwapped || g_iCoopNetLuaTarget == 0 || !g_bCoopNetConnected ) return false;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_PLAYERCMD );
	w.put<unsigned char> ( (unsigned char)iCmd );
	w.put<unsigned short> ( (unsigned short)g_iCoopNetLuaEntity );
	w.put<float> ( fX ); w.put<float> ( fY ); w.put<float> ( fZ );
	w.put<int> ( iValue );
	// camera position/angle are sent every frame while grabbed: latest wins, unreliable is fine
	bool bEveryFrame = ( iCmd == COOPNET_PCMD_CAMERAPOSITION || iCmd == COOPNET_PCMD_CAMERAANGLE );
	coopnet_sendtoplayer ( g_iCoopNetLuaTarget, w, bEveryFrame ? COOPNET_CHANNEL_STATE : COOPNET_CHANNEL_EVENTS, bEveryFrame ? 0 : ENET_PACKET_FLAG_RELIABLE );
	if ( !bEveryFrame ) coopnet_log ( "redirected player command %d (value %d) from e=%d to player %d", iCmd, iValue, g_iCoopNetLuaEntity, g_iCoopNetLuaTarget + 1 );
	return true;
}

static void coopnet_applyplayercommand ( int iCmd, int e, float fX, float fY, float fZ, int iValue )
{
	// client: what the host zombie's script asked "the player" to do, done to us (the same
	// engine calls the Lua commands make)
	switch ( iCmd )
	{
		case COOPNET_PCMD_CAMERAOVERRIDE: g.luacameraoverride = iValue; break;
		case COOPNET_PCMD_CAMERAPOSITION: PositionCamera ( 0, fX, fY, fZ ); break;
		case COOPNET_PCMD_CAMERAANGLE: RotateCamera ( 0, fX, fY, fZ ); break;
		case COOPNET_PCMD_FREEZE: lua_freezeplayer(); g_bCoopNetFrozenByRemote = true; break;
		case COOPNET_PCMD_UNFREEZE: lua_unfreezeplayer(); g_bCoopNetFrozenByRemote = false; break;
		case COOPNET_PCMD_WEAPON:
		{
			// the script restores the weapon it saw (the host's), so give back our own instead
			int iWeapon = iValue;
			if ( iValue == 0 ) g_iCoopNetGunBeforeGrab = t.gunid;
			else iWeapon = g_iCoopNetGunBeforeGrab;
			int storev = t.v; t.v = iWeapon;
			entity_lua_changeplayerweaponid();
			t.v = storev;
			break;
		}
	}
	if ( iCmd != COOPNET_PCMD_CAMERAPOSITION && iCmd != COOPNET_PCMD_CAMERAANGLE )
		coopnet_log ( "host zombie e=%d player command %d (value %d)", e, iCmd, iValue );
}

static void coopnet_undoremotefreeze ( void )
{
	// client: never stay frozen / camera-locked / unarmed if the host goes away mid-grab
	if ( g_CoopNetMode != COOPNET_CLIENT ) return;
	if ( g.luacameraoverride != 0 ) g.luacameraoverride = 0;
	if ( g_bCoopNetFrozenByRemote )
	{
		lua_unfreezeplayer();
		g_bCoopNetFrozenByRemote = false;
		if ( g_iCoopNetGunBeforeGrab > 0 ) { int storev = t.v; t.v = g_iCoopNetGunBeforeGrab; entity_lua_changeplayerweaponid(); t.v = storev; }
		coopnet_event ( "host left during a zombie grab - player unfrozen" );
	}
}

//
// Host-authoritative hits, blood at impacts, replayed zombie-script commands (protocol v5)
//

struct sCoopNetHitInputs
{
	// everything entity_applydamage reads from its callers (t.* globals)
	int ttte, tdamage, tdamageforce, tdamagesource, impacting, bulletraylimbhit, bulletraytype;
	float tdamagemultiplier_f, brayx1_f, brayy1_f, brayz1_f, brayx2_f, brayy2_f, brayz2_f;
};

static void coopnet_gethitinputs ( sCoopNetHitInputs* p )
{
	p->ttte = t.ttte; p->tdamage = t.tdamage; p->tdamageforce = t.tdamageforce; p->tdamagesource = t.tdamagesource;
	p->impacting = t.impacting; p->bulletraylimbhit = t.bulletraylimbhit; p->bulletraytype = t.bulletraytype;
	p->tdamagemultiplier_f = t.tdamagemultiplier_f;
	p->brayx1_f = t.brayx1_f; p->brayy1_f = t.brayy1_f; p->brayz1_f = t.brayz1_f;
	p->brayx2_f = t.brayx2_f; p->brayy2_f = t.brayy2_f; p->brayz2_f = t.brayz2_f;
}

static void coopnet_sethitinputs ( const sCoopNetHitInputs* p )
{
	t.ttte = p->ttte; t.tdamage = p->tdamage; t.tdamageforce = p->tdamageforce; t.tdamagesource = p->tdamagesource;
	t.impacting = p->impacting; t.bulletraylimbhit = p->bulletraylimbhit; t.bulletraytype = p->bulletraytype;
	t.tdamagemultiplier_f = p->tdamagemultiplier_f;
	t.brayx1_f = p->brayx1_f; t.brayy1_f = p->brayy1_f; t.brayz1_f = p->brayz1_f;
	t.brayx2_f = p->brayx2_f; t.brayy2_f = p->brayy2_f; t.brayz2_f = p->brayz2_f;
}

bool coopnet_forwardhit ( void )
{
	// client: damage to a zombie the host runs is the host's business. Apply nothing to our copy
	// (it would get its own health/death out of step) and send the hit as entity_applydamage saw it.
	if ( g_CoopNetMode != COOPNET_CLIENT || g_bCoopNetReplaying || !g_bCoopNetConnected || !g_bCoopNetHandshakeDone || !coopnet_levelsmatch() ) return false;
	int e = t.ttte;
	if ( !coopnet_issynccharacter ( e ) || e >= (int)g_CoopNetZombies.size() || g_CoopNetZombies[e].iCount == 0 ) return false;
	if ( g_CoopNetZombies[e].bDead ) return true;	// already dead on the host: nothing to do
	sCoopNetHitInputs h;
	coopnet_gethitinputs ( &h );
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_HIT );
	for ( size_t i = 0; i < sizeof(h); i++ ) w.put<unsigned char> ( ((const unsigned char*)&h)[i] );	// plain ints/floats, same build on both sides
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	coopnet_log ( "hit zombie e=%d for %d (x%.2f) - sent to host", e, h.tdamage, h.tdamagemultiplier_f );
	return true;
}

static void coopnet_applyhit ( const sCoopNetHitInputs& h )
{
	// host: the client's shot, applied exactly as if our own bullet had hit
	if ( !coopnet_issynccharacter ( h.ttte ) ) return;
	sCoopNetHitInputs store;
	coopnet_gethitinputs ( &store );
	coopnet_sethitinputs ( &h );
	g_bCoopNetReplaying = true;
	entity_applydamage();
	g_bCoopNetReplaying = false;
	coopnet_log ( "client hit zombie e=%d for %d, health now %d", h.ttte, h.tdamage, t.entityelement[h.ttte].health );
	coopnet_sethitinputs ( &store );
}

static void coopnet_receivehit ( sCoopNetReader& r )
{
	sCoopNetHitInputs h;
	r.getraw ( &h, sizeof(h) );
	if ( r.bOK ) coopnet_applyhit ( h );
}

void coopnet_forwardbloodimpact ( float fX, float fY, float fZ )
{
	// a blood splat at a bullet impact here: show it on the other machine too
	if ( g_bCoopNetReplaying || !g_bCoopNetConnected || !g_bCoopNetHandshakeDone || !coopnet_levelsmatch() ) return;
	if ( t.tttriggerdecalimpact != 2 ) return;	// 2 = blood (1 = impact, 10+ = material decal)
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_BLOODFX );
	w.put<float> ( fX ); w.put<float> ( fY ); w.put<float> ( fZ );
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
}

static void coopnet_applybloodimpact ( float fX, float fY, float fZ )
{
	int storetrigger = t.tttriggerdecalimpact;
	t.tttriggerdecalimpact = 2;
	g_bCoopNetReplaying = true;
	entity_triggerdecalatimpact ( fX, fY, fZ );
	g_bCoopNetReplaying = false;
	t.tttriggerdecalimpact = storetrigger;
}

void coopnet_forwardluacommand ( int iCmd, int e, float fValue )
{
	// any script destroying a placed item (a pickup collecting itself...) - both sides
	if ( iCmd == COOPNET_LUACMD_DESTROY && e > 0 && e < (int)t.entityelement.size()
		&& !( t.entityelement[e].eleprof.iscollectable == 2 && t.entityelement[e].eleprof.quantity > 0 ) )	// entity_lua_destroy refuses these
		coopnet_itemleftworld ( e );

	// host: only commands issued by a synced character's script (the client doesn't run those).
	// Commands on the character itself are skipped - its position/animation/visibility are
	// mirrored already - except sounds, which play from it.
	if ( g_iCoopNetLuaSyncRunning == 0 || !g_bCoopNetConnected ) return;
	bool bSound = iCmd >= COOPNET_LUACMD_SETSOUND && iCmd <= COOPNET_LUACMD_PLAYNON3DSOUND;
	if ( e == g_iCoopNetLuaSyncRunning && !bSound ) return;
	if ( e <= 0 || e > coopnet_levelentitylimit() ) return;	// only entities that exist on both sides
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_LUACMD );
	w.put<unsigned char> ( (unsigned char)iCmd );
	w.put<unsigned short> ( (unsigned short)e );
	w.put<float> ( fValue );
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
}

static void coopnet_applyluacommand ( int iCmd, int e, float fValue )
{
	// client: same entity_lua_* handler the Lua command dispatches to
	if ( e <= 0 || e > coopnet_levelentitylimit() || t.entityelement[e].bankindex <= 0 ) return;
	int storee = t.e, storev = t.v; float storevf = t.v_f;
	t.e = e; t.v = (int)fValue; t.v_f = fValue;
	switch ( iCmd )
	{
		case COOPNET_LUACMD_HIDE: entity_lua_hide(); break;
		case COOPNET_LUACMD_SHOW: entity_lua_show(); break;
		case COOPNET_LUACMD_DESTROY: entity_lua_destroy(); break;
		case COOPNET_LUACMD_SETSOUND: entity_lua_setsound(); break;
		case COOPNET_LUACMD_PLAYSOUND: entity_lua_playsound(); break;
		case COOPNET_LUACMD_PLAYSOUNDIFSILENT: entity_lua_playsoundifsilent(); break;
		case COOPNET_LUACMD_LOOPSOUND: entity_lua_loopsound(); break;
		case COOPNET_LUACMD_STOPSOUND: entity_lua_stopsound(); break;
		case COOPNET_LUACMD_PLAYNON3DSOUND: entity_lua_playnon3Dsound(); break;
		case COOPNET_LUACMD_SETPOSITIONX: entity_lua_setpositionx(); break;
		case COOPNET_LUACMD_SETPOSITIONY: entity_lua_setpositiony(); break;
		case COOPNET_LUACMD_SETPOSITIONZ: entity_lua_setpositionz(); break;
		case COOPNET_LUACMD_RESETPOSITIONX: entity_lua_resetpositionx(); break;
		case COOPNET_LUACMD_RESETPOSITIONY: entity_lua_resetpositiony(); break;
		case COOPNET_LUACMD_RESETPOSITIONZ: entity_lua_resetpositionz(); break;
		case COOPNET_LUACMD_SETROTATIONX: entity_lua_setrotationx(); break;
		case COOPNET_LUACMD_SETROTATIONY: entity_lua_setrotationy(); break;
		case COOPNET_LUACMD_SETROTATIONZ: entity_lua_setrotationz(); break;
		default: break;
	}
	t.e = storee; t.v = storev; t.v_f = storevf;
}

//----------------------------------------------------
// Items: a placed item picked up / destroyed by either player is gone for both (health,
// keys, quest items...). Guns and ammo stay per-player - each player collects their own.
//----------------------------------------------------

static bool coopnet_isshareditem ( int e )
{
	if ( e <= 0 || e > coopnet_levelentitylimit() ) return false;	// only entities that exist on both sides
	int entid = t.entityelement[e].bankindex;
	if ( entid <= 0 || t.entityprofile[entid].ischaracter == 1 ) return false;	// characters are synced separately
	if ( t.entityprofile[entid].isweapon > 0 || t.entityprofile[entid].isammo != 0 ) return false;
	return true;
}

void coopnet_itemleftworld ( int e )
{
	if ( !g_bCoopNetConnected || !g_bCoopNetHandshakeDone || g_bCoopNetReplaying || !coopnet_levelsmatch() ) return;
	if ( !g_CoopNetSettings.bSharePickups || !coopnet_isshareditem ( e ) ) return;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_ITEMGONE );
	w.put<unsigned short> ( (unsigned short)e );
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	coopnet_log ( "item e=%d left the world here - removing it for the partner", e );
}

static void coopnet_applyitemgone ( int e )
{
	// the partner took it: remove it the way Destroy() does. 'collected' is left alone - it
	// would hand this player the key/item too (darklua_refreshhaskeystatefor).
	if ( !coopnet_isshareditem ( e ) || t.entityelement[e].active == 0 ) return;
	int storee = t.e; t.e = e;
	g_bCoopNetReplaying = true;
	entity_lua_collisionoff();
	g_bCoopNetReplaying = false;
	t.e = storee;
	int iObj = t.entityelement[e].obj;
	if ( iObj > 0 && ObjectExist ( iObj ) == 1 ) HideObject ( iObj );
	t.entityelement[e].active = 0;
	coopnet_event ( "partner took item e=%d - removed here too", e );
}

//----------------------------------------------------
// Doors: each machine runs its own copy of every door script, which only reacts to its own
// player. When a door opens or closes here, the partner's copy of the same script is put
// into the same transition, so it plays its own sound/animation/rotation, collision, navmesh
// blocker, logic links and prompts. The stock scripts are untouched: this Lua is installed
// into the game's Lua state only while a co-op session is live (never in exports), and
// reaches each script's state table - a global, or a script-local via debug.getupvalue.
// Each adapter knows one script: phase() classifies its state, open()/close() perform the
// same state change the script makes when its own player presses E (false = not now).
//----------------------------------------------------

static const char* g_pCoopNetDoorLua = R"LUA(
local function upval ( fn, name )
	if type(fn) ~= 'function' or debug == nil then return nil end
	local i = 1
	while true do
		local n, v = debug.getupvalue ( fn, i )
		if n == nil then return nil end
		if n == name then return v end
		i = i + 1
	end
end
local function now () return g_Time or 0 end
local function logic ( e ) ActivateIfUsed ( e ) PerformLogicConnections ( e ) end
local function bymode ( d )
	-- door.lua / trapdoor.lua: 0 closed, 201 open trigger, 2 opening, 1 open, 202 close trigger, 3 closing
	local m = d['mode']
	if m == 0 then return 'closed' elseif m == 201 or m == 2 then return 'opening' elseif m == 1 then return 'open' elseif m == 202 or m == 3 then return 'closing' end
end
local function bystate ( s, closed, opening1, opening2, open, closing )
	if s == nil then return nil end
	if s == closed then return 'closed' elseif s == opening1 or s == opening2 then return 'opening' elseif s == open then return 'open' elseif s == closing then return 'closing' end
end
local K = {}
K.door = {
	get = function ( e ) local t = upval ( door_main, 'door' ) return t and t[e] end,
	phase = bymode,
	open = function ( e, d ) if d['mode'] == 0 then d['mode'] = 201 return true end end,
	close = function ( e, d ) if d['mode'] == 1 then d['mode'] = 202 return true end end,
}
K.trapdoor = {
	get = function ( e ) return g_trapdoor and g_trapdoor[e] end,
	phase = bymode,
	open = K.door.open,
	close = K.door.close,
}
K.door_rotate = {
	get = function ( e ) return g_door_rotate and g_door_rotate[e] end,
	phase = function ( d ) return bystate ( d.state, 'Closed', 'Knob', 'Opening', 'Open', 'Closing' ) end,
	open = function ( e, d )
		if d.state ~= 'Closed' or d.obj == nil then return end
		PlaySound ( e, 0 )
		d.state = 'Knob'
		d.timer = now() + 150
		if d.logic_trigger == 2 or d.logic_trigger == 4 or d.logic_trigger == 5 then logic ( e ) end
		return true
	end,
	close = function ( e, d )
		if d.state ~= 'Open' then return end
		d.state = 'Closing'
		if d.logic_trigger == 3 or d.logic_trigger == 4 then logic ( e ) end
		PlaySound ( e, 1 )
		return true
	end,
}
K.door_rotate_multi = {
	get = function ( e ) local t = upval ( door_rotate_multi_main, 'doors' ) return t and t[e] end,
	phase = function ( d ) return bystate ( d.state, 'closed', 'opening', nil, 'open', 'closing' ) end,
	open = function ( e, d ) if d.state == 'closed' then PlaySound ( e, 0 ) d.state = 'opening' return true end end,
	close = function ( e, d ) if d.state == 'open' then d.state = 'closing' PlaySound ( e, 1 ) return true end end,
}
K.door_sliding = {
	get = function ( e ) return g_door_sliding and g_door_sliding[e] end,
	phase = function ( d )
		if d.mode == 'delay' then return d.nextMode == 'opening' and 'opening' or 'closing' end
		return bystate ( d.mode, 'closed', 'opening', nil, 'open', 'closing' )
	end,
	open = function ( e, d )
		if d.mode ~= 'closed' then return end
		SetEntityActivated ( e, 1 )
		d.mode = 'delay'
		d.nextMode = 'opening'
		d.time = now() + ( d.move_delay or 0 )
		PlaySound ( e, 0 )
		if d.send_trigger == 1 or d.send_trigger == 3 then logic ( e ) end
		return true
	end,
	close = function ( e, d )
		if d.mode ~= 'open' then return end
		SetEntityActivated ( e, 1 )
		d.mode = 'delay'
		d.nextMode = 'closing'
		d.time = now() + ( d.move_delay or 0 )
		d.closeTime = now() + 1
		return true
	end,
}
K.oneway_door = {
	get = function ( e ) local t = upval ( oneway_door_main, 'doors' ) return t and t[e] end,
	phase = function ( d ) return bystate ( d.state, 'Closed', 'Knob', 'Opening', 'Open', 'Closing' ) end,
	open = function ( e, d )
		if d.state ~= 'Closed' then return end
		PlaySound ( e, 0 )
		PerformLogicConnections ( e )
		d.state = 'Knob'
		d.timer = now() + 500
		return true
	end,
	close = function ( e, d ) if d.state == 'Open' then d.state = 'Closing' return true end end,
}
K.trapdoor_rotate = {
	get = function ( e ) return g_trapdoor and g_trapdoor[e] end,
	phase = function ( d ) return bystate ( d.state, 'Closed', 'Trap', 'Opening', 'Open', 'Closing' ) end,
	open = function ( e, d )
		if d.state ~= 'Closed' or d.obj == nil then return end
		PlaySound ( e, 0 )
		PerformLogicConnections ( e )
		d.state = 'Trap'
		d.timer = now() + 500
		return true
	end,
	close = function ( e, d ) if d.state == 'Open' then d.state = 'Closing' PlaySound ( e, 1 ) return true end end,
}

coopnet_doorents = {}		-- e -> adapter name (filled by the engine from each entity's script)
coopnet_doorlast = {}		-- e -> phase last seen ('closed' if never seen)
coopnet_doorpending = {}	-- e -> { op, t }: partner's open/close that arrived mid-transition, applied when it ends
coopnet_doorout = ''

function coopnet_doorreg ( e, kind ) if K[kind] then coopnet_doorents[e] = kind end end

local function getdoor ( e )
	local kind = coopnet_doorents[e]
	if kind == nil then return nil end
	local k = K[kind]
	local ok, d = pcall ( k.get, e )
	if ok and type(d) == 'table' then return k, d end
end

function coopnet_doorpoll ()
	-- after this frame's scripts: report doors that started opening/closing here ("e:o;e:c")
	local out = {}
	for e, kind in pairs ( coopnet_doorents ) do
		local k, d = getdoor ( e )
		if k then
			local p = coopnet_doorpending[e]
			if p then
				if now() > p.t then coopnet_doorpending[e] = nil
				else
					local ok, r = pcall ( k[p.op], e, d )
					if ok and r then coopnet_doorpending[e] = nil coopnet_doorlast[e] = k.phase ( d ) end
				end
			end
			local ph = k.phase ( d )
			if ph then
				local last = coopnet_doorlast[e] or 'closed'
				local was = last == 'opening' or last == 'open'
				local is = ph == 'opening' or ph == 'open'
				if is and not was then out[#out + 1] = e .. ':o' elseif was and not is then out[#out + 1] = e .. ':c' end
				coopnet_doorlast[e] = ph
			end
		end
	end
	local s = table.concat ( out, ';' )
	if #s > 1000 then s = string.sub ( s, 1, 1000 ) end
	coopnet_doorout = s
end

function coopnet_doorapply ( e, op )
	-- the partner opened/closed door e: same transition here, without reporting it back
	local k, d = getdoor ( e )
	if k == nil then return end
	local ok, r = pcall ( k[op], e, d )
	if ok and r then
		coopnet_doorpending[e] = nil
		coopnet_doorlast[e] = k.phase ( d )
	else
		local ph = k.phase ( d )
		if ( op == 'open' and ph == 'closing' ) or ( op == 'close' and ph == 'opening' ) then
			coopnet_doorpending[e] = { op = op, t = now() + 10000 }
		end
	end
end

coopnet_doorsready = 1
)LUA";

static const char* g_pCoopNetDoorScripts[] = { "door", "door_rotate", "door_rotate_multi", "door_sliding", "oneway_door", "trapdoor", "trapdoor_rotate" };
static bool g_bCoopNetDoorsLive = false;
static unsigned int g_uCoopNetDoorsLevel = 0;
static int g_iCoopNetDoorCount = 0;
static char g_pCoopNetDoorLastError[320] = "";

static const char* coopnet_doorkind ( int e )
{
	// adapter name for entity e's script ("objects\door_rotate.lua" -> "door_rotate"), or NULL
	const char* pFull = t.entityelement[e].eleprof.aimain_s.Get();
	if ( pFull == NULL || pFull[0] == 0 ) return NULL;
	const char* pBase = pFull;
	for ( const char* p = pFull; *p; p++ ) if ( *p == '\\' || *p == '/' ) pBase = p + 1;
	char pName[64];
	int n = 0;
	for ( const char* p = pBase; *p && *p != '.' && n < (int)sizeof(pName) - 1; p++ ) pName[n++] = (char)tolower ( (unsigned char)*p );
	pName[n] = 0;
	for ( int i = 0; i < (int)( sizeof(g_pCoopNetDoorScripts) / sizeof(g_pCoopNetDoorScripts[0]) ); i++ )
		if ( strcmp ( pName, g_pCoopNetDoorScripts[i] ) == 0 ) return g_pCoopNetDoorScripts[i];
	return NULL;
}

static void coopnet_doors_checkerror ( void )
{
	char pErr[1024] = "";
	LuaGetString ( "coopnet_doorerr", pErr );
	if ( pErr[0] == 0 ) return;
	LuaExecute ( "coopnet_doorerr = nil" );
	if ( strncmp ( pErr, g_pCoopNetDoorLastError, sizeof(g_pCoopNetDoorLastError) - 1 ) == 0 ) return;
	strncpy ( g_pCoopNetDoorLastError, pErr, sizeof(g_pCoopNetDoorLastError) - 1 );
	coopnet_log ( "door sync Lua error: %s", pErr );
}

static void coopnet_doors_install ( void )
{
	// (re)load the adapters - also resets what was last seen, so open doors are re-sent
	LuaSetString ( "coopnet_doorsrc", (LPSTR)g_pCoopNetDoorLua );
	LuaExecute ( "local ld = loadstring or load "
		"local f, err = ld ( coopnet_doorsrc, 'coopnet_doors' ) coopnet_doorsrc = nil "
		"if f then local ok, e2 = pcall ( f ) if not ok then coopnet_doorerr = string.sub ( tostring ( e2 ), 1, 300 ) end "
		"else coopnet_doorerr = string.sub ( tostring ( err ), 1, 300 ) end" );
	coopnet_doors_checkerror();

	std::string sReg;
	char pLine[96];
	g_iCoopNetDoorCount = 0;
	for ( int e = 1; e <= coopnet_levelentitylimit(); e++ )
	{
		if ( t.entityelement[e].bankindex <= 0 ) continue;
		const char* pKind = coopnet_doorkind ( e );
		if ( pKind == NULL ) continue;
		sprintf ( pLine, "coopnet_doorreg(%d,'%s') ", e, pKind );
		sReg += pLine;
		coopnet_log ( "door e=%d '%s' uses %s", e, t.entityelement[e].eleprof.name_s.Get(), pKind );
		g_iCoopNetDoorCount++;
	}
	if ( !sReg.empty() ) LuaExecute ( (LPSTR)sReg.c_str() );
	g_uCoopNetDoorsLevel = g_CoopNetLocalLevel.uFingerprint;
	coopnet_log ( "door sync: %d doors (adapters %s)", g_iCoopNetDoorCount, LuaGetInt ( "coopnet_doorsready" ) == 1 ? "loaded" : "FAILED" );
}

static void coopnet_doors_update ( void )
{
	bool bLive = g_bCoopNetConnected && g_bCoopNetHandshakeDone && coopnet_levelsmatch() && g_CoopNetSettings.bSyncDoors;
	if ( !bLive ) { g_bCoopNetDoorsLive = false; return; }
	// a new Lua state (level load), another level, or a new session: install again
	if ( !g_bCoopNetDoorsLive || g_uCoopNetDoorsLevel != g_CoopNetLocalLevel.uFingerprint || LuaGetInt ( "coopnet_doorsready" ) != 1 )
	{
		g_bCoopNetDoorsLive = true;
		coopnet_doors_install();
	}
	if ( g_iCoopNetDoorCount == 0 ) return;

	LuaExecute ( "if coopnet_doorpoll then local ok, err = pcall ( coopnet_doorpoll ) "
		"if not ok then coopnet_doorout = '' coopnet_doorerr = string.sub ( tostring ( err ), 1, 300 ) end end" );
	coopnet_doors_checkerror();
	char pOut[1100] = "";
	LuaGetString ( "coopnet_doorout", pOut );
	if ( pOut[0] == 0 ) return;
	for ( char* p = pOut; *p; )
	{
		int e = atoi ( p );
		char* pColon = strchr ( p, ':' );
		if ( pColon == NULL || pColon[1] == 0 ) break;
		int iOp = pColon[1] == 'o' ? 1 : 2;
		if ( e > 0 && e <= coopnet_levelentitylimit() )
		{
			sCoopNetWriter w;
			w.put<unsigned char> ( COOPNET_MSG_DOOR );
			w.put<unsigned short> ( (unsigned short)e );
			w.put<unsigned char> ( (unsigned char)iOp );
			coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
			coopnet_log ( "door e=%d %s here - sent to partner", e, iOp == 1 ? "opened" : "closed" );
		}
		char* pNext = strchr ( pColon, ';' );
		if ( pNext == NULL ) break;
		p = pNext + 1;
	}
}

static void coopnet_applydoor ( int e, int iOp )
{
	if ( e <= 0 || e > coopnet_levelentitylimit() || ( iOp != 1 && iOp != 2 ) ) return;
	if ( !g_bCoopNetDoorsLive ) coopnet_doors_update();	// first frame of the session: adapters not loaded yet
	char pCmd[96];
	sprintf ( pCmd, "if coopnet_doorapply then coopnet_doorapply(%d,'%s') end", e, iOp == 1 ? "open" : "close" );
	LuaExecute ( pCmd );
	coopnet_doors_checkerror();
	coopnet_event ( "partner %s door e=%d", iOp == 1 ? "opened" : "closed", e );
}

static void coopnet_takedamage ( int e, int iDamage )
{
	// client: same path as the Lua HurtPlayer command (entity_lua_hurtplayer), so blood,
	// shake, regen delay and death all behave as normal; e is the same zombie on this side
	int storetdamage = t.tdamage, storete = t.te, storedrown = t.tDrownDamageFlag;
	t.tdamage = iDamage; t.te = e; t.tDrownDamageFlag = 0;
	physics_player_takedamage();
	t.tdamage = storetdamage; t.te = storete; t.tDrownDamageFlag = storedrown;
	coopnet_log ( "hurt by host zombie e=%d for %d (health now %d)", e, iDamage, t.player[t.plrid].health );
}

static void coopnet_updatetargets ( DWORD dwNow )
{
	// host only, with at least one client in play on our level
	bool bAny = false;
	for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ )
	{
		bool bLive = g_CoopNetMode == COOPNET_HOST && coopnet_playerpresent ( id ) && coopnet_levelmatchfor ( id );
		if ( bLive )
		{
			// where the host sees the client (its puppet), else the newest received position
			const sCoopNetPlayerSlot& p = g_CoopNetPlayers[id];
			const sCoopNetPlayerState& rs = p.puppet.bVisible ? p.puppet.shown : p.remote.state;
			g_fCoopNetTargetX[id] = rs.x; g_fCoopNetTargetY[id] = rs.y; g_fCoopNetTargetZ[id] = rs.z;
			g_iCoopNetTargetHealth[id] = p.remote.state.health;
			bAny = true;
		}
		g_bCoopNetTargetLive[id] = bLive;
	}
	if ( !bAny || !g_bCoopNetLocalValid )
	{
		if ( g_bCoopNetTargetingActive ) { g_bCoopNetTargetingActive = false; memset ( g_CoopNetTarget, 0, sizeof(g_CoopNetTarget) ); }
		g_iCoopNetChasingRemote = 0;
		return;
	}
	g_bCoopNetTargetingActive = true;
	if ( (int)( dwNow - g_dwCoopNetNextTargetEval ) < 0 ) return;
	g_dwCoopNetNextTargetEval = dwNow + COOPNET_TARGET_EVAL_MS;

	// candidates: us (player 0) and every live client
	float fX[COOPNET_MAX_PLAYERS], fZ[COOPNET_MAX_PLAYERS];
	bool bAlive[COOPNET_MAX_PLAYERS];
	fX[0] = g_CoopNetLocal.x; fZ[0] = g_CoopNetLocal.z;
	bAlive[0] = t.player[t.plrid].health > 0;
	for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ )
	{
		fX[id] = g_fCoopNetTargetX[id]; fZ[id] = g_fCoopNetTargetZ[id];
		bAlive[id] = g_bCoopNetTargetLive[id] && g_iCoopNetTargetHealth[id] > 0;
	}
	int iChasing = 0;
	int iLimit = g.entityelementlist < COOPNET_TARGET_MAX_ENTITY ? g.entityelementlist : COOPNET_TARGET_MAX_ENTITY - 1;
	for ( int e = 1; e <= iLimit; e++ )
	{
		int entid = t.entityelement[e].bankindex;
		int iObj = t.entityelement[e].obj;
		// characters only: pickups/zones also read plrdist and must stay with the host player
		if ( entid <= 0 || t.entityprofile[entid].ischaracter != 1 || iObj <= 0 || coopnet_isclaimedentity ( e ) || ObjectExist ( iObj ) == 0 )
		{
			g_CoopNetTarget[e] = 0;
			continue;
		}
		// nearest living player; the current target is kept unless someone is clearly nearer
		float ex = ObjectPositionX ( iObj ), ez = ObjectPositionZ ( iObj );
		float fDist[COOPNET_MAX_PLAYERS];
		int iNearest = -1;
		for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		{
			float dx = fX[id] - ex, dz = fZ[id] - ez;
			fDist[id] = dx * dx + dz * dz;	// squared
			if ( bAlive[id] && ( iNearest < 0 || fDist[id] < fDist[iNearest] ) ) iNearest = id;
		}
		int iCurrent = g_CoopNetTarget[e];
		if ( iCurrent >= COOPNET_MAX_PLAYERS || !bAlive[iCurrent] ) iCurrent = -1;
		int iWant = iCurrent;
		if ( iNearest < 0 ) iWant = 0;			// everyone dead: back to the host player
		else if ( iCurrent < 0 ) iWant = iNearest;
		else if ( iNearest != iCurrent && fDist[iNearest] < fDist[iCurrent] * COOPNET_TARGET_SWITCH * COOPNET_TARGET_SWITCH ) iWant = iNearest;
		g_CoopNetTarget[e] = (unsigned char)iWant;
		if ( iWant != 0 ) iChasing++;
	}
	g_iCoopNetChasingRemote = iChasing;
}

//
// Co-op player death: 10 s respawn delay, spectate a living teammate while dead, unlimited
// lives in co-op (the out-of-lives path is a fallback: spectate until every teammate is out
// too), game over only when everyone is dead at once, shared checkpoints. The engine's own
// death/respawn (physics_player_takedamage/handledeath) still does the work; this only
// stretches its timer, borrows the camera and defers game over.
//

#define COOPNET_RESPAWN_DELAY_MS	10000
#define COOPNET_ENGINE_DEATH_MS		2000	// physics_player_takedamage: deadtime = Timer() + 2000
#define COOPNET_SPECTATE_AFTER_MS	1500	// let the engine's death tilt play first

static int g_iCoopNetLastDeadtime = 0;
static int g_iCoopNetDeathStart = 0;		// Timer() when we died
static int g_iCoopNetLivesWhileAlive = -1;	// lives before dying (co-op deaths don't cost a life)
static bool g_bCoopNetGameOver = false;

static bool coopnet_teammate ( int id )
{
	// a remote player in play on our level (a client only knows the host's level: the host
	// keeps everyone it relays on the same game)
	if ( !coopnet_playerpresent ( id ) ) return false;
	return g_CoopNetMode == COOPNET_HOST ? coopnet_levelmatchfor ( id ) : coopnet_levelsmatch();
}

static bool coopnet_partnerpresent ( void )
{
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ ) if ( coopnet_teammate ( id ) ) return true;
	return false;
}

void coopnet_sharecheckpoint ( float fX, float fY, float fZ, float fA )
{
	if ( !coopnet_partnerpresent() || !g_CoopNetSettings.bShareCheckpoints ) return;
	sCoopNetWriter w;
	w.put<unsigned char> ( COOPNET_MSG_CHECKPOINT );
	w.put<float> ( fX ); w.put<float> ( fY ); w.put<float> ( fZ ); w.put<float> ( fA );
	coopnet_send ( w, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	coopnet_log ( "reached a checkpoint - shared with the team" );
}

bool coopnet_outoflives ( void )
{
	// every teammate gone or out of lives as well (or spectating switched off): game over as normal
	if ( !g_CoopNetSettings.bSpectateOutOfLives ) return false;
	bool bSomeoneLeft = false;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		if ( coopnet_teammate ( id ) && !( g_CoopNetPlayers[id].remote.state.flags & COOPNET_PLAYERFLAG_OUTOFLIVES ) ) bSomeoneLeft = true;
	if ( !bSomeoneLeft ) return false;
	if ( !g_bCoopNetOutOfLives )
	{
		g_bCoopNetOutOfLives = true;
		coopnet_event ( "out of lives - spectating until the rest of the team is out too" );
	}
	return true;
}

//
// Level exits: a win zone (or any script) ending the level takes the whole team to the next one.
// game_jump_to_level_from_lua / game_finish_level_from_lua (M-Game.cpp) ask coopnet_levelexit
// first. A client sends its exit to the host; the host decides when the team goes - as soon as one
// player reaches an exit, or once every player in play has (Co-op Settings) - and sends LEVELGO.
// Every PC then runs the same jump, so the storyboard picks the same next level everywhere.
// Reaching an exit counts for the rest of the level (exits like "press E to continue" only report
// once). Joining players, and anyone who ended up elsewhere, go to the host's level (LEVEL path).
//

static bool g_bCoopNetExitAllowed = false;		// running a team exit: let the engine's level jump through
static bool g_bCoopNetLocalAtExit = false;		// we reached an exit on this level
static sCoopNetExitRequest g_CoopNetLocalExit;	// ... that exit (client: resent until the team goes)
static DWORD g_dwCoopNetExitResend = 0;
static sCoopNetExitRequest g_CoopNetExitFirst;	// host: the first exit reached - where the team goes
static bool g_bCoopNetExitFirstValid = false;
static bool g_bCoopNetExitGone = false;			// host: LEVELGO sent (until the level changes)
static DWORD g_dwCoopNetNextFollow = 0;			// client: next try to go to the host's level
#define COOPNET_EXIT_RESEND_MS		2000
#define COOPNET_FOLLOW_RETRY_MS		15000

static void coopnet_sendexit ( int iTo, unsigned char msg, const sCoopNetExitRequest& q )
{
	sCoopNetWriter w;
	w.put<unsigned char> ( msg );
	w.put<unsigned char> ( q.bFinish ? 1 : 0 );
	w.put<unsigned char> ( (unsigned char)q.iResetStates );
	coopnet_putstring ( w, q.pTarget );
	coopnet_putstring ( w, q.pAdvance );
	coopnet_sendto ( iTo, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
}

static void coopnet_exitgo ( const sCoopNetExitRequest& q )
{
	// leave the level the way the exit said (on every PC; the host after sending LEVELGO)
	coopnet_event ( "level exit: going to %s", q.bFinish ? "the next level (storyboard)" : q.pTarget );
	g_bCoopNetExitAllowed = true;
	if ( q.bFinish )
		game_finish_level_from_lua();
	else
	{
		t.tleveltojump_s = q.pTarget;
		strncpy ( t.game.pAdvanceWarningOfLevelFilename, q.pAdvance, sizeof(t.game.pAdvanceWarningOfLevelFilename) - 1 );
		t.game.pAdvanceWarningOfLevelFilename[sizeof(t.game.pAdvanceWarningOfLevelFilename) - 1] = 0;
		game_jump_to_level_from_lua ( q.iResetStates );
	}
	g_bCoopNetExitAllowed = false;
}

static bool coopnet_exitcounts ( int id )
{
	// a player the team waits for: in play on our level and not out of lives (the dead respawn)
	if ( id == g_iCoopNetLocalID ) return !g_bCoopNetOutOfLives;
	return coopnet_teammate ( id ) && !( g_CoopNetPlayers[id].remote.state.flags & COOPNET_PLAYERFLAG_OUTOFLIVES );
}

static void coopnet_exitcheck ( void )
{
	// host: everyone needed at an exit (or anyone, if the first one takes everyone)? then go
	if ( g_CoopNetMode != COOPNET_HOST || g_bCoopNetExitGone ) return;
	int iAt = 0, iNeeded = 0;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		if ( !coopnet_exitcounts ( id ) ) continue;
		iNeeded++;
		if ( id == g_iCoopNetLocalID ? g_bCoopNetLocalAtExit : g_CoopNetPlayers[id].bAtExit ) iAt++;
	}
	if ( !g_bCoopNetExitFirstValid ) iAt = 0;
	bool bGo = iAt > 0 && ( !g_CoopNetSettings.bExitNeedsAll || iAt >= iNeeded );
	if ( bGo ) iAt = iNeeded = 0;	// nobody waiting any more
	if ( iAt == 0 ) iNeeded = 0;
	if ( iAt != g_iCoopNetExitAt || iNeeded != g_iCoopNetExitNeeded )
	{
		g_iCoopNetExitAt = iAt;
		g_iCoopNetExitNeeded = iNeeded;
		if ( iAt > 0 ) coopnet_event ( "%d of %d players at the level exit", iAt, iNeeded );
		sCoopNetWriter w;
		w.put<unsigned char> ( COOPNET_MSG_EXITSTATUS );
		w.put<unsigned char> ( (unsigned char)iAt );
		w.put<unsigned char> ( (unsigned char)iNeeded );
		coopnet_sendto ( -1, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE );
	}
	if ( !bGo ) return;
	g_bCoopNetExitGone = true;
	// only players on our level go the same way (anyone else follows our level once it's loaded)
	for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ )
		if ( coopnet_teammate ( id ) ) coopnet_sendexit ( id, COOPNET_MSG_LEVELGO, g_CoopNetExitFirst );
	if ( g_pCoopNetHost ) enet_host_flush ( g_pCoopNetHost );
	coopnet_exitgo ( g_CoopNetExitFirst );
}

static void coopnet_exitrequest ( int id, const sCoopNetExitRequest& q )
{
	// host: player id reached a level exit (us included)
	if ( g_bCoopNetExitGone ) return;
	if ( id != g_iCoopNetLocalID && !coopnet_teammate ( id ) ) return;	// from another level (still loading...)
	bool& bAt = id == g_iCoopNetLocalID ? g_bCoopNetLocalAtExit : g_CoopNetPlayers[id].bAtExit;
	if ( !bAt ) coopnet_event ( id == g_iCoopNetLocalID ? "reached the level exit" : "player %d reached the level exit", id + 1 );
	bAt = true;
	if ( !g_bCoopNetExitFirstValid ) { g_CoopNetExitFirst = q; g_bCoopNetExitFirstValid = true; }
	coopnet_exitcheck();
}

bool coopnet_levelexit ( bool bFinish, int iResetStates )
{
	// true = co-op takes care of this level exit (the engine must not leave the level now)
	if ( g_bCoopNetExitAllowed ) return false;
	if ( g_CoopNetMode != COOPNET_HOST && g_CoopNetMode != COOPNET_CLIENT ) return false;
	if ( t.game.gameisexe != 1 || t.game.gameloop != 1 || t.game.levelendingcycle != 0 ) return false;	// the engine ignores it anyway
	if ( !coopnet_partnerpresent() ) return false;	// nobody else on this level: as in single player
	sCoopNetExitRequest q;
	q.bFinish = bFinish;
	q.iResetStates = iResetStates;
	q.pTarget[0] = q.pAdvance[0] = 0;
	if ( !bFinish )
	{
		strncpy ( q.pTarget, t.tleveltojump_s.Get(), sizeof(q.pTarget) - 1 ); q.pTarget[sizeof(q.pTarget) - 1] = 0;
		strncpy ( q.pAdvance, t.game.pAdvanceWarningOfLevelFilename, sizeof(q.pAdvance) - 1 ); q.pAdvance[sizeof(q.pAdvance) - 1] = 0;
		t.game.pAdvanceWarningOfLevelFilename[0] = 0;	// set again when the team goes
	}
	if ( g_CoopNetMode == COOPNET_HOST )
	{
		coopnet_exitrequest ( g_iCoopNetLocalID, q );	// when the team goes now, this ran the jump already
		return true;
	}
	if ( !g_bCoopNetLocalAtExit )
	{
		g_bCoopNetLocalAtExit = true;
		g_CoopNetLocalExit = q;
		g_dwCoopNetExitResend = timeGetTime() + COOPNET_EXIT_RESEND_MS;
		coopnet_event ( "reached the level exit - told the host" );
		coopnet_sendexit ( 0, COOPNET_MSG_LEVELEXIT, q );
	}
	return true;
}

static void coopnet_exitupdate ( DWORD dwNow )
{
	if ( g_CoopNetMode == COOPNET_HOST ) { coopnet_exitcheck(); return; }
	if ( g_CoopNetMode != COOPNET_CLIENT || !g_CoopNetPlayers[0].bHandshakeDone ) return;
	// our exit again now and then (the host ignores it while we're on different levels)
	if ( g_bCoopNetLocalAtExit && (int)( dwNow - g_dwCoopNetExitResend ) >= 0 )
	{
		g_dwCoopNetExitResend = dwNow + COOPNET_EXIT_RESEND_MS;
		coopnet_sendexit ( 0, COOPNET_MSG_LEVELEXIT, g_CoopNetLocalExit );
	}
	// on another level than the host (just joined, or the storyboard sent us elsewhere): go there
	const sCoopNetLevelInfo& h = g_CoopNetRemoteLevels[0];
	if ( !h.bValid || h.pPath[0] == 0 || !g_CoopNetLocalLevel.bValid || _stricmp ( h.pPath, g_CoopNetLocalLevel.pPath ) == 0 ) return;
	if ( t.game.gameisexe != 1 || t.game.gameloop != 1 || t.game.levelendingcycle != 0 ) return;
	if ( (int)( dwNow - g_dwCoopNetNextFollow ) < 0 ) return;
	g_dwCoopNetNextFollow = dwNow + COOPNET_FOLLOW_RETRY_MS;
	char pFile[MAX_PATH];
	snprintf ( pFile, MAX_PATH, "%s\\Files\\%s", g.fpscrootdir_s.Get(), h.pPath );
	if ( FileExist ( pFile ) != 1 && !coopnet_gamefileexists ( h.pPath ) )
	{
		coopnet_event ( "the host is on level '%s', which this copy of the game doesn't have", h.pName );
		return;
	}
	coopnet_event ( "going to the host's level '%s'", h.pName );
	sCoopNetExitRequest q;
	q.bFinish = false;
	q.iResetStates = 0;
	strncpy ( q.pTarget, h.pPath, sizeof(q.pTarget) - 1 ); q.pTarget[sizeof(q.pTarget) - 1] = 0;
	strcpy ( q.pAdvance, q.pTarget );	// a specific level, not the storyboard's next one
	coopnet_exitgo ( q );
}

static void coopnet_newlevel ( void )
{
	// our level changed: nobody is at this level's exit yet, and nobody is dead or spectating
	g_bCoopNetLocalAtExit = false;
	g_bCoopNetExitFirstValid = false;
	g_bCoopNetExitGone = false;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ ) g_CoopNetPlayers[id].bAtExit = false;
	g_iCoopNetExitAt = g_iCoopNetExitNeeded = 0;
	g_dwCoopNetNextFollow = 0;
	if ( g_bCoopNetSpectating ) { g.luacameraoverride = 0; g_bCoopNetSpectating = false; }
	g_iCoopNetSpectateID = -1;
	g_bCoopNetOutOfLives = false;
	g_iCoopNetLastDeadtime = 0;
	g_iCoopNetLivesWhileAlive = -1;
	g_bCoopNetGameOver = false;
}

static void coopnet_drawexittext ( void )
{
	// all players must reach the exit: show who is waiting
	if ( g_iCoopNetExitAt <= 0 || g_iCoopNetExitNeeded <= 0 ) return;
	char pLine[128];
	if ( g_bCoopNetLocalAtExit )
		sprintf ( pLine, "At the exit - waiting for your team (%d/%d)", g_iCoopNetExitAt, g_iCoopNetExitNeeded );
	else
		sprintf ( pLine, "%d/%d players are waiting at the exit", g_iCoopNetExitAt, g_iCoopNetExitNeeded );
	int iFont = 1;
	pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, GetDisplayHeight() * 3 / 4, iFont, 255 );
}

void coopnet_loadingupdate ( void )
{
	// a level loading, or a storyboard screen between levels: the game loop (and coopnet_update) is
	// stopped. Keep the connections alive; packets wait until the level runs (g_CoopNetHeldPackets).
	if ( g_CoopNetMode != COOPNET_HOST && g_CoopNetMode != COOPNET_CLIENT ) return;
	static DWORD s_dwLast = 0;
	DWORD dwNow = timeGetTime();
	if ( g_bCoopNetLoading && dwNow - s_dwLast < 50 ) return;
	s_dwLast = dwNow;
	if ( !g_bCoopNetLoading )
	{
		g_bCoopNetLoading = true;
		coopnet_log ( "loading - game packets held until the level runs" );
		sCoopNetWriter w;
		w.put<unsigned char> ( COOPNET_MSG_LOADING );
		coopnet_sendto ( -1, w.buf, w.len, COOPNET_CHANNEL_EVENTS, ENET_PACKET_FLAG_RELIABLE, -1, false );
		for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
			if ( coopnet_isremoteplayer ( id ) && g_CoopNetPlayers[id].bConnected ) coopnet_settimeoutlong ( id, true );
	}
	coopnet_service();
	if ( g_pCoopNetHost ) enet_host_flush ( g_pCoopNetHost );
}

static void coopnet_loadingdone ( DWORD dwNow )
{
	// the game loop runs again: our new level first (so old-level events no longer match it),
	// then everything that arrived meanwhile, in order
	if ( !g_bCoopNetLoading ) return;
	g_bCoopNetLoading = false;
	g_dwCoopNetNextLevelCheck = 0;
	coopnet_updatelevel ( dwNow );
	coopnet_log ( "loaded - handling %d held packets", (int)g_CoopNetHeldPackets.size() );
	std::vector<sCoopNetHeldPacket> held;
	held.swap ( g_CoopNetHeldPackets );
	g_bCoopNetReplayingHeld = true;
	for ( size_t i = 0; i < held.size(); i++ )
		if ( g_CoopNetPlayers[held[i].iFrom].bConnected && !held[i].data.empty() ) coopnet_receive ( &held[i].data[0], held[i].data.size(), held[i].iFrom );
	g_bCoopNetReplayingHeld = false;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		if ( coopnet_isremoteplayer ( id ) && g_CoopNetPlayers[id].bConnected ) coopnet_settimeoutlong ( id, g_CoopNetPlayers[id].bLoading );
}

static int coopnet_pickspectate ( void )
{
	// a living teammate to watch: keep the current one while alive, else the nearest
	int iCur = g_iCoopNetSpectateID;
	if ( iCur >= 0 && coopnet_teammate ( iCur ) && g_CoopNetPlayers[iCur].remote.state.health > 0 && g_CoopNetPlayers[iCur].iSnapCount > 0 ) return iCur;
	int iBest = -1;
	float fBest = 0;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		const sCoopNetPlayerSlot& p = g_CoopNetPlayers[id];
		if ( !coopnet_teammate ( id ) || p.remote.state.health <= 0 || p.iSnapCount == 0 ) continue;
		float dx = p.remote.state.x - g_CoopNetLocal.x, dz = p.remote.state.z - g_CoopNetLocal.z;
		float d = dx * dx + dz * dz;
		if ( iBest < 0 || d < fBest ) { iBest = id; fBest = d; }
	}
	return iBest;
}

static void coopnet_updatedeath ( void )
{
	int iNow = Timer();
	int iDeadtime = t.playercontrol.deadtime;
	bool bCoop = coopnet_partnerpresent();
	if ( iDeadtime == 0 && t.player[t.plrid].health > 0 ) g_iCoopNetLivesWhileAlive = t.player[t.plrid].lives;

	// death just started: the engine waits 2 s before respawning, co-op waits 10 s
	if ( iDeadtime > 0 && g_iCoopNetLastDeadtime == 0 )
	{
		g_iCoopNetDeathStart = iNow;
		if ( bCoop )
		{
			// unlimited lives in co-op: give back the life physics_player_takedamage just took, so
			// physics_player_handledeath always respawns (single player keeps the level's lives)
			if ( t.playercontrol.startlives > 0 && g_iCoopNetLivesWhileAlive >= 0 ) t.player[t.plrid].lives = g_iCoopNetLivesWhileAlive;
			t.playercontrol.deadtime += COOPNET_RESPAWN_DELAY_MS - COOPNET_ENGINE_DEATH_MS;
			iDeadtime = t.playercontrol.deadtime;
			coopnet_log ( "died - respawn in %d s", COOPNET_RESPAWN_DELAY_MS / 1000 );
		}
	}
	g_iCoopNetLastDeadtime = iDeadtime;

	// everyone dead at the same time: game over for all (each machine sees the same state)
	bool bAllDead = bCoop && iDeadtime > 0;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS && bAllDead; id++ )
		if ( coopnet_teammate ( id ) && g_CoopNetPlayers[id].remote.state.health > 0 ) bAllDead = false;
	if ( bAllDead && t.game.gameisexe == 1 && !g_bCoopNetGameOver )
	{
		g_bCoopNetGameOver = true;
		coopnet_event ( "the whole team is dead - game over" );
		t.game.gameloop = 0; t.game.lostthegame = 1;
	}

	// spectate a living teammate after the death tilt, until just before respawning (or for
	// good when out of lives); the engine's death view if nobody is alive to watch
	int iWatch = iDeadtime > 0 && bCoop ? coopnet_pickspectate() : -1;
	bool bWant = iWatch >= 0 && ( iNow - g_iCoopNetDeathStart ) > COOPNET_SPECTATE_AFTER_MS
		&& ( g_bCoopNetOutOfLives || iNow < iDeadtime - 200 );
	g_iCoopNetSpectateID = bWant ? iWatch : -1;
	if ( bWant != g_bCoopNetSpectating )
	{
		// override 3 = camera fully script-controlled (nothing else positions or tilts it);
		// the engine set it to 0 at death, and respawn expects it back at 0
		g.luacameraoverride = bWant ? 3 : 0;
		g_bCoopNetSpectating = bWant;
	}
	if ( g_bCoopNetSpectating )
	{
		// from the teammate's eyes, at the same (smoothed) moment as their puppet
		const sCoopNetPlayerState& s = g_CoopNetPlayers[g_iCoopNetSpectateID].puppet.shown;
		PositionCamera ( 0, s.x, s.y + COOPNET_EYE_ABOVE_CAPSULE, s.z );
		RotateCamera ( 0, s.pitch, s.yaw, 0 );
		// gameplayerhealth.lua fades the screen to full red/black while dead (respawn clears it);
		// undo it after physics_player_control applied it this frame, so the partner is visible
		t.playercontrol.redDeathFog_f = 0;
		SetVector4 ( g.terrainvectorindex, 0, 0, 0, 0 );
		t.tColorVector = g.terrainvectorindex;
		postprocess_setscreencolor();
	}
}

static void coopnet_drawdeathtext ( void )
{
	// centre-screen co-op death status
	char pLine[128] = "";
	int iDeadtime = t.playercontrol.deadtime;
	bool bTeamOut = coopnet_partnerpresent();
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		if ( coopnet_teammate ( id ) && !( g_CoopNetPlayers[id].remote.state.flags & COOPNET_PLAYERFLAG_OUTOFLIVES ) ) bTeamOut = false;
	if ( iDeadtime > 0 && g_bCoopNetOutOfLives )
		strcpy ( pLine, "OUT OF LIVES - spectating your team" );
	else if ( iDeadtime > 0 && coopnet_partnerpresent() )
	{
		int iSeconds = ( iDeadtime - Timer() ) / 1000 + 1;
		if ( iSeconds < 0 ) iSeconds = 0;
		if ( g_bCoopNetSpectating )
			sprintf ( pLine, "YOU DIED - respawning in %d (watching player %d)", iSeconds, g_iCoopNetSpectateID + 1 );
		else
			sprintf ( pLine, "YOU DIED - respawning in %d", iSeconds );
	}
	else if ( bTeamOut )
		strcpy ( pLine, "The rest of your team is out of lives" );
	if ( pLine[0] == 0 ) return;
	int iFont = 1;
	int iX = ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2;
	pastebitmapfont ( pLine, iX, GetDisplayHeight() / 3, iFont, 255 );
}

static bool coopnet_keypressed ( int iVirtualKey )
{
	// edge-triggered, only while our window has focus
	static bool s_bDown[256] = { false };
	bool bDown = GetForegroundWindow() == g_pGlob->hWnd && ( GetAsyncKeyState ( iVirtualKey ) & 0x8000 ) != 0;
	bool bPressed = bDown && !s_bDown[iVirtualKey & 255];
	s_bDown[iVirtualKey & 255] = bDown;
	return bPressed;
}

static void coopnet_updatesteamui ( void )
{
	// Steam co-op: what's happening (top centre) until someone is in, and for a few seconds
	// whenever the team changes; key: client 1-9 = pick which hosting friend to join (when started
	// without a lobby from the launcher's friend list)
	if ( !g_bCoopNetSteam ) return;
	char pLine[200];
	int iFont = 1;
	int iY = GetDisplayHeight() / 8;
	int iLineHeight = getbitmapfontheight ( iFont ) + 6;
	DWORD dwNow = timeGetTime();

	int iPlayers = 1;
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		if ( g_CoopNetMode == COOPNET_HOST ? ( coopnet_isremoteplayer ( id ) && g_CoopNetPlayers[id].bHandshakeDone ) : coopnet_playerpresent ( id ) ) iPlayers++;
	static int s_iLastPlayers = 1;
	static DWORD s_dwChangedAt = 0;
	if ( iPlayers != s_iLastPlayers ) { s_iLastPlayers = iPlayers; s_dwChangedAt = dwNow; }
	if ( g_bCoopNetHandshakeDone )
	{
		if ( dwNow - s_dwChangedAt > 5000 ) return;
		if ( g_CoopNetMode == COOPNET_HOST )
		{
			if ( iPlayers < COOPNET_MAX_PLAYERS )
				sprintf ( pLine, "Playing co-op (%d/%d players) - friends can join from their Join co-op list", iPlayers, COOPNET_MAX_PLAYERS );
			else
				sprintf ( pLine, "Playing co-op - the game is full (%d/%d players)", iPlayers, COOPNET_MAX_PLAYERS );
		}
		else
			sprintf ( pLine, "Playing co-op with %s (%d players)", coopsteam_partnername ( 0 ), iPlayers );
		pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
		return;
	}
	int iChoices = coopsteam_choicecount();
	for ( int i = 0; i < iChoices && i < 9; i++ )
		if ( coopnet_keypressed ( '1' + i ) ) { coopsteam_choose ( i ); break; }
	strncpy ( pLine, coopsteam_status(), sizeof(pLine) - 1 ); pLine[sizeof(pLine) - 1] = 0;
	pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
	iChoices = coopsteam_choicecount();
	for ( int i = 0; i < iChoices && i < 9; i++ )
	{
		iY += iLineHeight;
		sprintf ( pLine, "%d - %s", i + 1, coopsteam_choicename ( i ) );
		pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
	}
}

static void coopnet_updatekickui ( void )
{
	// host F7: the players in the game - number = kick, Shift+number = kick and block (saved to
	// coop-blocklist.txt; delete the line to unblock). Client: why the host sent us away.
	char pLine[200];
	int iFont = 1;
	int iY = GetDisplayHeight() / 8;
	int iLineHeight = getbitmapfontheight ( iFont ) + 6;
	if ( g_CoopNetMode == COOPNET_CLIENT )
	{
		if ( g_iCoopNetKickedReason == COOPNET_KICK_NONE ) return;
		strcpy ( pLine, "The host removed you from the game" );
		if ( g_iCoopNetKickedReason == COOPNET_KICK_FILES ) strcpy ( pLine, "Can't join: your game files don't match the host's - reinstall the game" );
		if ( g_iCoopNetKickedReason == COOPNET_KICK_BLOCKED ) strcpy ( pLine, "The host has blocked you from their games" );
		if ( g_iCoopNetKickedReason == COOPNET_KICK_HOSTCHEAT ) strcpy ( pLine, "The host was using cheats - the co-op game ended" );
		if ( g_iCoopNetKickedReason == COOPNET_KICK_FULL ) strcpy ( pLine, "Can't join: the host's game is full" );
		if ( g_iCoopNetKickedReason == COOPNET_KICK_CHEAT ) sprintf ( pLine, "Co-op stopped: %s. Close it and restart the game", g_pCoopNetCheat );
		pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
		return;
	}
	iY = GetDisplayHeight() / 4;	// below the Steam status line
	if ( g_pCoopNetCheat[0] )
	{
		sprintf ( pLine, "Co-op stopped: %s. Close it and restart the game", g_pCoopNetCheat );
		pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
		return;
	}
	static bool s_bOpen = false;
	static char s_pNote[160] = "";
	static DWORD s_dwNoteUntil = 0;
	DWORD dwNow = timeGetTime();
	if ( coopnet_keypressed ( VK_F7 ) ) s_bOpen = !s_bOpen;
	int ids[COOPNET_MAX_PLAYERS], n = 0;
	for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ ) if ( g_CoopNetPlayers[id].bHandshakeDone ) ids[n++] = id;
	if ( s_bOpen )
	{
		bool bShift = ( GetAsyncKeyState ( VK_SHIFT ) & 0x8000 ) != 0;
		for ( int i = 0; i < n; i++ )
		{
			if ( !coopnet_keypressed ( '1' + i ) ) continue;
			std::string sName = coopnet_playername ( ids[i] );
			if ( bShift ) coopnet_block ( ids[i] );
			coopnet_kick ( ids[i], bShift ? COOPNET_KICK_BLOCKED : COOPNET_KICK_KICKED );
			sprintf ( s_pNote, bShift ? "Kicked and blocked %s" : "Kicked %s", sName.c_str() );
			coopnet_event ( "%s", s_pNote );
			s_dwNoteUntil = dwNow + 3000;
			s_bOpen = false;
			break;
		}
	}
	if ( s_bOpen )
	{
		strcpy ( pLine, n > 0 ? "Players - press a number to kick, Shift+number to kick and block (F7 to close)" : "No other players in your game (F7 to close)" );
		pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
		for ( int i = 0; i < n; i++ )
		{
			iY += iLineHeight;
			sprintf ( pLine, "%d - %s", i + 1, coopnet_playername ( ids[i] ).c_str() );
			pastebitmapfont ( pLine, ( GetDisplayWidth() - getbitmapfontwidth ( pLine, iFont ) ) / 2, iY, iFont, 255 );
		}
	}
	else if ( (int)( s_dwNoteUntil - dwNow ) > 0 )
		pastebitmapfont ( s_pNote, ( GetDisplayWidth() - getbitmapfontwidth ( s_pNote, iFont ) ) / 2, iY, iFont, 255 );
}

static void coopnet_drawdebug ( void )
{
	// technical readout: always in the dev build, F9 toggles it in an exported game
	static bool s_bShow = false;
	if ( coopnet_keypressed ( VK_F9 ) ) s_bShow = !s_bShow;
	if ( t.game.gameisexe == 1 && !s_bShow ) return;
	char pLine[256];
	int iX = 10, iY = 10;
	int iFont = 1;
	int iLineHeight = getbitmapfontheight ( iFont ) + 4;
	DWORD dwNow = timeGetTime();

	if ( g_bCoopNetSteam )
	{
		sprintf ( pLine, "COOPNET %s (STEAM)  %s", g_CoopNetMode == COOPNET_HOST ? "HOST" : "CLIENT",
			g_bCoopNetHandshakeDone ? ( g_CoopNetMode == COOPNET_HOST ? "in play" : coopsteam_partnername ( 0 ) ) : coopsteam_status() );
	}
	else if ( g_CoopNetMode == COOPNET_HOST )
	{
		int iPlayers = 1;
		for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ ) if ( g_CoopNetPlayers[id].bHandshakeDone ) iPlayers++;
		if ( g_bCoopNetHostBindFailed )
			sprintf ( pLine, "COOPNET HOST  port %d  BIND FAILED", g_CoopNetAddress.port );
		else
			sprintf ( pLine, "COOPNET HOST  port %d  players %d/%d", g_CoopNetAddress.port, iPlayers, COOPNET_MAX_PLAYERS );
	}
	else
	{
		sprintf ( pLine, "COOPNET CLIENT  -> %s:%d  %s", g_pCoopNetJoinName, g_CoopNetAddress.port,
			g_bCoopNetHandshakeDone ? "connected" : "connecting..." );
	}
	pastebitmapfont ( pLine, iX, iY, iFont, 255 ); iY += iLineHeight;

	sprintf ( pLine, "you are player %d   tx %d/s   rx %d/s", g_iCoopNetLocalID < COOPNET_MAX_PLAYERS ? g_iCoopNetLocalID + 1 : 0, g_iCoopNetTxRate, g_iCoopNetRxRate );
	pastebitmapfont ( pLine, iX, iY, iFont, 255 ); iY += iLineHeight;

	if ( g_bCoopNetLocalValid )
	{
		sprintf ( pLine, "local   %8.1f %8.1f %8.1f   yaw %6.1f  pitch %5.1f",
			g_CoopNetLocal.x, g_CoopNetLocal.y, g_CoopNetLocal.z, g_CoopNetLocal.yaw, g_CoopNetLocal.pitch );
		pastebitmapfont ( pLine, iX, iY, iFont, 255 ); iY += iLineHeight;
	}

	// one line per other player: position, weapon, connection, puppet
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		const sCoopNetPlayerSlot& p = g_CoopNetPlayers[id];
		if ( !coopnet_isremoteplayer ( id ) || !p.remote.bValid ) continue;
		DWORD dwAge = dwNow - p.remote.dwLastReceived;
		int iPing = coopnet_ping ( id );
		char pPing[32] = "relayed";
		if ( iPing >= 0 ) sprintf ( pPing, "ping %dms", iPing );
		const char* pPuppet = p.puppet.iEntity > 0 ? t.entityelement[p.puppet.iEntity].eleprof.name_s.Get() : ( p.puppet.iObj > 0 ? "box" : "-" );
		sprintf ( pLine, "P%d %7.0f %7.0f %7.0f  hp %d  '%s'  %s  as %s %s  %s", id + 1, p.remote.state.x, p.remote.state.y, p.remote.state.z,
			p.remote.state.health, p.remote.pWeapon, pPing, pPuppet, p.puppet.bVisible ? "(shown)" : "(hidden)", dwAge > COOPNET_STALE_MS ? "STALE" : "" );
		pastebitmapfont ( pLine, iX, iY, iFont, 255 ); iY += iLineHeight;
	}

	if ( g_bCoopNetHandshakeDone )
	{
		if ( coopnet_levelsmatch() )
			sprintf ( pLine, "level '%s'  %d characters  zombie sync ON  (%s %d)", g_CoopNetLocalLevel.pName, g_CoopNetLocalLevel.uCharacters,
				g_CoopNetMode == COOPNET_HOST ? "sending" : "mirroring", g_CoopNetMode == COOPNET_HOST ? g_iCoopNetZombiesSent : g_iCoopNetZombiesMirrored );
		else
			sprintf ( pLine, "LEVEL MISMATCH  ours '%s' (%d characters)  zombie sync OFF", g_CoopNetLocalLevel.pName, g_CoopNetLocalLevel.uCharacters );
		pastebitmapfont ( pLine, iX, iY, iFont, 255 ); iY += iLineHeight;
		if ( g_bCoopNetTargetingActive )
		{
			sprintf ( pLine, "targeting: %d characters chasing clients", g_iCoopNetChasingRemote );
			pastebitmapfont ( pLine, iX, iY, iFont, 255 ); iY += iLineHeight;
		}
	}

	if ( g_pCoopNetLastEvent[0] )
	{
		sprintf ( pLine, "last: %s", g_pCoopNetLastEvent );
		pastebitmapfont ( pLine, iX, iY, iFont, 200 );
	}
}

void coopnet_update ( void )
{
	if ( g_CoopNetMode == COOPNET_UNINITIALISED ) coopnet_init();
	DWORD dwNow = timeGetTime();

	// Player1..Player4 entities are taken over even in single player, where they are
	// kept hidden, so a co-op level still plays normally without networking
	coopnet_claimentities ( dwNow );
	if ( g_CoopNetMode == COOPNET_OFF ) return;
	coopnet_updatesettings();
	coopnet_loadingdone ( dwNow );	// first frame after loading a level

	// client keeps retrying until the host is up (not once the host sent us away)
	if ( g_CoopNetMode == COOPNET_CLIENT && g_pCoopNetHost && g_CoopNetPlayers[0].pPeer == NULL && g_iCoopNetKickedReason == COOPNET_KICK_NONE )
	{
		if ( dwNow - g_dwCoopNetLastConnectAttempt >= COOPNET_RECONNECT_MS ) coopnet_clientconnect();
	}

	coopnet_service();
	coopnet_cheatcheck ( dwNow );
	// host: hang up on kicked players once their KICK message has had time to arrive
	for ( int id = 1; id < COOPNET_MAX_PLAYERS; id++ )
		if ( g_dwCoopNetKickAt[id] != 0 && (int)( dwNow - g_dwCoopNetKickAt[id] ) >= 0 ) { g_dwCoopNetKickAt[id] = 0; coopnet_dropconnection ( id, COOPNET_DISCONNECT_NORMAL ); }
	coopnet_readlocalplayer();
	coopnet_updatelevel ( dwNow );
	coopnet_exitupdate ( dwNow );

	// send local player state at a fixed 20 Hz (catch up at most once, never burst)
	if ( (int)(dwNow - g_dwCoopNetNextSend) >= 0 )
	{
		g_dwCoopNetNextSend += COOPNET_SEND_INTERVAL_MS;
		if ( (int)(dwNow - g_dwCoopNetNextSend) >= COOPNET_SEND_INTERVAL_MS ) g_dwCoopNetNextSend = dwNow + COOPNET_SEND_INTERVAL_MS;
		if ( g_bCoopNetConnected && g_bCoopNetHandshakeDone && g_bCoopNetLocalValid ) coopnet_sendplayerstate();
		if ( g_CoopNetMode == COOPNET_HOST && g_bCoopNetConnected && g_bCoopNetHandshakeDone && coopnet_levelsmatch() ) coopnet_sendzombies();
	}
	if ( g_pCoopNetHost ) enet_host_flush ( g_pCoopNetHost );

	// per-second stats + one remote-position log line per second
	if ( dwNow - g_dwCoopNetStatsWindow >= 1000 )
	{
		g_iCoopNetTxRate = g_iCoopNetTxCount; g_iCoopNetRxRate = g_iCoopNetRxCount;
		g_iCoopNetTxCount = 0; g_iCoopNetRxCount = 0;
		g_dwCoopNetStatsWindow = dwNow;
		if ( g_CoopNetMode == COOPNET_CLIENT && g_iCoopNetZombiesMirrored > 0 )
		{
			coopnet_log ( "zombies mirrored %d: per second ok %d, frozen-early %d, frozen-late %d, anim changes %d, anim jumps %d",
				g_iCoopNetZombiesMirrored, g_iCoopNetZombieStatOK, g_iCoopNetZombieStatEarly, g_iCoopNetZombieStatLate, g_iCoopNetZombieStatAnimChange, g_iCoopNetZombieStatResync );
		}
		g_iCoopNetZombieStatOK = g_iCoopNetZombieStatEarly = g_iCoopNetZombieStatLate = g_iCoopNetZombieStatResync = g_iCoopNetZombieStatAnimChange = 0;
		// calibration data for COOPNET_CAPSULE_TO_FEET (only meaningful when standing on terrain)
		if ( g_bCoopNetLocalValid )
			coopnet_log ( "local capsule %.1f above terrain, camera %.1f above capsule, gunid %d gunmode %d action %d",
				g_CoopNetLocal.y - GetLUATerrainHeightEx ( g_CoopNetLocal.x, g_CoopNetLocal.z ),
				CameraPositionY ( 0 ) - g_CoopNetLocal.y, t.gunid, t.gunmode, g_CoopNetLocal.action );
		// what each puppet model is actually doing (to diagnose animations not playing)
		for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		{
			const sCoopNetPuppet& pp = g_CoopNetPlayers[id].puppet;
			if ( !coopnet_isremoteplayer ( id ) || pp.iEntity <= 0 || !pp.bVisible || ObjectExist ( pp.iObj ) == 0 ) continue;
			coopnet_log ( "player %d puppet state %s found=%d frames %.0f-%.0f  model: playing=%d looping=%d frame=%.1f speed=%.0f numframes=%d",
				id + 1, g_pCoopNetAnimStateNames[pp.iAnim + 1], pp.bAnimFound ? 1 : 0, pp.fAnimStart, pp.fAnimFinish,
				GetPlaying ( pp.iObj ), GetLooping ( pp.iObj ), GetFrame ( pp.iObj ), GetSpeed ( pp.iObj ), GetNumberOfFrames ( pp.iObj ) );
		}
	}
	if ( (int)(dwNow - g_dwCoopNetNextRemoteLog) >= 0 )
	{
		g_dwCoopNetNextRemoteLog = dwNow + 1000;
		for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
		{
			const sCoopNetRemotePlayer& r = g_CoopNetPlayers[id].remote;
			if ( !coopnet_isremoteplayer ( id ) || !r.bValid ) continue;
			coopnet_log ( "player %d  pos %.1f %.1f %.1f  yaw %.1f pitch %.1f  seq %u  ping %dms  rx %d/s",
				id + 1, r.state.x, r.state.y, r.state.z, r.state.yaw, r.state.pitch, r.state.seq, coopnet_ping ( id ), g_iCoopNetRxRate );
		}
	}

	// every other player's puppet (the puppet code works on the player g_iCoopNetCur selects)
	for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
	{
		if ( !coopnet_isremoteplayer ( id ) ) continue;
		g_iCoopNetCur = id;
		coopnet_puppet_update ( dwNow );
	}
	coopnet_zombies_update ( dwNow );
	coopnet_updatetargets ( dwNow );
	coopnet_updatedeath();
	coopnet_doors_update();
	coopnet_drawdebug();
	coopnet_drawdeathtext();
	coopnet_drawexittext();
	coopnet_updatesteamui();
	coopnet_updatekickui();
}

void coopnet_menuupdate ( void )
{
	// in-game menu open (game_masterroot_gameloop_loopcode): the game loop - and coopnet_update -
	// stop, but the others keep playing. Keep the connections alive and keep telling them where we
	// are, or they time us out after a few seconds of menu.
	if ( g_CoopNetMode != COOPNET_HOST && g_CoopNetMode != COOPNET_CLIENT ) return;
	DWORD dwNow = timeGetTime();
	coopnet_service();
	coopnet_cheatcheck ( dwNow );
	if ( (int)( dwNow - g_dwCoopNetNextSend ) >= 0 )
	{
		g_dwCoopNetNextSend = dwNow + COOPNET_SEND_INTERVAL_MS;
		if ( g_bCoopNetConnected && g_bCoopNetHandshakeDone && g_bCoopNetLocalValid ) coopnet_sendplayerstate();
	}
	if ( g_pCoopNetHost ) enet_host_flush ( g_pCoopNetHost );
}

void coopnet_earlylaunch ( const char* pCommandLine )
{
	// same test as coopnet_parsecommandline: only the part before project=
	if ( pCommandLine == NULL ) return;
	char pCmd[2048];
	strncpy ( pCmd, pCommandLine, sizeof(pCmd) - 1 );
	pCmd[sizeof(pCmd) - 1] = 0;
	_strlwr ( pCmd );
	char* pProject = strstr ( pCmd, "project=" );
	if ( pProject ) *pProject = 0;
	if ( strstr ( pCmd, "-netsteam=" ) || strstr ( pCmd, "+connect_lobby" ) ) coopsteam_earlyinit();
}

bool coopnet_keeprunninginbackground ( void )
{
	return g_CoopNetMode == COOPNET_HOST || g_CoopNetMode == COOPNET_CLIENT;
}

void coopnet_shutdown ( void )
{
	if ( g_bCoopNetSpectating ) { g.luacameraoverride = 0; g_bCoopNetSpectating = false; }
	g_iCoopNetLastDeadtime = 0;
	g_iCoopNetLivesWhileAlive = -1;
	g_bCoopNetGameOver = false;
	g_bCoopNetLoading = false;
	if ( g_CoopNetMode == COOPNET_HOST || g_CoopNetMode == COOPNET_CLIENT )
	{
		if ( g_bCoopNetSteam ) coopsteam_shutdown();
		for ( int id = 0; id < COOPNET_MAX_PLAYERS; id++ )
			if ( g_CoopNetPlayers[id].pPeer ) enet_peer_disconnect_now ( g_CoopNetPlayers[id].pPeer, COOPNET_DISCONNECT_NORMAL );
		if ( g_pCoopNetHost ) { enet_host_flush ( g_pCoopNetHost ); enet_host_destroy ( g_pCoopNetHost ); }
		g_pCoopNetHost = NULL;
		coopnet_resetsession();
		coopnet_puppet_free();
		g_bCoopNetTargetingActive = false;
		memset ( g_CoopNetTarget, 0, sizeof(g_CoopNetTarget) );
		coopnet_log ( "shutdown" );
		if ( g_pCoopNetLogFile ) { fclose ( g_pCoopNetLogFile ); g_pCoopNetLogFile = NULL; }
		enet_deinitialize();
	}
	g_CoopNetMode = COOPNET_UNINITIALISED;
}
