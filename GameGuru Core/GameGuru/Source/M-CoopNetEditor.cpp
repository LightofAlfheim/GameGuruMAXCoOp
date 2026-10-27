//----------------------------------------------------
//--- GAMEGURU - M-CoopNetEditor
//--- The editor side of co-op: the Co-op Settings window (Co-op menu), testing co-op on
//--- this PC, and what a standalone export needs for co-op (launcher, settings, animation sets).
//--- The settings themselves (sCoopNetSettings, load/save) live in M-CoopNet.cpp.
//----------------------------------------------------

#include "stdafx.h"
#include "gameguru.h"
#include "..\Imgui\imgui.h"
#include "..\Imgui\imgui_impl_win32.h"
#include "..\Imgui\imgui_gg_dx11.h"
#include "M-CoopNet.h"
#include <shellapi.h>
#include <string>

extern StoryboardStruct Storyboard;

const char* coopnet_projectname ( void )
{
	return Storyboard.gamename;
}

static bool g_bCoopNetEditorOpen = false;
static sCoopNetSettings g_CoopNetEditSettings;
static char g_pCoopNetEditProject[256] = "";	// project the window's settings belong to
static bool g_bCoopNetEditLoaded = false;
static char g_pCoopNetEditStatus[300] = "";
static int g_iCoopNetTestPlayers = 2;

static std::string coopnet_editor_exefolder ( void )
{
	char pPath[MAX_PATH];
	GetModuleFileNameA ( NULL, pPath, MAX_PATH );
	char* pSlash = strrchr ( pPath, '\\' );
	if ( pSlash ) *pSlash = 0;
	return pPath;
}

void coopnet_editor_open ( void )
{
	g_bCoopNetEditorOpen = true;
	g_bCoopNetEditLoaded = false;	// read the file again
}

static void coopnet_editor_save ( void )
{
	if ( coopnet_savesettings ( g_pCoopNetEditProject, &g_CoopNetEditSettings ) )
	{
		char pFile[MAX_PATH];
		coopnet_settingsfile ( g_pCoopNetEditProject, pFile, false );
		snprintf ( g_pCoopNetEditStatus, sizeof(g_pCoopNetEditStatus), "Saved to %s", pFile );
	}
	else
		strcpy ( g_pCoopNetEditStatus, "Could not save the settings file" );
}

