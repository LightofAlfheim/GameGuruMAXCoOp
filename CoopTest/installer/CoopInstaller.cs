// Installer for the co-op mod: adds online co-op to a GameGuru MAX the user already owns on Steam.
// It ships only what the mod changes - the co-op GameGuruMAX.exe and the launcher kit (coop\Play.exe,
// coop\How to play.txt) - never the rest of GameGuru MAX.
//   Install:   GameGuruMAX.exe -> GameGuruMAX-original.exe (backup), co-op exe in its place, coop\ kit.
//              The engine exports and relaunches "GameGuruMAX.exe" by name, so the co-op exe must
//              have that name. Steam updates / "Verify integrity" put the Steam exe back: run again.
//   Uninstall: the backup goes back to GameGuruMAX.exe, coop\ and the marker file are removed.
// Embedded at build time (build-installer.bat): coop.exe, coop.play, coop.howto, coop.version
// (the version.ini line of the GameGuru MAX build the exe was made for).
// Command line (for testing / scripting): /path=<GameGuru MAX folder> /install | /uninstall [/quiet]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

static class CoopInstaller
{
	public const string ExeName = "GameGuruMAX.exe";
	public const string BackupName = "GameGuruMAX-original.exe";
	public const string MarkerName = "coop-mod.txt";
	public static Color Accent = Color.FromArgb ( 110, 11, 11 );	// dark blood red, as the launcher

	[STAThread]
	static int Main ( string[] args )
	{
		string sPath = null, sAction = null;
		bool bQuiet = false;
		foreach ( string a in args )
		{
			if ( a.StartsWith ( "/path=", StringComparison.OrdinalIgnoreCase ) ) sPath = a.Substring ( 6 ).Trim ( '"' );
			else if ( a.Equals ( "/install", StringComparison.OrdinalIgnoreCase ) ) sAction = "install";
			else if ( a.Equals ( "/uninstall", StringComparison.OrdinalIgnoreCase ) ) sAction = "uninstall";
			else if ( a.Equals ( "/quiet", StringComparison.OrdinalIgnoreCase ) ) bQuiet = true;
		}
		if ( bQuiet && sPath != null && sAction != null )
		{
			// no window: result on stdout and in the exit code (0 = done)
			string sResult;
			bool bOk = sAction == "install" ? Install ( sPath, out sResult ) : Uninstall ( sPath, out sResult );
			Console.WriteLine ( sResult );
			return bOk ? 0 : 1;
		}
		Application.EnableVisualStyles();
		Application.Run ( new InstallerForm ( sPath, sAction ) );
		return 0;
	}

	// ---- embedded files ----

	static Stream Resource ( string sName )
	{
		return Assembly.GetExecutingAssembly().GetManifestResourceStream ( sName );
	}

	public static string ModVersion ()
	{
		// "GameGuru MAX Build 13-08-2026-20-36" - the GameGuru MAX build the co-op exe was made for
		using ( Stream s = Resource ( "coop.version" ) )
		{
			if ( s == null ) return "";
			using ( StreamReader r = new StreamReader ( s ) ) return r.ReadToEnd().Trim();
		}
	}

	static void Extract ( string sName, string sDest )
	{
		using ( Stream s = Resource ( sName ) )
		{
			if ( s == null ) throw new Exception ( "this installer is missing " + sName + " - rebuild it with build-installer.bat" );
			string sTemp = sDest + ".new";
			using ( FileStream f = File.Create ( sTemp ) ) s.CopyTo ( f );
			if ( File.Exists ( sDest ) ) File.Delete ( sDest );
			File.Move ( sTemp, sDest );
		}
	}

	static string Hash ( Stream s )
	{
		using ( SHA256 sha = SHA256.Create() ) return BitConverter.ToString ( sha.ComputeHash ( s ) );
	}

	static string FileHash ( string sFile )
	{
		using ( FileStream f = File.OpenRead ( sFile ) ) return Hash ( f );
	}

	static string ResourceHash ( string sName )
	{
		using ( Stream s = Resource ( sName ) ) return s == null ? "" : Hash ( s );
	}

	// ---- finding GameGuru MAX ----

