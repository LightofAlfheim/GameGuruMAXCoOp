//----------------------------------------------------
//--- GAMEGURU - M-CoopNetSteam
//--- Steam transport for co-op, see M-CoopNetSteam.h.
//---
//--- Finding each other: the host creates a friends-only Steam lobby (up to 4) tagged "edcoop".
//--- A client joins it from the launcher's friend list ("+connect_lobby <id>" on the command line),
//--- from a Steam invite accepted while the game is running, or by itself: it looks through its
//--- Steam friends for ones playing this app whose lobby carries the tag (several = player picks).
//--- Talking: Steam P2P packets (ISteamNetworking - NAT traversal with Valve relay fallback).
//--- Every packet starts with a transport byte; game packets are passed up unchanged.
//--- Connections are kept per co-op player id: the host's clients get 1..3, a client's host is 0.
//---
//--- App ID: steam_appid.txt next to the exe if there is one, else Valve's test app 480
//--- ("Spacewar") - fine for testing with friends; a released game needs its own App ID.
//----------------------------------------------------

#define _CRT_SECURE_NO_WARNINGS		// as the rest of the engine (and the Steamworks headers need it)
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <deque>
#include <string>
#include "steam/steam_api.h"
#include "M-CoopNetSteam.h"

#define COOPSTEAM_DEFAULT_APPID		"480"
#define COOPSTEAM_LOBBY_TAG_KEY		"edcoop"	// lobby data marking a co-op lobby of this game
#define COOPSTEAM_LOBBY_TAG_VALUE	"1"
#define COOPSTEAM_SEARCH_MS			2000		// client: how often to look for friends hosting
#define COOPSTEAM_CONNECT_RETRY_MS	1000		// client: CONNECT resend while waiting for ACCEPT
#define COOPSTEAM_PING_MS			1000
#define COOPSTEAM_TIMEOUT_MS		10000		// no packet for this long = player gone
#define COOPSTEAM_UNRELIABLE_MAX	1200		// Steam's limit for unreliable P2P packets
#define COOPSTEAM_HOST				0			// a client's connection to the host

enum eCoopSteamPacket
{
	COOPSTEAM_PKT_GAME = 0,		// game packet follows
	COOPSTEAM_PKT_CONNECT,		// client -> host: let me in
	COOPSTEAM_PKT_ACCEPT,		// host -> client: you're in
	COOPSTEAM_PKT_BUSY,			// host -> client: the game is full
	COOPSTEAM_PKT_BYE,			// either way: leaving
	COOPSTEAM_PKT_PING,			// u32 sender time
	COOPSTEAM_PKT_PONG,			// u32 the time from the PING
};

struct sCoopSteamChoice
{
	CSteamID lobbyID;
	char pName[64];			// the host's Steam name
};

struct sCoopSteamPeer
{
	CSteamID id;			// valid = in use (client: also while still knocking on the host)
	bool bConnected;
	DWORD dwLastReceived, dwNextPing, dwNextConnect;
	DWORD dwTimeout;		// silence allowed before dropping them (longer while they load a level)
	int iPing;
	char pName[64];
};

struct sCoopSteamEvent { int iEvent, iPlayer; };
struct sCoopSteamPacket { int iPlayer; std::vector<unsigned char> data; };

class CCoopSteam
{
public:
	CCoopSteam ( bool bHost );

	bool m_bHost;
	CSteamID m_Lobby;					// the lobby we are in (host: ours)
	sCoopSteamPeer m_Peers[COOPSTEAM_MAX_PLAYERS];	// host: 1..3, client: COOPSTEAM_HOST
	bool m_bJoining;					// client: JoinLobby in flight
	DWORD m_dwNextSearch;
	std::deque<sCoopSteamEvent> m_Events;
	std::deque<sCoopSteamPacket> m_Incoming;
	std::vector<sCoopSteamChoice> m_Choices;
	std::vector<CSteamID> m_Pending;	// client: friends' lobbies we asked Steam for data on
	char m_pStatus[160];

	int FindPeer ( CSteamID id );
	int ConnectedCount ( void );
	void SetStatus ( const char* pFormat, ... );
	void UpdateHostStatus ( void );
	void SendRaw ( CSteamID to, unsigned char type, const void* pData, int iLen, EP2PSend eSend );
	void SetConnected ( int iPlayer, CSteamID id );
	void Drop ( int iPlayer, int iEvent, const char* pWhy );
	void JoinLobby ( CSteamID lobby, const char* pWhy );
	void Search ( void );
	void ReadPackets ( void );
	void Update ( void );