static void coopnet_editor_help ( const char* pText )
{
	ImGui::SameLine();
	ImGui::TextDisabled ( "(?)" );
	if ( ImGui::IsItemHovered() )
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos ( ImGui::GetFontSize() * 30.0f );
		ImGui::TextUnformatted ( pText );
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

static bool coopnet_editor_launch ( const char* pArgs )
{
	// another copy of this exe, running the saved project
	std::string sFolder = coopnet_editor_exefolder();
	char pExe[MAX_PATH];
	GetModuleFileNameA ( NULL, pExe, MAX_PATH );
	char pCmd[1024];
	snprintf ( pCmd, sizeof(pCmd), "\"%s\" %s", pExe, pArgs );
	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	if ( !CreateProcessA ( NULL, pCmd, NULL, NULL, FALSE, 0, NULL, sFolder.c_str(), &si, &pi ) ) return false;
	CloseHandle ( pi.hThread );
	CloseHandle ( pi.hProcess );
	return true;
}

static void coopnet_editor_testcoop ( int iPlayers )
{
	// net flags must come before project= (the engine treats the rest of the line as the project name)
	char pArgs[600];
	snprintf ( pArgs, sizeof(pArgs), "-nethost=%d project=0%s", g_CoopNetEditSettings.iPort, g_pCoopNetEditProject );
	if ( !coopnet_editor_launch ( pArgs ) ) { strcpy ( g_pCoopNetEditStatus, "Could not start the host copy of the game" ); return; }
	for ( int i = 1; i < iPlayers; i++ )
	{
		Sleep ( 300 );
		snprintf ( pArgs, sizeof(pArgs), "-netjoin=127.0.0.1:%d project=0%s", g_CoopNetEditSettings.iPort, g_pCoopNetEditProject );
		coopnet_editor_launch ( pArgs );
	}
	snprintf ( g_pCoopNetEditStatus, sizeof(g_pCoopNetEditStatus), "Started %d copies of the game: click Play in each, then start the same level", iPlayers );
}

static void coopnet_editor_levelcheck ( void )
{
	// which co-op player characters the loaded level has
	int iFound = 0;
	for ( int iSlot = 0; iSlot < 4; iSlot++ )
	{
		int e = coopnet_findpuppetentity ( iSlot );
		int eMelee = coopnet_findpuppetentity ( iSlot + 4 );
		if ( e > 0 ) iFound++;
		ImVec4 col = e > 0 ? ImVec4 ( 0.4f, 0.9f, 0.4f, 1.0f ) : ImVec4 ( 0.9f, 0.45f, 0.4f, 1.0f );
		ImGui::TextColored ( col, "%s %s", e > 0 ? "[found]  " : "[missing]", coopnet_puppetname ( iSlot ) );
		ImGui::SameLine ( ImGui::GetFontSize() * 11.0f );
		if ( eMelee > 0 ) ImGui::TextColored ( ImVec4 ( 0.4f, 0.9f, 0.4f, 1.0f ), "+ %s", coopnet_puppetname ( iSlot + 4 ) );
		else ImGui::TextDisabled ( "no %s (optional)", coopnet_puppetname ( iSlot + 4 ) );
	}
	int iNeed = g_CoopNetEditSettings.iMaxPlayers - 1;
	if ( iFound >= iNeed )
		ImGui::TextColored ( ImVec4 ( 0.4f, 0.9f, 0.4f, 1.0f ), "This level is ready for %d players.", g_CoopNetEditSettings.iMaxPlayers );
	else
		ImGui::TextColored ( ImVec4 ( 0.95f, 0.75f, 0.3f, 1.0f ), "Add %d more Player character(s): %d players need any %d of Player1-4.",
			iNeed - iFound, g_CoopNetEditSettings.iMaxPlayers, iNeed );
	coopnet_editor_help ( "Each PC hides its own character, so N players need any N-1 of Player1-Player4. "
		"Place characters and set their Name (Object properties) to Player1, Player2... "
		"A PlayerNMelee twin (same character, melee animation set) is shown instead while that player holds a melee weapon. "
		"Players without a character in the level appear as a grey box." );
}

void coopnet_editor_window ( void )
{
	if ( !g_bCoopNetEditorOpen ) return;
	if ( !g_bCoopNetEditLoaded || strcmp ( g_pCoopNetEditProject, Storyboard.gamename ) != 0 )
	{
		strncpy ( g_pCoopNetEditProject, Storyboard.gamename, sizeof(g_pCoopNetEditProject) - 1 );
		bool bFound = coopnet_loadsettings ( g_pCoopNetEditProject, &g_CoopNetEditSettings );
		g_bCoopNetEditLoaded = true;
		strcpy ( g_pCoopNetEditStatus, bFound ? "" : "Default settings (not saved yet - any change saves them)" );
	}

	ImGui::SetNextWindowSize ( ImVec2 ( 40 * ImGui::GetFontSize(), 44 * ImGui::GetFontSize() ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowPosCenter ( ImGuiCond_FirstUseEver );
	if ( !ImGui::Begin ( "Co-op Settings##CoopNetSettings", &g_bCoopNetEditorOpen, 0 ) ) { ImGui::End(); return; }

	if ( g_pCoopNetEditProject[0] == 0 )
	{
		ImGui::TextWrapped ( "Save your game project first (File > Save) - co-op settings are stored with the project." );
		ImGui::End();
		return;
	}
	sCoopNetSettings& s = g_CoopNetEditSettings;
	bool bChanged = false;
	ImGui::TextWrapped ( "Online co-op for \"%s\". Players start the game with the Play launcher (Host co-op / Join co-op), over Steam or by address.", g_pCoopNetEditProject );
	ImGui::Separator();

	if ( ImGui::CollapsingHeader ( "Players", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		ImGui::PushItemWidth ( ImGui::GetFontSize() * 10.0f );
		if ( ImGui::SliderInt ( "Maximum players", &s.iMaxPlayers, 2, 4 ) ) bChanged = true;
		ImGui::PopItemWidth();
		coopnet_editor_help ( "Including the host. Players who try to join a full game are told so." );
		ImGui::Spacing();
		ImGui::Text ( "Player characters in this level:" );
		coopnet_editor_levelcheck();
	}

	if ( ImGui::CollapsingHeader ( "Shared gameplay", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		ImGui::TextDisabled ( "The host's choices are used by everyone in the game." );
		if ( ImGui::Checkbox ( "Share pickups (keys, quest items)", &s.bSharePickups ) ) bChanged = true;
		coopnet_editor_help ( "When one player picks up a key or other placed item, it disappears for everyone. Guns and ammo are always per player." );
		if ( ImGui::Checkbox ( "Sync doors", &s.bSyncDoors ) ) bChanged = true;
		coopnet_editor_help ( "A door opened or closed by one player opens / closes for everyone." );
		if ( ImGui::Checkbox ( "Share checkpoints", &s.bShareCheckpoints ) ) bChanged = true;
		coopnet_editor_help ( "A checkpoint reached by one player is everyone's respawn point." );
		if ( ImGui::Checkbox ( "Spectate when out of lives", &s.bSpectateOutOfLives ) ) bChanged = true;
		coopnet_editor_help ( "A player out of lives watches a teammate until everyone is out (then game over). Off: game over for that player straight away." );
		ImGui::Text ( "Level exit (win zone):" );
		ImGui::SameLine();
		int iExit = s.bExitNeedsAll ? 1 : 0;
		if ( ImGui::RadioButton ( "All players must reach it", &iExit, 1 ) ) { s.bExitNeedsAll = true; bChanged = true; }
		ImGui::SameLine();
		if ( ImGui::RadioButton ( "First player takes everyone", &iExit, 0 ) ) { s.bExitNeedsAll = false; bChanged = true; }
		coopnet_editor_help ( "When a win zone (or any script) ends the level, the whole team moves on together. All players: whoever reaches the exit waits there (\"waiting for your team 1/3\") until every player still in the game has reached it; players out of lives don't count. First player: the team goes as soon as anyone reaches it. Players who join later are sent to the host's level." );
	}

	if ( ImGui::CollapsingHeader ( "Anti-cheat" ) )
	{
		if ( ImGui::Checkbox ( "Refuse players with changed game files", &s.bFileCheck ) ) bChanged = true;
		coopnet_editor_help ( "The host compares everyone's scripts and weapon files with its own and refuses edited copies." );
		if ( ImGui::Checkbox ( "Stop co-op when a cheat tool is found", &s.bCheatCheck ) ) bChanged = true;
		coopnet_editor_help ( "Mod Menu / Cheat Engine running, or scripts changed while the game runs. Single player is never checked." );
		ImGui::TextDisabled ( "In game the host presses F7 to kick (number) or kick and block (Shift+number) a player." );
	}

	if ( ImGui::CollapsingHeader ( "Launcher and export" ) )
	{
		ImGui::PushItemWidth ( ImGui::GetFontSize() * 16.0f );
		if ( ImGui::InputText ( "Launcher title", s.pTitle, sizeof(s.pTitle) ) ) bChanged = true;
		coopnet_editor_help ( "The heading in the Play launcher. Empty: the game's name." );
		float fCol[3] = { 110 / 255.0f, 11 / 255.0f, 11 / 255.0f };
		unsigned int r, g2, b;
		if ( s.pAccent[0] == '#' && sscanf ( s.pAccent + 1, "%02x%02x%02x", &r, &g2, &b ) == 3 ) { fCol[0] = r / 255.0f; fCol[1] = g2 / 255.0f; fCol[2] = b / 255.0f; }
		if ( ImGui::ColorEdit3 ( "Button colour", fCol, ImGuiColorEditFlags_NoInputs ) )
		{
			sprintf ( s.pAccent, "#%02X%02X%02X", (int)( fCol[0] * 255 + 0.5f ), (int)( fCol[1] * 255 + 0.5f ), (int)( fCol[2] * 255 + 0.5f ) );
			bChanged = true;
		}
		if ( ImGui::InputText ( "Host address", s.pHostAddress, sizeof(s.pHostAddress) ) ) bChanged = true;
		coopnet_editor_help ( "What the launcher's 'Connect by address' box starts with (e.g. a Radmin VPN address). Empty: players type it. Not needed for Steam." );
		if ( ImGui::InputInt ( "Port (UDP)", &s.iPort ) ) { if ( s.iPort < 1 ) s.iPort = 1; if ( s.iPort > 65535 ) s.iPort = 65535; bChanged = true; }
		ImGui::PopItemWidth();
		if ( ImGui::Checkbox ( "Add co-op files when exporting the game", &s.bExportCoopFiles ) ) bChanged = true;
		coopnet_editor_help ( "Export Standalone then also writes Play.exe (the launcher), coop.ini, How to play.txt and steam_appid.txt, and includes the character animation sets the co-op players switch between." );
	}

	if ( ImGui::CollapsingHeader ( "Test co-op on this PC", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		ImGui::TextWrapped ( "Starts copies of the game next to the editor: one hosts, the others join it. They play the SAVED project, so save your level first." );
		if ( g.projectmodified != 0 ) ImGui::TextColored ( ImVec4 ( 0.95f, 0.75f, 0.3f, 1.0f ), "This level has unsaved changes - the test won't include them." );
		ImGui::PushItemWidth ( ImGui::GetFontSize() * 6.0f );
		ImGui::SliderInt ( "Players##test", &g_iCoopNetTestPlayers, 2, 4 );
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if ( ImGui::Button ( "Start co-op test" ) ) coopnet_editor_testcoop ( g_iCoopNetTestPlayers );
		ImGui::SameLine();
		if ( ImGui::Button ( "Open logs folder" ) ) ShellExecuteA ( NULL, "open", coopnet_editor_exefolder().c_str(), NULL, NULL, SW_SHOWDEFAULT );
		coopnet_editor_help ( "Each copy writes coopnet-host.log / coopnet-client.log (client2, client3...) next to GameGuruMAX.exe." );
	}

	if ( bChanged ) coopnet_editor_save();
	if ( g_pCoopNetEditStatus[0] )
	{
		ImGui::Separator();
		ImGui::TextWrapped ( "%s", g_pCoopNetEditStatus );
	}
	ImGui::End();
}

//
// Standalone export
//

void coopnet_export_addfiles ( void )
{
	// called while the export builds its file list (paths relative to Files); encrypted with the rest
	if ( Storyboard.gamename[0] == 0 ) return;
	sCoopNetSettings s;
	bool bFound = coopnet_loadsettings ( Storyboard.gamename, &s );
	char pRel[MAX_PATH];
	if ( bFound )
	{
		snprintf ( pRel, MAX_PATH, "projectbank\\%s\\coopsettings.ini", Storyboard.gamename );
		addtocollection ( pRel );
	}
	if ( !s.bExportCoopFiles ) return;
	timestampactivity ( 0, "adding co-op files (animation sets for the co-op player characters)" );

	// the animation sets co-op characters switch between (M-CoopNet.cpp coopnet_animsetfor): only for
	// the character types the Player entities in this level use (both when the level has none)
	bool bMale = false, bFemale = false;
	for ( int iSlot = 0; iSlot < COOPNET_PUPPET_SLOTS; iSlot++ )
	{
		int e = coopnet_findpuppetentity ( iSlot );
		if ( e <= 0 ) continue;
		int iBase = t.entityprofile[t.entityelement[e].bankindex].characterbasetype;
		if ( iBase == 0 ) bMale = true;
		if ( iBase == 1 ) bFemale = true;
	}
	if ( !bMale && !bFemale ) bMale = bFemale = true;
	for ( int i = 0; i < 2; i++ )
	{
		if ( ( i == 0 && !bMale ) || ( i == 1 && !bFemale ) ) continue;
		std::string sFolder = std::string ( "charactercreatorplus\\animations\\sets\\" ) + ( i == 0 ? "adult male" : "adult female" );
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA ( ( coopnet_editor_exefolder() + "\\Files\\" + sFolder + "\\default animations*.dbo" ).c_str(), &fd );
		if ( h == INVALID_HANDLE_VALUE ) continue;
		do
		{
			std::string sFile = sFolder + "\\" + fd.cFileName;
			strncpy ( pRel, sFile.c_str(), MAX_PATH - 1 ); pRel[MAX_PATH - 1] = 0;
			addtocollection ( pRel );
		} while ( FindNextFileA ( h, &fd ) );
		FindClose ( h );
	}
}

static bool coopnet_export_copy ( const std::string& sFrom, const std::string& sTo )
{
	if ( GetFileAttributesA ( sFrom.c_str() ) == INVALID_FILE_ATTRIBUTES ) return false;
	return CopyFileA ( sFrom.c_str(), sTo.c_str(), FALSE ) != 0;
}

void coopnet_export_copyextras ( const char* pExportFolder, const char* pExeName )
{
	// next to the exported exe: the Play launcher (kept in <MAX folder>\coop\), its coop.ini from
	// Co-op Settings, How to play.txt, and steam_appid.txt so Steam co-op works outside Steam
	if ( Storyboard.gamename[0] == 0 ) return;
	sCoopNetSettings s;
	coopnet_loadsettings ( Storyboard.gamename, &s );
	if ( !s.bExportCoopFiles ) return;
	std::string sDest = pExportFolder;
	std::string sKit = coopnet_editor_exefolder() + "\\coop\\";
	char pLog[MAX_PATH + 100];
	if ( !coopnet_export_copy ( sKit + "Play.exe", sDest + "\\Play.exe" ) )
	{
		snprintf ( pLog, sizeof(pLog), "co-op export: no launcher at %sPlay.exe - copy CoopTest\\launcher\\Play.exe there", sKit.c_str() );
		timestampactivity ( 0, pLog );
	}
	coopnet_export_copy ( sKit + "How to play.txt", sDest + "\\How to play.txt" );

	FILE* f = fopen ( ( sDest + "\\coop.ini" ).c_str(), "w" );
	if ( f )
	{
		fprintf ( f, "; Launcher settings (from the editor's Co-op Settings - change them there and export again)\n" );
		fprintf ( f, "title=%s\n", s.pTitle[0] ? s.pTitle : Storyboard.gamename );
		fprintf ( f, "accent=%s\n", s.pAccent );
		fprintf ( f, "host=%s\n", s.pHostAddress );
		fprintf ( f, "port=%d\n", s.iPort );
		fprintf ( f, "game=%s.exe\n", pExeName );
		fclose ( f );
	}
	f = fopen ( ( sDest + "\\steam_appid.txt" ).c_str(), "w" );
	if ( f ) { fprintf ( f, "480\n" ); fclose ( f ); }
	timestampactivity ( 0, "co-op export: launcher, coop.ini, How to play.txt, steam_appid.txt written" );
}
