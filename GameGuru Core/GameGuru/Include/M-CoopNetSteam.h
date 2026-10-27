//----------------------------------------------------
//--- GAMEGURU - M-CoopNetSteam
//--- Steam transport for co-op (used only by M-CoopNet.cpp): a friends-only lobby to find
//--- each other, then Steam P2P packets (NAT traversal / Valve relay - no IPs, no ports).
//--- Connections are numbered like co-op player ids: the host's clients are 1..3 (first free
//--- one when they join), a client's only connection is the host, 0.
//----------------------------------------------------

#pragma once

#define COOPSTEAM_MAX_PLAYERS	4

enum eCoopSteamEvent
{
	COOPSTEAM_EVENT_NONE = 0,
	COOPSTEAM_EVENT_CONNECTED,		// a player connected (host: a friend joined, client: the host accepted us)
	COOPSTEAM_EVENT_DISCONNECTED,	// a player left, timed out or could not be reached
	COOPSTEAM_EVENT_REFUSED,		// client: the host's game was full
};

typedef void ( *CoopSteamLogFn ) ( const char* pLine );

// At launch, before the renderer is created: starts the Steam API so Steam's overlay can attach
// to the game (Shift+Tab). Only call it when co-op over Steam was asked for.
void coopsteam_earlyinit ( void );

// Starts Steam (Steam must be running and signed in). Host: creates the friends-only lobby.
// Client: starts looking for a friend who is hosting, and accepts Steam invites. Returns false
// (with the reason logged and in coopsteam_status) if Steam is not available.
bool coopsteam_start ( bool bHost, CoopSteamLogFn pLog );

// Once per frame: Steam callbacks, friend search, connection keep-alive and time-outs.
void coopsteam_update ( void );

// Connection events since the last call (call until COOPSTEAM_EVENT_NONE); *piPlayer = which one.
int coopsteam_pollevent ( int* piPlayer );

// Next game packet; false when there are none. *piPlayer = who sent it.
bool coopsteam_receive ( unsigned char* pBuf, int iBufSize, int* piLen, int* piPlayer );

bool coopsteam_send ( int iPlayer, const void* pData, int iLen, bool bReliable );
bool coopsteam_connected ( int iPlayer );
int coopsteam_ping ( int iPlayer );				// round trip in ms, -1 if not measured yet
void coopsteam_settimeout ( int iPlayer, int iMs );	// silence allowed before that player is dropped (0 = normal)
void coopsteam_disconnect ( int iPlayer );		// drop that player (client: go back to looking)
void coopsteam_shutdown ( void );				// leave the lobby and close the sessions

const char* coopsteam_status ( void );			// one line for the screen while not connected
const char* coopsteam_partnername ( int iPlayer );	// Steam name of that player ("" if none)
unsigned long long coopsteam_partnersteamid ( int iPlayer );	// their Steam ID (0 if none) - for the host's block list

// client: when several friends are hosting, the player picks one
int coopsteam_choicecount ( void );
const char* coopsteam_choicename ( int i );
void coopsteam_choose ( int i );