	CCallResult<CCoopSteam, LobbyCreated_t> m_CreateCall;
	CCallResult<CCoopSteam, LobbyEnter_t> m_JoinCall;
	void OnLobbyCreated ( LobbyCreated_t* p, bool bIOFailure );
	void OnLobbyJoined ( LobbyEnter_t* p, bool bIOFailure );

	STEAM_CALLBACK ( CCoopSteam, OnJoinRequested, GameLobbyJoinRequested_t, m_cbJoinRequested );
	STEAM_CALLBACK ( CCoopSteam, OnSessionRequest, P2PSessionRequest_t, m_cbSessionRequest );
	STEAM_CALLBACK ( CCoopSteam, OnSessionFail, P2PSessionConnectFail_t, m_cbSessionFail );
	STEAM_CALLBACK ( CCoopSteam, OnLobbyChatUpdate, LobbyChatUpdate_t, m_cbLobbyChatUpdate );
	STEAM_CALLBACK ( CCoopSteam, OnLobbyDataUpdate, LobbyDataUpdate_t, m_cbLobbyDataUpdate );
};

static CCoopSteam* g_pCoopSteam = NULL;
static CoopSteamLogFn g_pCoopSteamLog = NULL;
static char g_pCoopSteamStartError[160] = "";

static void coopsteam_log ( const char* pFormat, ... )
{
	char pLine[512];
	va_list args;
	va_start ( args, pFormat );
	vsnprintf ( pLine, sizeof(pLine), pFormat, args );
	va_end ( args );
	if ( g_pCoopSteamLog ) g_pCoopSteamLog ( pLine );
}

static bool coopsteam_validplayer ( int i ) { return i >= 0 && i < COOPSTEAM_MAX_PLAYERS; }

CCoopSteam::CCoopSteam ( bool bHost )
	: m_cbJoinRequested ( this, &CCoopSteam::OnJoinRequested ),
	  m_cbSessionRequest ( this, &CCoopSteam::OnSessionRequest ),
	  m_cbSessionFail ( this, &CCoopSteam::OnSessionFail ),
	  m_cbLobbyChatUpdate ( this, &CCoopSteam::OnLobbyChatUpdate ),
	  m_cbLobbyDataUpdate ( this, &CCoopSteam::OnLobbyDataUpdate )
{
	m_bHost = bHost;
	m_bJoining = false;
	m_dwNextSearch = 0;
	m_pStatus[0] = 0;
	for ( int i = 0; i < COOPSTEAM_MAX_PLAYERS; i++ )
	{
		m_Peers[i].bConnected = false;
		m_Peers[i].dwLastReceived = m_Peers[i].dwNextPing = m_Peers[i].dwNextConnect = 0;
		m_Peers[i].iPing = -1;
		m_Peers[i].pName[0] = 0;
		m_Peers[i].dwTimeout = COOPSTEAM_TIMEOUT_MS;
	}
}

int CCoopSteam::FindPeer ( CSteamID id )
{
	for ( int i = 0; i < COOPSTEAM_MAX_PLAYERS; i++ ) if ( id.IsValid() && m_Peers[i].id == id ) return i;
	return -1;
}

int CCoopSteam::ConnectedCount ( void )
{
	int n = 0;
	for ( int i = 0; i < COOPSTEAM_MAX_PLAYERS; i++ ) if ( m_Peers[i].bConnected ) n++;
	return n;
}

void CCoopSteam::SetStatus ( const char* pFormat, ... )
{
	va_list args;
	va_start ( args, pFormat );
	vsnprintf ( m_pStatus, sizeof(m_pStatus), pFormat, args );
	va_end ( args );
}

void CCoopSteam::UpdateHostStatus ( void )
{
	int iPlayers = 1 + ConnectedCount();
	char pPresence[64];
	sprintf ( pPresence, "Hosting co-op (%d/%d)", iPlayers, COOPSTEAM_MAX_PLAYERS );
	SteamFriends()->SetRichPresence ( "status", pPresence );
	if ( iPlayers < COOPSTEAM_MAX_PLAYERS )
		SetStatus ( "Hosting on Steam (%d/%d players) - friends can join from their Join co-op list", iPlayers, COOPSTEAM_MAX_PLAYERS );
	else
		SetStatus ( "Hosting on Steam - the game is full (%d/%d players)", iPlayers, COOPSTEAM_MAX_PLAYERS );
}