	public static string FindSteamInstall ()
	{
		// Steam library folders (libraryfolders.vdf) -> the one with GameGuru MAX's app manifest (1247290)
		string sSteam = null;
		try
		{
			using ( RegistryKey k = Registry.CurrentUser.OpenSubKey ( @"Software\Valve\Steam" ) )
				if ( k != null ) sSteam = k.GetValue ( "SteamPath" ) as string;
		}
		catch { }
		if ( string.IsNullOrEmpty ( sSteam ) ) return null;
		sSteam = sSteam.Replace ( '/', '\\' );
		List<string> libs = new List<string>();
		libs.Add ( sSteam );
		try
		{
			string sVdf = File.ReadAllText ( Path.Combine ( sSteam, @"steamapps\libraryfolders.vdf" ) );
			foreach ( Match m in Regex.Matches ( sVdf, "\"path\"\\s+\"([^\"]+)\"" ) )
				libs.Add ( m.Groups[1].Value.Replace ( @"\\", @"\" ) );
		}
		catch { }
		foreach ( string sLib in libs )
		{
			try
			{
				string sAcf = Path.Combine ( sLib, @"steamapps\appmanifest_1247290.acf" );
				if ( !File.Exists ( sAcf ) ) continue;
				Match m = Regex.Match ( File.ReadAllText ( sAcf ), "\"installdir\"\\s+\"([^\"]+)\"" );
				string sDir = Path.Combine ( sLib, @"steamapps\common", m.Success ? m.Groups[1].Value : "GameGuru MAX" );
				if ( File.Exists ( Path.Combine ( sDir, ExeName ) ) ) return sDir;
			}
			catch { }
		}
		return null;
	}

	public static string InstalledVersion ( string sDir )
	{
		try { return File.ReadAllLines ( Path.Combine ( sDir, "version.ini" ) )[0].Trim(); }
		catch { return ""; }
	}

	static string MarkerValue ( string sDir, string sKey )
	{
		try
		{
			foreach ( string l in File.ReadAllLines ( Path.Combine ( sDir, MarkerName ) ) )
				if ( l.StartsWith ( sKey + ":" ) ) return l.Substring ( sKey.Length + 1 ).Trim();
		}
		catch { }
		return "";
	}

	public static bool IsInstalled ( string sDir )
	{
		return File.Exists ( Path.Combine ( sDir, MarkerName ) ) && File.Exists ( Path.Combine ( sDir, BackupName ) );
	}

	public static string CheckFolder ( string sDir )
	{
		// null = fine to install into; else why not
		if ( string.IsNullOrEmpty ( sDir ) || !Directory.Exists ( sDir ) ) return "Pick your GameGuru MAX folder.";
		if ( !File.Exists ( Path.Combine ( sDir, ExeName ) ) ) return "There is no " + ExeName + " in that folder.";
		if ( !File.Exists ( Path.Combine ( sDir, "version.ini" ) ) || !Directory.Exists ( Path.Combine ( sDir, "Files" ) ) )
			return "That doesn't look like a GameGuru MAX folder (no version.ini / Files).";
		return null;
	}

	static bool EditorRunning ( string sDir )
	{
		string sExe = Path.GetFullPath ( Path.Combine ( sDir, ExeName ) );
		foreach ( Process p in Process.GetProcessesByName ( "GameGuruMAX" ) )
		{
			try { if ( string.Equals ( Path.GetFullPath ( p.MainModule.FileName ), sExe, StringComparison.OrdinalIgnoreCase ) ) return true; }
			catch { return true; }	// can't tell (other user / elevated): assume it's ours
		}
		return false;
	}

	// ---- install / uninstall ----

	public static bool Install ( string sDir, out string sResult )
	{
		try
		{
			string sProblem = CheckFolder ( sDir );
			if ( sProblem != null ) { sResult = sProblem; return false; }
			if ( EditorRunning ( sDir ) ) { sResult = "Close GameGuru MAX first."; return false; }
			string sExe = Path.Combine ( sDir, ExeName );
			string sBackup = Path.Combine ( sDir, BackupName );

			// back up the Steam exe, unless a co-op exe (this one or the one an earlier install recorded in
			// the marker) is there - then the old backup stays. Any other exe is Steam's (new or updated).
			string sNow = FileHash ( sExe ), sCoop = ResourceHash ( "coop.exe" );
			bool bCoopThere = sNow == sCoop || sNow == MarkerValue ( sDir, "exe-sha256" );
			if ( !bCoopThere )
			{
				if ( File.Exists ( sBackup ) ) File.Delete ( sBackup );
				File.Copy ( sExe, sBackup );
			}
			else if ( !File.Exists ( sBackup ) )
			{
				sResult = "The co-op exe is already in place but " + BackupName + " is missing. Use Steam's \"Verify integrity of game files\" to get the original back, then install again.";
				return false;
			}
			Extract ( "coop.exe", sExe );
			string sKit = Path.Combine ( sDir, "coop" );
			Directory.CreateDirectory ( sKit );
			Extract ( "coop.play", Path.Combine ( sKit, "Play.exe" ) );
			Extract ( "coop.howto", Path.Combine ( sKit, "How to play.txt" ) );
			File.WriteAllText ( Path.Combine ( sDir, MarkerName ),
				"GameGuru MAX co-op mod\r\n" +
				"made for: " + ModVersion() + "\r\n" +
				"installed: " + DateTime.Now.ToString ( "yyyy-MM-dd HH:mm" ) + "\r\n" +
				"exe-sha256: " + sCoop + "\r\n" +
				"The Steam exe is backed up as " + BackupName + ". Run the installer again and click Uninstall to put it back.\r\n" );
			sResult = "Co-op installed. Start GameGuru MAX as usual - the editor now has a Co-op menu.";
			string sHave = InstalledVersion ( sDir ), sWant = ModVersion();
			if ( sWant.Length > 0 && !string.Equals ( sHave, sWant, StringComparison.OrdinalIgnoreCase ) )
				sResult += "\r\n\r\nNote: this mod was made for \"" + sWant + "\" but you have \"" + sHave + "\". If the editor misbehaves, uninstall and get a co-op installer for your version.";
			return true;
		}
		catch ( UnauthorizedAccessException ) { sResult = "No permission to write there - run the installer as administrator."; return false; }
		catch ( Exception e ) { sResult = "Install failed: " + e.Message; return false; }
	}

	public static bool Uninstall ( string sDir, out string sResult )
	{
		try
		{
			string sExe = Path.Combine ( sDir, ExeName );
			string sBackup = Path.Combine ( sDir, BackupName );
			if ( !File.Exists ( sBackup ) ) { sResult = "Co-op isn't installed there (no " + BackupName + ")."; return false; }
			if ( EditorRunning ( sDir ) ) { sResult = "Close GameGuru MAX first."; return false; }
			if ( File.Exists ( sExe ) ) File.Delete ( sExe );
			File.Move ( sBackup, sExe );
			string sKit = Path.Combine ( sDir, "coop" );
			foreach ( string f in new string[] { "Play.exe", "How to play.txt" } )
				if ( File.Exists ( Path.Combine ( sKit, f ) ) ) File.Delete ( Path.Combine ( sKit, f ) );
			if ( Directory.Exists ( sKit ) && Directory.GetFileSystemEntries ( sKit ).Length == 0 ) Directory.Delete ( sKit );
			if ( File.Exists ( Path.Combine ( sDir, MarkerName ) ) ) File.Delete ( Path.Combine ( sDir, MarkerName ) );
			sResult = "Co-op removed. GameGuru MAX is back to the Steam version.";
			return true;
		}
		catch ( UnauthorizedAccessException ) { sResult = "No permission to write there - run the installer as administrator."; return false; }
		catch ( Exception e ) { sResult = "Uninstall failed: " + e.Message; return false; }
	}
}

class InstallerForm : Form
{
	TextBox m_Path;
	Button m_Install, m_Uninstall;
	Label m_Status;

	public InstallerForm ( string sPath, string sAction )
	{
		Text = "GameGuru MAX Co-op Setup";
		FormBorderStyle = FormBorderStyle.FixedDialog;
		MaximizeBox = false;
		StartPosition = FormStartPosition.CenterScreen;
		AutoScaleMode = AutoScaleMode.Dpi;
		BackColor = Color.FromArgb ( 24, 22, 22 );
		ForeColor = Color.Gainsboro;
		Font = new Font ( "Segoe UI", 9.5f );
		ClientSize = new Size ( 560, 300 );
		try { Icon = Icon.ExtractAssociatedIcon ( Application.ExecutablePath ); } catch { }

		Label title = new Label { Text = "GAMEGURU MAX CO-OP", Font = new Font ( "Segoe UI", 16f, FontStyle.Bold ), ForeColor = Color.White, AutoSize = true, Location = new Point ( 20, 16 ) };
		Label intro = new Label
		{
			Text = "Adds 2-4 player online co-op to your GameGuru MAX (Steam). Only the editor exe is swapped; " +
				"the original is kept as " + CoopInstaller.BackupName + ". Made for " + CoopInstaller.ModVersion() + ".",
			Location = new Point ( 22, 54 ), Size = new Size ( 516, 44 )
		};
		Label where = new Label { Text = "GameGuru MAX folder:", AutoSize = true, Location = new Point ( 22, 108 ) };
		m_Path = new TextBox { Location = new Point ( 22, 130 ), Size = new Size ( 420, 26 ), BackColor = Color.FromArgb ( 40, 36, 36 ), ForeColor = Color.White, BorderStyle = BorderStyle.FixedSingle };
		Button browse = MakeButton ( "Browse...", new Point ( 450, 128 ), new Size ( 88, 28 ), false );
		m_Install = MakeButton ( "Install co-op", new Point ( 22, 172 ), new Size ( 160, 38 ), true );
		m_Uninstall = MakeButton ( "Uninstall", new Point ( 192, 172 ), new Size ( 110, 38 ), false );
		m_Status = new Label { Location = new Point ( 22, 222 ), Size = new Size ( 516, 70 ) };
		Controls.AddRange ( new Control[] { title, intro, where, m_Path, browse, m_Install, m_Uninstall, m_Status } );

		browse.Click += delegate
		{
			using ( FolderBrowserDialog d = new FolderBrowserDialog() )
			{
				d.Description = "Pick the folder that has GameGuruMAX.exe (Steam: right-click GameGuru MAX > Manage > Browse local files)";
				if ( Directory.Exists ( m_Path.Text ) ) d.SelectedPath = m_Path.Text;
				if ( d.ShowDialog ( this ) == DialogResult.OK ) m_Path.Text = d.SelectedPath;
			}
		};
		m_Path.TextChanged += delegate { Refresh_(); };
		m_Install.Click += delegate { Run ( "install" ); };
		m_Uninstall.Click += delegate { Run ( "uninstall" ); };

		m_Path.Text = sPath ?? CoopInstaller.FindSteamInstall() ?? "";
		Refresh_();
		if ( sAction != null ) Shown += delegate { Run ( sAction ); };	// relaunched as administrator
	}

	Button MakeButton ( string sText, Point at, Size size, bool bPrimary )
	{
		Button b = new Button { Text = sText, Location = at, Size = size, FlatStyle = FlatStyle.Flat, ForeColor = Color.White, Cursor = Cursors.Hand };
		b.BackColor = bPrimary ? CoopInstaller.Accent : Color.FromArgb ( 48, 44, 44 );
		b.FlatAppearance.BorderColor = bPrimary ? ControlPaint.Light ( CoopInstaller.Accent ) : Color.FromArgb ( 80, 72, 72 );
		return b;
	}

	void Refresh_ ()
	{
		string sDir = m_Path.Text.Trim();
		string sProblem = CoopInstaller.CheckFolder ( sDir );
		bool bInstalled = sProblem == null && CoopInstaller.IsInstalled ( sDir );
		m_Install.Enabled = sProblem == null;
		m_Install.Text = bInstalled ? "Reinstall co-op" : "Install co-op";
		m_Uninstall.Enabled = bInstalled;
		if ( sProblem != null ) m_Status.Text = sProblem;
		else m_Status.Text = ( bInstalled ? "Co-op is installed here. " : "" ) + "Found " + CoopInstaller.InstalledVersion ( sDir ) + ".";
	}

	void Run ( string sAction )
	{
		string sDir = m_Path.Text.Trim();
		string sResult;
		bool bOk = sAction == "install" ? CoopInstaller.Install ( sDir, out sResult ) : CoopInstaller.Uninstall ( sDir, out sResult );
		if ( !bOk && sResult.Contains ( "administrator" ) && !IsAdmin() )
		{
			// e.g. Steam in Program Files: ask Windows for permission and do it again from there
			try
			{
				Process.Start ( new ProcessStartInfo ( Application.ExecutablePath, "\"/path=" + sDir + "\" /" + sAction ) { Verb = "runas", UseShellExecute = true } );
				Close();
				return;
			}
			catch { }	// declined: show the message
		}
		Refresh_();
		m_Status.Text = sResult;
		m_Status.ForeColor = bOk ? Color.FromArgb ( 140, 220, 140 ) : Color.FromArgb ( 240, 120, 120 );
	}

	static bool IsAdmin ()
	{
		return new System.Security.Principal.WindowsPrincipal ( System.Security.Principal.WindowsIdentity.GetCurrent() ).IsInRole ( System.Security.Principal.WindowsBuiltInRole.Administrator );
	}
}