void CCoopSteam::SendRaw ( CSteamID to, unsigned char type, const void* pData, int iLen, EP2PSend eSend )
{
	unsigned char buf[16384];
	if ( iLen + 1 > (int)sizeof(buf) ) return;
	buf[0] = type;
	if ( iLen > 0 ) memcpy ( buf + 1, pData, iLen );
	if ( eSend != k_EP2PSendReliable && iLen + 1 > COOPSTEAM_UNRELIABLE_MAX ) eSend = k_EP2PSendReliable;
	SteamNetworking()->SendP2PPacket ( to, buf, (uint32)( iLen + 1 ), eSend, 0 );
}

void CCoopSteam::SetConnected ( int iPlayer, CSteamID id )
{
	sCoopSteamPeer& p = m_Peers[iPlayer];
	p.id = id;
	p.bConnected = true;
	p.dwLastReceived = GetTickCount();
	p.dwNextPing = 0;
	p.iPing = -1;
	strncpy ( p.pName, SteamFriends()->GetFriendPersonaName ( id ), sizeof(p.pName) - 1 );
	p.pName[sizeof(p.pName) - 1] = 0;
	if ( m_bHost )
		UpdateHostStatus();
	else
	{
		m_bJoining = false;
		m_Choices.clear();
		SetStatus ( "Connected to %s", p.pName );
		SteamFriends()->SetRichPresence ( "status", "Playing co-op" );
	}
	coopsteam_log ( "steam: %s connected (player %d)", p.pName, iPlayer + 1 );
	sCoopSteamEvent ev = { COOPSTEAM_EVENT_CONNECTED, iPlayer };
	m_Events.push_back ( ev );
}

void CCoopSteam::Drop ( int iPlayer, int iEvent, const char* pWhy )
{
	// lose one player; the host keeps its lobby open for others, a client goes back to looking
	if ( !coopsteam_validplayer ( iPlayer ) ) return;
	sCoopSteamPeer& p = m_Peers[iPlayer];
	bool bWasConnected = p.bConnected;
	if ( p.id.IsValid() )
	{
		SendRaw ( p.id, COOPSTEAM_PKT_BYE, NULL, 0, k_EP2PSendReliable );
		SteamNetworking()->CloseP2PSessionWithUser ( p.id );
	}
	coopsteam_log ( "steam: player %d (%s) dropped (%s)", iPlayer + 1, p.pName, pWhy );
	p.id = CSteamID();
	p.bConnected = false;
	p.pName[0] = 0;
	p.dwTimeout = COOPSTEAM_TIMEOUT_MS;
	// their packets still queued are of no use now
	for ( std::deque<sCoopSteamPacket>::iterator it = m_Incoming.begin(); it != m_Incoming.end(); )
		it = it->iPlayer == iPlayer ? m_Incoming.erase ( it ) : it + 1;
	if ( m_bHost )
		UpdateHostStatus();
	else
	{
		m_bJoining = false;
		if ( m_Lobby.IsValid() ) { SteamMatchmaking()->LeaveLobby ( m_Lobby ); m_Lobby = CSteamID(); }
		SetStatus ( "Looking for a friend hosting on Steam..." );
		SteamFriends()->SetRichPresence ( "status", NULL );
		m_dwNextSearch = 0;
	}
	if ( bWasConnected || iEvent == COOPSTEAM_EVENT_REFUSED )
	{
		sCoopSteamEvent ev = { iEvent, iPlayer };
		m_Events.push_back ( ev );
	}
}

void CCoopSteam::JoinLobby ( CSteamID lobby, const char* pWhy )
{
	if ( m_bHost || m_Peers[COOPSTEAM_HOST].bConnected || !lobby.IsValid() ) return;
	if ( m_Lobby.IsValid() ) { SteamMatchmaking()->LeaveLobby ( m_Lobby ); m_Lobby = CSteamID(); }
	coopsteam_log ( "steam: joining lobby %llu (%s)", lobby.ConvertToUint64(), pWhy );
	m_bJoining = true;
	m_Choices.clear();
	SetStatus ( "Joining your friend's game..." );
	m_JoinCall.Set ( SteamMatchmaking()->JoinLobby ( lobby ), this, &CCoopSteam::OnLobbyJoined );
}

void CCoopSteam::OnLobbyCreated ( LobbyCreated_t* p, bool bIOFailure )
{
	if ( bIOFailure || p->m_eResult != k_EResultOK )
	{
		SetStatus ( "Could not create a Steam lobby (error %d) - is Steam online?", bIOFailure ? -1 : (int)p->m_eResult );
		coopsteam_log ( "steam: %s", m_pStatus );
		return;
	}
	m_Lobby = CSteamID ( p->m_ulSteamIDLobby );
	SteamMatchmaking()->SetLobbyData ( m_Lobby, COOPSTEAM_LOBBY_TAG_KEY, COOPSTEAM_LOBBY_TAG_VALUE );
	SteamMatchmaking()->SetLobbyData ( m_Lobby, "host", SteamFriends()->GetPersonaName() );
	UpdateHostStatus();
	coopsteam_log ( "steam: lobby %llu created", m_Lobby.ConvertToUint64() );
}

void CCoopSteam::OnLobbyJoined ( LobbyEnter_t* p, bool bIOFailure )
{
	if ( bIOFailure || p->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess )
	{
		coopsteam_log ( "steam: could not join lobby (response %d)", bIOFailure ? -1 : (int)p->m_EChatRoomEnterResponse );
		m_bJoining = false;
		SetStatus ( "Could not join (the game may be full) - looking again..." );
		m_dwNextSearch = GetTickCount() + COOPSTEAM_SEARCH_MS;
		return;
	}
	m_Lobby = CSteamID ( p->m_ulSteamIDLobby );
	m_bJoining = false;
	CSteamID owner = SteamMatchmaking()->GetLobbyOwner ( m_Lobby );
	if ( !owner.IsValid() || owner == SteamUser()->GetSteamID() )
	{
		SteamMatchmaking()->LeaveLobby ( m_Lobby );
		m_Lobby = CSteamID();
		m_dwNextSearch = GetTickCount() + COOPSTEAM_SEARCH_MS;
		coopsteam_log ( "steam: lobby has no host" );
		return;
	}
	sCoopSteamPeer& h = m_Peers[COOPSTEAM_HOST];
	h.id = owner;
	h.dwNextConnect = 0;	// send CONNECT right away (Update)
	h.dwLastReceived = GetTickCount();
	strncpy ( h.pName, SteamFriends()->GetFriendPersonaName ( owner ), sizeof(h.pName) - 1 );
	h.pName[sizeof(h.pName) - 1] = 0;
	SetStatus ( "Connecting to %s...", h.pName );
	coopsteam_log ( "steam: in lobby, connecting to host %s", h.pName );
}

void CCoopSteam::OnJoinRequested ( GameLobbyJoinRequested_t* p )
{
	// accepted an invite, or "Join game" on a friend in the Steam friends list
	JoinLobby ( p->m_steamIDLobby, "Steam invite / friends list" );
}

void CCoopSteam::OnSessionRequest ( P2PSessionRequest_t* p )
{
	// only talk to Steam friends (and whoever is in our lobby)
	bool bFriend = SteamFriends()->GetFriendRelationship ( p->m_steamIDRemote ) == k_EFriendRelationshipFriend;
	bool bInLobby = false;
	if ( m_Lobby.IsValid() )
	{
		int n = SteamMatchmaking()->GetNumLobbyMembers ( m_Lobby );
		for ( int i = 0; i < n; i++ ) if ( SteamMatchmaking()->GetLobbyMemberByIndex ( m_Lobby, i ) == p->m_steamIDRemote ) bInLobby = true;
	}
	if ( bFriend || bInLobby ) SteamNetworking()->AcceptP2PSessionWithUser ( p->m_steamIDRemote );
	else coopsteam_log ( "steam: ignored a connection from a non-friend" );
}

void CCoopSteam::OnSessionFail ( P2PSessionConnectFail_t* p )
{
	int i = FindPeer ( p->m_steamIDRemote );
	if ( i < 0 ) return;
	char pWhy[64];
	sprintf ( pWhy, "could not reach them, error %d", (int)p->m_eP2PSessionError );
	Drop ( i, COOPSTEAM_EVENT_DISCONNECTED, pWhy );
}

void CCoopSteam::OnLobbyChatUpdate ( LobbyChatUpdate_t* p )
{
	// a player left the lobby (quit the game, lost Steam...)
	if ( CSteamID ( p->m_ulSteamIDLobby ) != m_Lobby ) return;
	const uint32 uLeft = k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected | k_EChatMemberStateChangeKicked | k_EChatMemberStateChangeBanned;
	if ( !( p->m_rgfChatMemberStateChange & uLeft ) ) return;
	int i = FindPeer ( CSteamID ( p->m_ulSteamIDUserChanged ) );
	if ( i >= 0 ) Drop ( i, COOPSTEAM_EVENT_DISCONNECTED, "left the lobby" );
}

void CCoopSteam::OnLobbyDataUpdate ( LobbyDataUpdate_t* p )
{
	// data for a friend's lobby arrived (Search asked for it) - search again now
	if ( !m_bHost && !m_Peers[COOPSTEAM_HOST].id.IsValid() && !m_bJoining ) m_dwNextSearch = 0;
}

void CCoopSteam::Search ( void )
{
	// client: co-op lobbies of this app that Steam friends are in (the host, or players already
	// in it - one entry per lobby, named after its host)
	std::vector<sCoopSteamChoice> found;
	AppId_t uApp = SteamUtils()->GetAppID();
	int n = SteamFriends()->GetFriendCount ( k_EFriendFlagImmediate );
	for ( int i = 0; i < n; i++ )
	{
		CSteamID f = SteamFriends()->GetFriendByIndex ( i, k_EFriendFlagImmediate );
		FriendGameInfo_t info;
		if ( !SteamFriends()->GetFriendGamePlayed ( f, &info ) ) continue;
		if ( info.m_gameID.AppID() != uApp || !info.m_steamIDLobby.IsValid() ) continue;
		const char* pTag = SteamMatchmaking()->GetLobbyData ( info.m_steamIDLobby, COOPSTEAM_LOBBY_TAG_KEY );
		if ( pTag == NULL || pTag[0] == 0 )
		{
			// lobby data not known yet: ask once (OnLobbyDataUpdate brings us back here)
			bool bAsked = false;
			for ( size_t a = 0; a < m_Pending.size(); a++ ) if ( m_Pending[a] == info.m_steamIDLobby ) bAsked = true;
			if ( !bAsked ) { m_Pending.push_back ( info.m_steamIDLobby ); SteamMatchmaking()->RequestLobbyData ( info.m_steamIDLobby ); }
			continue;
		}
		if ( strcmp ( pTag, COOPSTEAM_LOBBY_TAG_VALUE ) != 0 ) continue;
		bool bDup = false;
		for ( size_t d = 0; d < found.size(); d++ ) if ( found[d].lobbyID == info.m_steamIDLobby ) bDup = true;
		if ( bDup ) continue;
		sCoopSteamChoice c;
		c.lobbyID = info.m_steamIDLobby;
		const char* pHost = SteamMatchmaking()->GetLobbyData ( info.m_steamIDLobby, "host" );
		strncpy ( c.pName, pHost && pHost[0] ? pHost : SteamFriends()->GetFriendPersonaName ( f ), sizeof(c.pName) - 1 );
		c.pName[sizeof(c.pName) - 1] = 0;
		found.push_back ( c );
	}
	m_Choices = found;
	if ( found.size() == 1 )
		JoinLobby ( found[0].lobbyID, found[0].pName );
	else if ( found.size() > 1 )
		SetStatus ( "%d friends are hosting - press a number to pick one", (int)found.size() );
	else
		SetStatus ( "Looking for a friend hosting on Steam..." );
}

void CCoopSteam::ReadPackets ( void )
{
	uint32 uSize = 0;
	while ( SteamNetworking()->IsP2PPacketAvailable ( &uSize, 0 ) )
	{
		std::vector<unsigned char> buf ( uSize > 0 ? uSize : 1 );
		CSteamID from;
		uint32 uRead = 0;
		if ( !SteamNetworking()->ReadP2PPacket ( &buf[0], uSize, &uRead, &from, 0 ) || uRead == 0 ) continue;
		unsigned char type = buf[0];
		int i = FindPeer ( from );
		if ( m_bHost && type == COOPSTEAM_PKT_CONNECT )
		{
			// a new client gets the first free player id (1..3); a full game says so
			if ( i < 0 )
				for ( int s = 1; s < COOPSTEAM_MAX_PLAYERS && i < 0; s++ ) if ( !m_Peers[s].id.IsValid() ) i = s;
			if ( i < 0 ) { SendRaw ( from, COOPSTEAM_PKT_BUSY, NULL, 0, k_EP2PSendReliable ); continue; }
			SendRaw ( from, COOPSTEAM_PKT_ACCEPT, NULL, 0, k_EP2PSendReliable );
			if ( !m_Peers[i].bConnected ) SetConnected ( i, from );
			continue;
		}
		if ( i < 0 ) continue;	// anyone else: not in our game
		sCoopSteamPeer& p = m_Peers[i];
		p.dwLastReceived = GetTickCount();
		switch ( type )
		{
			case COOPSTEAM_PKT_GAME:
				if ( p.bConnected )
				{
					sCoopSteamPacket pk;
					pk.iPlayer = i;
					pk.data.assign ( buf.begin() + 1, buf.begin() + uRead );
					m_Incoming.push_back ( pk );
				}
				break;
			case COOPSTEAM_PKT_ACCEPT:
				if ( !m_bHost && !p.bConnected ) SetConnected ( i, from );
				break;
			case COOPSTEAM_PKT_BUSY:
				Drop ( i, COOPSTEAM_EVENT_REFUSED, "the game is full" );
				break;
			case COOPSTEAM_PKT_BYE:
				Drop ( i, COOPSTEAM_EVENT_DISCONNECTED, "left" );
				break;
			case COOPSTEAM_PKT_PING:
				if ( uRead >= 5 ) SendRaw ( from, COOPSTEAM_PKT_PONG, &buf[1], 4, k_EP2PSendUnreliableNoDelay );
				break;
			case COOPSTEAM_PKT_PONG:
				if ( uRead >= 5 ) { DWORD dwSent; memcpy ( &dwSent, &buf[1], 4 ); p.iPing = (int)( GetTickCount() - dwSent ); }
				break;
			default:
				break;
		}
	}
}

void CCoopSteam::Update ( void )
{
	DWORD dwNow = GetTickCount();
	ReadPackets();
	for ( int i = 0; i < COOPSTEAM_MAX_PLAYERS; i++ )
	{
		sCoopSteamPeer& p = m_Peers[i];
		if ( !p.id.IsValid() ) continue;
		if ( p.bConnected )
		{
			if ( (int)( dwNow - p.dwNextPing ) >= 0 )
			{
				p.dwNextPing = dwNow + COOPSTEAM_PING_MS;
				SendRaw ( p.id, COOPSTEAM_PKT_PING, &dwNow, 4, k_EP2PSendUnreliableNoDelay );
			}
			if ( (int)( dwNow - p.dwLastReceived ) > (int)p.dwTimeout ) Drop ( i, COOPSTEAM_EVENT_DISCONNECTED, "timed out" );
		}
		else if ( !m_bHost )
		{
			// in the host's lobby: knock until let in
			if ( (int)( dwNow - p.dwNextConnect ) >= 0 )
			{
				p.dwNextConnect = dwNow + COOPSTEAM_CONNECT_RETRY_MS;
				SendRaw ( p.id, COOPSTEAM_PKT_CONNECT, NULL, 0, k_EP2PSendReliable );
			}
			if ( (int)( dwNow - p.dwLastReceived ) > COOPSTEAM_TIMEOUT_MS ) Drop ( i, COOPSTEAM_EVENT_DISCONNECTED, "host did not answer" );
		}
	}
	if ( m_bHost || m_Peers[COOPSTEAM_HOST].id.IsValid() || m_bJoining ) return;
	if ( (int)( dwNow - m_dwNextSearch ) >= 0 )
	{
		m_dwNextSearch = dwNow + COOPSTEAM_SEARCH_MS;
		Search();
	}
}

//
// API
//

static bool coopsteam_haveappidfile ( void )
{
	// steam_appid.txt in the working folder (SteamAPI_Init reads it from there)
	FILE* fp = fopen ( "steam_appid.txt", "r" );
	if ( fp == NULL ) return false;
	fclose ( fp );
	return true;
}

static bool g_bCoopSteamAPIReady = false;
static char g_pCoopSteamEarlyNote[96] = "";

static bool coopsteam_initapi ( void )
{
	// SteamAPI_Init with our App ID (once per process)
	if ( g_bCoopSteamAPIReady ) return true;
	if ( !coopsteam_haveappidfile() )
	{
		char pEnv[32] = "";
		if ( GetEnvironmentVariableA ( "SteamAppId", pEnv, sizeof(pEnv) ) == 0 )
		{
			// no App ID given: Valve's public test app (see top of file)
			SetEnvironmentVariableA ( "SteamAppId", COOPSTEAM_DEFAULT_APPID );
			SetEnvironmentVariableA ( "SteamGameId", COOPSTEAM_DEFAULT_APPID );
		}
	}
	bool bInit = SteamAPI_Init();
	if ( !bInit && !coopsteam_haveappidfile() )
	{
		// this steam_api build may only take the App ID from the file: write it and try again
		FILE* fp = fopen ( "steam_appid.txt", "w" );
		if ( fp ) { fputs ( COOPSTEAM_DEFAULT_APPID, fp ); fclose ( fp ); bInit = SteamAPI_Init(); }
		if ( bInit ) strcpy ( g_pCoopSteamEarlyNote, "wrote steam_appid.txt (" COOPSTEAM_DEFAULT_APPID ")" );
	}
	g_bCoopSteamAPIReady = bInit;
	return bInit;
}

void coopsteam_earlyinit ( void )
{
	// at launch, before the renderer exists, so Steam's overlay can attach to the game
	// (Shift+Tab); coopsteam_start finishes the job later
	bool bOK = coopsteam_initapi();
	sprintf ( g_pCoopSteamEarlyNote + strlen ( g_pCoopSteamEarlyNote ), "%sSteam started at launch: %s", g_pCoopSteamEarlyNote[0] ? ", " : "", bOK ? "yes" : "no" );
}

bool coopsteam_start ( bool bHost, CoopSteamLogFn pLog )
{
	g_pCoopSteamLog = pLog;
	if ( g_pCoopSteam ) return true;
	if ( g_pCoopSteamEarlyNote[0] ) { coopsteam_log ( "steam: %s", g_pCoopSteamEarlyNote ); g_pCoopSteamEarlyNote[0] = 0; }
	bool bInit = coopsteam_initapi();
	if ( !bInit )
	{
		strcpy ( g_pCoopSteamStartError, "Steam is not running - start Steam and sign in, then try again" );
		coopsteam_log ( "steam: SteamAPI_Init failed" );
		return false;
	}
	if ( !SteamUser()->BLoggedOn() )
	{
		strcpy ( g_pCoopSteamStartError, "Steam is offline - sign in to Steam, then try again" );
		coopsteam_log ( "steam: not logged on" );
		return false;
	}
	g_pCoopSteam = new CCoopSteam ( bHost );
	SteamNetworking()->AllowP2PPacketRelay ( true );
	coopsteam_log ( "steam: started as %s, app %u, user %s, overlay %s", bHost ? "host" : "client", SteamUtils()->GetAppID(),
		SteamFriends()->GetPersonaName(), SteamUtils()->IsOverlayEnabled() ? "available" : "not available" );
	if ( bHost )
	{
		g_pCoopSteam->SetStatus ( "Creating Steam lobby..." );
		g_pCoopSteam->m_CreateCall.Set ( SteamMatchmaking()->CreateLobby ( k_ELobbyTypeFriendsOnly, COOPSTEAM_MAX_PLAYERS ), g_pCoopSteam, &CCoopSteam::OnLobbyCreated );
	}
	else
	{
		g_pCoopSteam->SetStatus ( "Looking for a friend hosting on Steam..." );
		// launched by Steam from an invite: +connect_lobby <id>
		const char* pCmd = strstr ( GetCommandLineA(), "+connect_lobby" );
		if ( pCmd )
		{
			unsigned long long uLobby = _strtoui64 ( pCmd + 14, NULL, 10 );
			if ( uLobby ) g_pCoopSteam->JoinLobby ( CSteamID ( (uint64)uLobby ), "+connect_lobby" );
		}
	}
	return true;
}

void coopsteam_update ( void )
{
	if ( g_pCoopSteam == NULL ) return;
	SteamAPI_RunCallbacks();
	g_pCoopSteam->Update();
}

int coopsteam_pollevent ( int* piPlayer )
{
	if ( g_pCoopSteam == NULL || g_pCoopSteam->m_Events.empty() ) return COOPSTEAM_EVENT_NONE;
	sCoopSteamEvent ev = g_pCoopSteam->m_Events.front();
	g_pCoopSteam->m_Events.pop_front();
	*piPlayer = ev.iPlayer;
	return ev.iEvent;
}

bool coopsteam_receive ( unsigned char* pBuf, int iBufSize, int* piLen, int* piPlayer )
{
	if ( g_pCoopSteam == NULL || g_pCoopSteam->m_Incoming.empty() ) return false;
	sCoopSteamPacket& p = g_pCoopSteam->m_Incoming.front();
	int n = (int)p.data.size();
	if ( n > iBufSize ) n = iBufSize;
	if ( n > 0 ) memcpy ( pBuf, &p.data[0], n );
	*piLen = n;
	*piPlayer = p.iPlayer;
	g_pCoopSteam->m_Incoming.pop_front();
	return true;
}

bool coopsteam_send ( int iPlayer, const void* pData, int iLen, bool bReliable )
{
	if ( g_pCoopSteam == NULL || !coopsteam_validplayer ( iPlayer ) || !g_pCoopSteam->m_Peers[iPlayer].bConnected ) return false;
	g_pCoopSteam->SendRaw ( g_pCoopSteam->m_Peers[iPlayer].id, COOPSTEAM_PKT_GAME, pData, iLen, bReliable ? k_EP2PSendReliable : k_EP2PSendUnreliableNoDelay );
	return true;
}

bool coopsteam_connected ( int iPlayer )
{
	return g_pCoopSteam && coopsteam_validplayer ( iPlayer ) && g_pCoopSteam->m_Peers[iPlayer].bConnected;
}

int coopsteam_ping ( int iPlayer )
{
	return g_pCoopSteam && coopsteam_validplayer ( iPlayer ) ? g_pCoopSteam->m_Peers[iPlayer].iPing : -1;
}

void coopsteam_settimeout ( int iPlayer, int iMs )
{
	if ( g_pCoopSteam && coopsteam_validplayer ( iPlayer ) )
		g_pCoopSteam->m_Peers[iPlayer].dwTimeout = iMs > 0 ? (DWORD)iMs : COOPSTEAM_TIMEOUT_MS;
}

void coopsteam_disconnect ( int iPlayer )
{
	if ( g_pCoopSteam && coopsteam_validplayer ( iPlayer ) && g_pCoopSteam->m_Peers[iPlayer].id.IsValid() )
		g_pCoopSteam->Drop ( iPlayer, COOPSTEAM_EVENT_DISCONNECTED, "disconnected locally" );
}

void coopsteam_shutdown ( void )
{
	// leave Steam itself running (SteamAPI stays initialised for the rest of the process)
	if ( g_pCoopSteam == NULL ) return;
	for ( int i = 0; i < COOPSTEAM_MAX_PLAYERS; i++ )
	{
		CSteamID id = g_pCoopSteam->m_Peers[i].id;
		if ( !id.IsValid() ) continue;
		g_pCoopSteam->SendRaw ( id, COOPSTEAM_PKT_BYE, NULL, 0, k_EP2PSendReliable );
		SteamNetworking()->CloseP2PSessionWithUser ( id );
	}
	if ( g_pCoopSteam->m_Lobby.IsValid() ) SteamMatchmaking()->LeaveLobby ( g_pCoopSteam->m_Lobby );
	SteamFriends()->SetRichPresence ( "status", NULL );
	SteamAPI_RunCallbacks();
	delete g_pCoopSteam;
	g_pCoopSteam = NULL;
}

const char* coopsteam_status ( void ) { return g_pCoopSteam ? g_pCoopSteam->m_pStatus : g_pCoopSteamStartError; }

const char* coopsteam_partnername ( int iPlayer )
{
	return g_pCoopSteam && coopsteam_validplayer ( iPlayer ) ? g_pCoopSteam->m_Peers[iPlayer].pName : "";
}

unsigned long long coopsteam_partnersteamid ( int iPlayer )
{
	return g_pCoopSteam && coopsteam_validplayer ( iPlayer ) ? g_pCoopSteam->m_Peers[iPlayer].id.ConvertToUint64() : 0;
}

int coopsteam_choicecount ( void ) { return g_pCoopSteam ? (int)g_pCoopSteam->m_Choices.size() : 0; }

const char* coopsteam_choicename ( int i )
{
	if ( g_pCoopSteam == NULL || i < 0 || i >= (int)g_pCoopSteam->m_Choices.size() ) return "";
	return g_pCoopSteam->m_Choices[i].pName;
}

void coopsteam_choose ( int i )
{
	if ( g_pCoopSteam == NULL || i < 0 || i >= (int)g_pCoopSteam->m_Choices.size() ) return;
	sCoopSteamChoice c = g_pCoopSteam->m_Choices[i];
	g_pCoopSteam->JoinLobby ( c.lobbyID, c.pName );
}
