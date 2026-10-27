// Launcher for an exported GameGuru MAX game: single player and co-op from one window.
// Sits next to the game's .exe (built as "Play.exe" by default) so players never type
// command lines:
//   PLAY         single player (no flags)
//   HOST CO-OP   over Steam (-netsteam=host): friends join from their launcher's friend list
//   JOIN CO-OP   over Steam (-netsteam=join): lists Steam friends who are hosting, click one
//   "Connect by address" switches to the direct IP options (-nethost / -netjoin=, e.g. over a VPN).
// Look: background and logo images embedded at build time (build-launcher.bat), drawn by hand
// (borderless window, owner-drawn buttons) so it matches the game rather than Windows.
// Settings (optional) in coop.ini next to it:
//   title=Name      window/taskbar title, and the heading when there is no logo (default: exe name)
//   accent=#6E0B0B  button colour
//   host=address    the address box starts with this (IP or host name, optionally :port)
//   port=27015      UDP port for the direct IP options
//   game=Name.exe   which exe to start (default: the largest other .exe in the folder)
// The last address used is remembered per user (%LOCALAPPDATA%\GameGuruCoop\<game>.ini),
// until the host= line in coop.ini changes.
// Build: build-launcher.bat (uses the C# compiler that ships with Windows, .NET Framework 4).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows.Forms;

static class CoopLauncher
{
	const int DefaultPort = 27015;

	static string g_Folder;
	static string g_GameExe;
	static string g_IniHost = "";
	static int g_Port = DefaultPort;
	static string g_UserIni;
	public static string Title;
	public static Color Accent = Color.FromArgb ( 110, 11, 11 );	// dark blood red

	[STAThread]
	static void Main ()
	{
		Application.EnableVisualStyles();
		Application.SetCompatibleTextRenderingDefault ( false );
		g_Folder = AppDomain.CurrentDomain.BaseDirectory;
		Dictionary<string, string> ini = ReadIni ( Path.Combine ( g_Folder, "coop.ini" ) );
		string v;
		if ( ini.TryGetValue ( "host", out v ) ) g_IniHost = v;
		if ( ini.TryGetValue ( "port", out v ) ) { int p; if ( int.TryParse ( v, out p ) && p > 0 && p < 65536 ) g_Port = p; }
		if ( ini.TryGetValue ( "accent", out v ) && v.Length > 0 ) { try { Accent = ColorTranslator.FromHtml ( v ); } catch { } }
		ini.TryGetValue ( "game", out v );
		g_GameExe = FindGameExe ( v );
		if ( g_GameExe == null )
		{
			MessageBox.Show ( "Could not find the game next to this launcher.\n\nKeep the launcher in the same folder as the game's .exe.",
				"Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error );
			return;
		}
		string sGame = Path.GetFileNameWithoutExtension ( g_GameExe );
		Title = ini.TryGetValue ( "title", out v ) && v.Length > 0 ? v : sGame;
		g_UserIni = Path.Combine ( Environment.GetFolderPath ( Environment.SpecialFolder.LocalApplicationData ), "GameGuruCoop", MakeSafe ( sGame ) + ".ini" );
		Application.Run ( new LauncherForm() );
	}

	static string MakeSafe ( string s )
	{
		foreach ( char c in Path.GetInvalidFileNameChars() ) s = s.Replace ( c, '_' );
		return s;
	}

	static Dictionary<string, string> ReadIni ( string sPath )
	{
		Dictionary<string, string> d = new Dictionary<string, string> ( StringComparer.OrdinalIgnoreCase );
		try
		{
			if ( !File.Exists ( sPath ) ) return d;
			foreach ( string sRaw in File.ReadAllLines ( sPath ) )
			{
				string s = sRaw.Trim();
				if ( s.Length == 0 || s[0] == ';' || s[0] == '#' || s[0] == '[' ) continue;
				int i = s.IndexOf ( '=' );
				if ( i <= 0 ) continue;
				d[s.Substring ( 0, i ).Trim()] = s.Substring ( i + 1 ).Trim();
			}
		}
		catch { }
		return d;
	}

	static string FindGameExe ( string sConfigured )
	{
		if ( !string.IsNullOrEmpty ( sConfigured ) )
		{
			string s = Path.Combine ( g_Folder, sConfigured );
			return File.Exists ( s ) ? s : null;
		}
		string sSelf = Path.GetFileName ( Application.ExecutablePath );
		string sFound = null;
		foreach ( string s in Directory.GetFiles ( g_Folder, "*.exe" ) )
		{
			string n = Path.GetFileName ( s );
			if ( string.Equals ( n, sSelf, StringComparison.OrdinalIgnoreCase ) ) continue;
			if ( string.Equals ( n, "ffmpeg.exe", StringComparison.OrdinalIgnoreCase ) ) continue;
			if ( string.Equals ( n, "Guru-Converter.exe", StringComparison.OrdinalIgnoreCase ) ) continue;
			if ( string.Equals ( n, "Play Co-op.exe", StringComparison.OrdinalIgnoreCase ) ) continue;	// the older co-op-only launcher
			if ( sFound == null || new FileInfo ( s ).Length > new FileInfo ( sFound ).Length ) sFound = s;	// the engine is the big one
		}
		return sFound;
	}

	public static string StartAddress ()
	{
		// the remembered address, unless the host= line has changed since it was saved
		Dictionary<string, string> u = ReadIni ( g_UserIni );
		string sLast, sBasedOn;
		if ( u.TryGetValue ( "last", out sLast ) && sLast.Length > 0 )
		{
			if ( !u.TryGetValue ( "inihost", out sBasedOn ) ) sBasedOn = "";
			if ( sBasedOn == g_IniHost ) return sLast;
		}
		return g_IniHost;
	}

	public static void RememberAddress ( string sAddress )
	{
		try
		{
			Directory.CreateDirectory ( Path.GetDirectoryName ( g_UserIni ) );
			File.WriteAllLines ( g_UserIni, new string[] { "last=" + sAddress, "inihost=" + g_IniHost } );
		}
		catch { }
	}

	public static string LocalAddresses ()
	{
		List<string> list = new List<string>();
		try
		{
			foreach ( NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces() )
			{
				if ( ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback ) continue;
				foreach ( UnicastIPAddressInformation a in ni.GetIPProperties().UnicastAddresses )
				{
					if ( a.Address.AddressFamily != AddressFamily.InterNetwork ) continue;
					string s = a.Address.ToString();
					if ( s.StartsWith ( "169.254." ) ) continue;
					if ( !list.Contains ( s ) ) list.Add ( s );
				}
			}
		}
		catch { }
		return list.Count > 0 ? string.Join ( ",  ", list.ToArray() ) : "(no network found)";
	}

	public static bool SteamRunning ()
	{
		try { return Process.GetProcessesByName ( "steam" ).Length > 0; }
		catch { return true; }	// can't tell: let the game report it
	}

	public static bool Launch ( string sArgs, out string sError )
	{
		sError = null;
		try
		{
			ProcessStartInfo psi = new ProcessStartInfo ( g_GameExe, sArgs );
			psi.WorkingDirectory = Path.GetDirectoryName ( g_GameExe );
			psi.UseShellExecute = false;
			Process.Start ( psi );
			return true;
		}
		catch ( Exception ex ) { sError = ex.Message; return false; }
	}

	public static string JoinArgs ( string sAddress )
	{
		// -netjoin=host[:port]; add the configured port when none was typed
		if ( sAddress.IndexOf ( ':' ) < 0 && g_Port != DefaultPort ) sAddress += ":" + g_Port;
		return "-netjoin=" + sAddress;
	}

	public static string HostArgs ()
	{
		return g_Port == DefaultPort ? "-nethost" : "-nethost=" + g_Port;
	}

	public static int Port { get { return g_Port; } }
	public static string GameExe { get { return g_GameExe; } }

	public static Image LoadEmbedded ( string sName )
	{
		// background / logo baked in by build-launcher.bat (/resource:...)
		try
		{
			Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream ( sName );
			return s != null ? Image.FromStream ( s ) : null;
		}
		catch { return null; }
	}
}

// Steam, straight from the steam_api64.dll that ships next to the game (its plain-C "flat"
// exports), just enough to list Steam friends who are hosting co-op: friends playing the same
// App ID whose lobby carries the game's "edcoop" tag (see M-CoopNetSteam.cpp).
static class SteamFriendsList
{
	const string DLL = "steam_api64.dll";
	const int k_EFriendFlagImmediate = 4;

	[StructLayout ( LayoutKind.Sequential, Pack = 8 )]
	struct FriendGameInfo { public ulong gameID; public uint gameIP; public ushort gamePort, queryPort; public ulong lobbyID; }

	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] [return: MarshalAs ( UnmanagedType.I1 )] static extern bool SteamAPI_Init ();
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern void SteamAPI_Shutdown ();
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern void SteamAPI_RunCallbacks ();
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern IntPtr SteamFriends ();
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern IntPtr SteamMatchmaking ();
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern IntPtr SteamUtils ();
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern int SteamAPI_ISteamFriends_GetFriendCount ( IntPtr self, int flags );
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern ulong SteamAPI_ISteamFriends_GetFriendByIndex ( IntPtr self, int index, int flags );
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern IntPtr SteamAPI_ISteamFriends_GetFriendPersonaName ( IntPtr self, ulong friendID );
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] [return: MarshalAs ( UnmanagedType.I1 )] static extern bool SteamAPI_ISteamFriends_GetFriendGamePlayed ( IntPtr self, ulong friendID, out FriendGameInfo info );
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] [return: MarshalAs ( UnmanagedType.I1 )] static extern bool SteamAPI_ISteamMatchmaking_RequestLobbyData ( IntPtr self, ulong lobbyID );
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern IntPtr SteamAPI_ISteamMatchmaking_GetLobbyData ( IntPtr self, ulong lobbyID, [MarshalAs ( UnmanagedType.LPStr )] string key );
	[DllImport ( DLL, CallingConvention = CallingConvention.Cdecl )] static extern uint SteamAPI_ISteamUtils_GetAppID ( IntPtr self );

	public class Host { public ulong Lobby; public string Name; }

	static bool s_bReady;
	static HashSet<ulong> s_Asked = new HashSet<ulong>();

	static string Utf8 ( IntPtr p )
	{
		if ( p == IntPtr.Zero ) return "";
		List<byte> b = new List<byte>();
		for ( int i = 0; ; i++ ) { byte c = Marshal.ReadByte ( p, i ); if ( c == 0 ) break; b.Add ( c ); }
		return System.Text.Encoding.UTF8.GetString ( b.ToArray() );
	}

	// starts Steam for the launcher with the game's App ID (steam_appid.txt, else 480 - as the game does)
	public static bool Start ( string sGameFolder, out string sError )
	{
		sError = null;
		if ( s_bReady ) return true;
		try
		{
			Directory.SetCurrentDirectory ( sGameFolder );
			string sFile = Path.Combine ( sGameFolder, "steam_appid.txt" );
			if ( !File.Exists ( sFile ) && string.IsNullOrEmpty ( Environment.GetEnvironmentVariable ( "SteamAppId" ) ) )
			{
				Environment.SetEnvironmentVariable ( "SteamAppId", "480" );
				Environment.SetEnvironmentVariable ( "SteamGameId", "480" );
			}
			s_bReady = SteamAPI_Init();
			if ( !s_bReady && !File.Exists ( sFile ) ) { File.WriteAllText ( sFile, "480" ); s_bReady = SteamAPI_Init(); }
			if ( !s_bReady ) sError = "Couldn't connect to Steam. Make sure Steam is running and you're signed in.";
		}
		catch ( Exception ex ) { sError = "Couldn't use Steam (" + ex.Message + ")."; s_bReady = false; }
		return s_bReady;
	}

	public static void Stop ()
	{
		if ( !s_bReady ) return;
		try { SteamAPI_Shutdown(); } catch { }
		s_bReady = false;
	}

	public static List<Host> Hosting ()
	{
		// friends in a co-op lobby of this game, one entry per lobby (named after its host)
		List<Host> list = new List<Host>();
		if ( !s_bReady ) return list;
		SteamAPI_RunCallbacks();
		IntPtr friends = SteamFriends(), mm = SteamMatchmaking();
		uint uApp = SteamAPI_ISteamUtils_GetAppID ( SteamUtils() );
		int n = SteamAPI_ISteamFriends_GetFriendCount ( friends, k_EFriendFlagImmediate );
		for ( int i = 0; i < n; i++ )
		{
			ulong f = SteamAPI_ISteamFriends_GetFriendByIndex ( friends, i, k_EFriendFlagImmediate );
			FriendGameInfo info;
			if ( !SteamAPI_ISteamFriends_GetFriendGamePlayed ( friends, f, out info ) ) continue;
			if ( ( info.gameID & 0xFFFFFF ) != uApp || info.lobbyID == 0 ) continue;	// CGameID: app id in the low 24 bits
			string sTag = Utf8 ( SteamAPI_ISteamMatchmaking_GetLobbyData ( mm, info.lobbyID, "edcoop" ) );
			if ( sTag.Length == 0 )
			{
				// not known yet: ask Steam once; it shows up on a later refresh
				if ( s_Asked.Add ( info.lobbyID ) ) SteamAPI_ISteamMatchmaking_RequestLobbyData ( mm, info.lobbyID );
				continue;
			}
			if ( sTag != "1" ) continue;
			bool bDup = false;
			foreach ( Host h in list ) if ( h.Lobby == info.lobbyID ) bDup = true;
			if ( bDup ) continue;
			string sHost = Utf8 ( SteamAPI_ISteamMatchmaking_GetLobbyData ( mm, info.lobbyID, "host" ) );
			Host e = new Host();
			e.Lobby = info.lobbyID;
			e.Name = sHost.Length > 0 ? sHost : Utf8 ( SteamAPI_ISteamFriends_GetFriendPersonaName ( friends, f ) );
			list.Add ( e );
		}
		return list;
	}
}

// everything is laid out on this 760 x 460 grid and scaled to the screen's DPI
class LauncherForm : Form
{
	const int BaseW = 760, BaseH = 460;

	class Item
	{
		public string Name, Title, Sub;
		public RectangleF Rect;			// base units
		public bool Primary, Link, Small;
		public Action Click;
	}

	float m_Scale = 1f;
	Image m_Background, m_Logo;
	Bitmap m_Backdrop;					// background + darkening, rendered once at window size
	List<Item> m_Items = new List<Item>();
	Item m_Hover, m_Pressed;
	bool m_AddressMode;
	bool m_FriendsMode;					// "join a friend" screen: Steam friends hosting, refreshed by m_FriendsTimer
	List<SteamFriendsList.Host> m_Hosts = new List<SteamFriendsList.Host>();
	Timer m_FriendsTimer;
	string m_Status = "";
	bool m_SteamRunning;
	TextBox m_Address;
	Timer m_SteamTimer;
	string m_TitleFamily;

	[DllImport ( "user32.dll" )] static extern bool ReleaseCapture ();
	[DllImport ( "user32.dll" )] static extern IntPtr SendMessage ( IntPtr h, int msg, IntPtr w, IntPtr l );

	public LauncherForm ()
	{
		Text = CoopLauncher.Title;
		FormBorderStyle = FormBorderStyle.None;
		StartPosition = FormStartPosition.CenterScreen;
		DoubleBuffered = true;
		KeyPreview = true;
		BackColor = Color.Black;
		using ( Graphics g = CreateGraphics() ) m_Scale = g.DpiX / 96f;
		ClientSize = new Size ( S ( BaseW ), S ( BaseH ) );
		try { Icon = Icon.ExtractAssociatedIcon ( Application.ExecutablePath ); } catch { }
		m_Background = CoopLauncher.LoadEmbedded ( "launcher.background" );
		m_Logo = CoopLauncher.LoadEmbedded ( "launcher.logo" );
		m_TitleFamily = PickFamily ( new string[] { "Bahnschrift SemiBold Condensed", "Bahnschrift", "Segoe UI Semibold" } );
		BuildBackdrop();

		m_Address = new TextBox();
		m_Address.Name = "address";
		m_Address.BorderStyle = BorderStyle.FixedSingle;
		m_Address.BackColor = Color.FromArgb ( 24, 16, 16 );
		m_Address.ForeColor = Color.White;
		m_Address.Font = new Font ( "Segoe UI", 12f );
		m_Address.Text = CoopLauncher.StartAddress();
		m_Address.Visible = false;
		m_Address.SetBounds ( S ( 210 ), S ( 222 ), S ( 340 ), S ( 30 ) );
		m_Address.KeyDown += delegate ( object o, KeyEventArgs e ) { if ( e.KeyCode == Keys.Enter ) { e.SuppressKeyPress = true; JoinAddress(); } };
		Controls.Add ( m_Address );

		m_SteamRunning = CoopLauncher.SteamRunning();
		m_SteamTimer = new Timer();
		m_SteamTimer.Interval = 2000;
		m_SteamTimer.Tick += delegate { bool b = CoopLauncher.SteamRunning(); if ( b != m_SteamRunning ) { m_SteamRunning = b; Invalidate(); } };
		m_SteamTimer.Start();

		m_FriendsTimer = new Timer();
		m_FriendsTimer.Interval = 1500;
		m_FriendsTimer.Tick += delegate { if ( m_FriendsMode ) RefreshFriends ( false ); };
		FormClosed += delegate { SteamFriendsList.Stop(); };

		ShowMenu();
	}

	int S ( float f ) { return (int)Math.Round ( f * m_Scale ); }
	RectangleF SR ( RectangleF r ) { return new RectangleF ( r.X * m_Scale, r.Y * m_Scale, r.Width * m_Scale, r.Height * m_Scale ); }

	static string PickFamily ( string[] names )
	{
		using ( InstalledFontCollection fonts = new InstalledFontCollection() )
			foreach ( string n in names )
				foreach ( FontFamily f in fonts.Families )
					if ( string.Equals ( f.Name, n, StringComparison.OrdinalIgnoreCase ) ) return f.Name;
		return "Segoe UI";
	}

	Font TitleFont ( float fPt ) { return new Font ( m_TitleFamily, fPt * m_Scale, FontStyle.Regular, GraphicsUnit.Point ); }
	Font UIFont ( float fPt ) { return new Font ( "Segoe UI", fPt * m_Scale, FontStyle.Regular, GraphicsUnit.Point ); }

	void BuildBackdrop ()
	{
		int w = ClientSize.Width, h = ClientSize.Height;
		m_Backdrop = new Bitmap ( w, h );
		using ( Graphics g = Graphics.FromImage ( m_Backdrop ) )
		{
			g.InterpolationMode = InterpolationMode.HighQualityBicubic;
			g.Clear ( Color.FromArgb ( 12, 8, 8 ) );
			if ( m_Background != null )
			{
				// cover the window, keeping the picture's proportions
				float k = Math.Max ( (float)w / m_Background.Width, (float)h / m_Background.Height );
				float bw = m_Background.Width * k, bh = m_Background.Height * k;
				g.DrawImage ( m_Background, ( w - bw ) / 2, ( h - bh ) / 2, bw, bh );
			}
			// darken, more towards the bottom where the buttons are, with a faint red cast
			using ( SolidBrush b = new SolidBrush ( Color.FromArgb ( 20, 0, 0, 0 ) ) ) g.FillRectangle ( b, 0, 0, w, h );
			using ( LinearGradientBrush b = new LinearGradientBrush ( new Rectangle ( 0, 0, w, h ), Color.FromArgb ( 0, 0, 0, 0 ), Color.FromArgb ( 190, 0, 0, 0 ), LinearGradientMode.Vertical ) )
				g.FillRectangle ( b, 0, 0, w, h );
			using ( SolidBrush b = new SolidBrush ( Color.FromArgb ( 28, CoopLauncher.Accent ) ) ) g.FillRectangle ( b, 0, 0, w, h );
			// vignette
			using ( GraphicsPath p = new GraphicsPath() )
			{
				p.AddEllipse ( -w * 0.25f, -h * 0.35f, w * 1.5f, h * 1.7f );
				using ( PathGradientBrush b = new PathGradientBrush ( p ) )
				{
					b.CenterColor = Color.FromArgb ( 0, 0, 0, 0 );
					b.SurroundColors = new Color[] { Color.FromArgb ( 130, 0, 0, 0 ) };
					g.FillRectangle ( b, 0, 0, w, h );
				}
			}
			// logo, or the title as text
			if ( m_Logo != null )
			{
				float lw = 540 * m_Scale, lh = lw * m_Logo.Height / m_Logo.Width;
				if ( lh > 130 * m_Scale ) { lh = 130 * m_Scale; lw = lh * m_Logo.Width / m_Logo.Height; }
				g.DrawImage ( m_Logo, ( w - lw ) / 2, 40 * m_Scale, lw, lh );
			}
			else
			{
				g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
				using ( Font f = TitleFont ( 34 ) )
				using ( StringFormat sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center } )
					g.DrawString ( CoopLauncher.Title.ToUpperInvariant(), f, Brushes.White, new RectangleF ( 0, 50 * m_Scale, w, 100 * m_Scale ), sf );
			}
			// thin accent frame
			using ( Pen pen = new Pen ( Color.FromArgb ( 160, CoopLauncher.Accent ), Math.Max ( 1f, m_Scale ) ) ) g.DrawRectangle ( pen, 0, 0, w - 1, h - 1 );
		}
	}

	Item Add ( string sName, string sTitle, string sSub, float x, float y, float w, float h, Action click )
	{
		Item it = new Item();
		it.Name = sName; it.Title = sTitle; it.Sub = sSub;
		it.Rect = new RectangleF ( x, y, w, h );
		it.Click = click;
		m_Items.Add ( it );
		return it;
	}

	void AddWindowButtons ()
	{
		Add ( "close", "✕", null, BaseW - 44, 0, 44, 32, delegate { Close(); } ).Small = true;
		Add ( "minimise", "–", null, BaseW - 88, 0, 44, 32, delegate { WindowState = FormWindowState.Minimized; } ).Small = true;
	}

	void ShowMenu ()
	{
		m_AddressMode = false;
		m_FriendsMode = false;
		m_FriendsTimer.Stop();
		m_Address.Visible = false;
		m_Items.Clear();
		m_Hover = m_Pressed = null;
		const float x = 210, w = 340, h = 58;
		Add ( "play", "PLAY", "Single player", x, 196, w, h, delegate { Start ( "" ); } ).Primary = true;
		Add ( "host", "HOST CO-OP", "Steam friends join you", x, 264, w, h, delegate { StartSteam ( "-netsteam=host" ); } );
		Add ( "join", "JOIN CO-OP", "Pick a Steam friend who's hosting", x, 332, w, h, delegate { ShowFriends(); } );
		Add ( "addresslink", "Connect by address instead", null, 18, BaseH - 34, 220, 24, delegate { ShowAddress(); } ).Link = true;
		AddWindowButtons();
		Invalidate();
	}

	void ShowAddress ()
	{
		m_AddressMode = true;
		m_Items.Clear();
		m_Hover = m_Pressed = null;
		Add ( "joinip", "JOIN", "This address", 210, 270, 165, 54, delegate { JoinAddress(); } ).Primary = true;
		Add ( "hostip", "HOST", "Without Steam", 385, 270, 165, 54, delegate { Start ( CoopLauncher.HostArgs() ); } );
		Add ( "back", "‹  Back", null, 18, BaseH - 34, 120, 24, delegate { ShowMenu(); } ).Link = true;
		AddWindowButtons();
		m_Address.Visible = true;
		m_Address.Focus();
		m_Address.SelectAll();
		Invalidate();
	}

	void ShowFriends ()
	{
		// Steam friends who are hosting right now; click one to join exactly their game
		if ( !CoopLauncher.SteamRunning() ) { SetStatus ( "Steam isn't running. Start Steam and sign in, then try again." ); return; }
		string sError;
		if ( !SteamFriendsList.Start ( Path.GetDirectoryName ( CoopLauncher.GameExe ), out sError ) ) { SetStatus ( sError ); return; }
		m_Status = "";
		m_FriendsMode = true;
		m_AddressMode = false;
		m_Address.Visible = false;
		m_Hosts = new List<SteamFriendsList.Host>();
		RefreshFriends ( true );
		m_FriendsTimer.Start();
	}

	void RefreshFriends ( bool bForce )
	{
		List<SteamFriendsList.Host> hosts;
		try { hosts = SteamFriendsList.Hosting(); }
		catch ( Exception ex ) { SetStatus ( "Couldn't read your Steam friends (" + ex.Message + ")." ); return; }
		// rebuild the buttons only when the list changed (keeps hover/press steady)
		bool bSame = !bForce && hosts.Count == m_Hosts.Count;
		for ( int i = 0; bSame && i < hosts.Count; i++ ) bSame = hosts[i].Lobby == m_Hosts[i].Lobby && hosts[i].Name == m_Hosts[i].Name;
		if ( bSame ) return;
		m_Hosts = hosts;
		string sHover = m_Hover != null ? m_Hover.Name : null;
		m_Items.Clear();
		m_Hover = m_Pressed = null;
		for ( int i = 0; i < m_Hosts.Count && i < 3; i++ )
		{
			SteamFriendsList.Host h = m_Hosts[i];
			Item it = Add ( "host" + h.Lobby, h.Name, "Hosting - click to join", 210, 226 + i * 58, 340, 50, delegate { JoinHost ( h ); } );
			it.Primary = i == 0;
		}
		Add ( "auto", "Join whoever is hosting", null, BaseW - 230, BaseH - 34, 212, 24, delegate { SteamFriendsList.Stop(); StartSteam ( "-netsteam=join" ); } ).Link = true;
		Add ( "back", "‹  Back", null, 18, BaseH - 34, 120, 24, delegate { SteamFriendsList.Stop(); ShowMenu(); } ).Link = true;
		AddWindowButtons();
		if ( sHover != null ) foreach ( Item it in m_Items ) if ( it.Name == sHover ) m_Hover = it;
		Invalidate();
	}

	void JoinHost ( SteamFriendsList.Host h )
	{
		// straight into that friend's lobby (the game handles +connect_lobby like a Steam invite)
		SteamFriendsList.Stop();
		Start ( "-netsteam=join +connect_lobby " + h.Lobby );
	}

	void JoinAddress ()
	{
		string sAddress = m_Address.Text.Trim().Replace ( " ", "" );
		if ( sAddress.Length == 0 ) { SetStatus ( "Type the host's address first." ); m_Address.Focus(); return; }
		CoopLauncher.RememberAddress ( sAddress );
		Start ( CoopLauncher.JoinArgs ( sAddress ) );
	}

	void StartSteam ( string sArgs )
	{
		if ( !CoopLauncher.SteamRunning() ) { SetStatus ( "Steam isn't running. Start Steam and sign in, then try again." ); return; }
		Start ( sArgs );
	}

	void Start ( string sArgs )
	{
		string sError;
		if ( CoopLauncher.Launch ( sArgs, out sError ) ) Close();
		else SetStatus ( "Could not start the game: " + sError );
	}

	void SetStatus ( string s ) { m_Status = s; Invalidate(); }

	//
	// drawing
	//

	protected override void OnPaint ( PaintEventArgs e )
	{
		Graphics g = e.Graphics;
		g.DrawImageUnscaled ( m_Backdrop, 0, 0 );
		g.SmoothingMode = SmoothingMode.AntiAlias;
		g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;

		if ( m_AddressMode )
		{
			using ( Font f = TitleFont ( 12 ) ) DrawText ( g, "HOST ADDRESS", f, Color.FromArgb ( 210, 200, 200 ), new RectangleF ( 210, 196, 340, 24 ), StringAlignment.Near );
			using ( Font f = UIFont ( 8.5f ) )
				DrawText ( g, "Hosting without Steam? Your friend joins one of: " + CoopLauncher.LocalAddresses(), f, Color.FromArgb ( 150, 140, 140 ),
					new RectangleF ( 150, 334, 460, 40 ), StringAlignment.Center );
		}

		if ( m_FriendsMode )
		{
			using ( Font f = TitleFont ( 12 ) ) DrawText ( g, "JOIN A FRIEND", f, Color.FromArgb ( 210, 200, 200 ), new RectangleF ( 210, 196, 340, 24 ), StringAlignment.Near );
			if ( m_Hosts.Count == 0 )
			{
				using ( Font f = TitleFont ( 15 ) ) DrawText ( g, "No friends are hosting right now", f, Color.White, new RectangleF ( 120, 250, 520, 32 ), StringAlignment.Center );
				using ( Font f = UIFont ( 9 ) )
					DrawText ( g, "When a Steam friend clicks Host co-op, they show up here. This list updates by itself.", f, Color.FromArgb ( 170, 160, 160 ),
						new RectangleF ( 100, 284, 560, 24 ), StringAlignment.Center );
			}
			else if ( m_Hosts.Count > 3 )
				using ( Font f = UIFont ( 8.5f ) )
					DrawText ( g, "+" + ( m_Hosts.Count - 3 ) + " more hosting", f, Color.FromArgb ( 170, 160, 160 ), new RectangleF ( 210, 398, 340, 20 ), StringAlignment.Center );
		}

		foreach ( Item it in m_Items ) DrawItem ( g, it );

		// Steam indicator, bottom right (the friend list has its own link there)
		if ( !m_FriendsMode )
		{
			Color dot = m_SteamRunning ? Color.FromArgb ( 90, 200, 90 ) : Color.FromArgb ( 120, 110, 110 );
			using ( SolidBrush b = new SolidBrush ( dot ) ) g.FillEllipse ( b, SR ( new RectangleF ( BaseW - 132, BaseH - 27, 9, 9 ) ) );
			using ( Font f = UIFont ( 9 ) )
				DrawText ( g, m_SteamRunning ? "Steam running" : "Steam not running", f, Color.FromArgb ( 170, 160, 160 ), new RectangleF ( BaseW - 118, BaseH - 34, 110, 24 ), StringAlignment.Near );
		}

		if ( m_Status.Length > 0 )
			using ( Font f = UIFont ( 9.5f ) )
				DrawText ( g, m_Status, f, Color.FromArgb ( 235, 110, 110 ), new RectangleF ( 120, 398, 520, 24 ), StringAlignment.Center );
	}

	void DrawText ( Graphics g, string s, Font f, Color c, RectangleF rBase, StringAlignment align )
	{
		using ( SolidBrush b = new SolidBrush ( c ) )
		using ( StringFormat sf = new StringFormat() )
		{
			sf.Alignment = align;
			sf.LineAlignment = StringAlignment.Center;
			sf.Trimming = StringTrimming.EllipsisCharacter;
			g.DrawString ( s, f, b, SR ( rBase ), sf );
		}
	}

	void DrawItem ( Graphics g, Item it )
	{
		bool bHover = it == m_Hover, bDown = it == m_Pressed && bHover;
		RectangleF r = SR ( it.Rect );
		if ( it.Small )
		{
			// window buttons: plain glyph, close turns red on hover
			if ( bHover ) using ( SolidBrush b = new SolidBrush ( it.Name == "close" ? Color.FromArgb ( 200, CoopLauncher.Accent ) : Color.FromArgb ( 50, 255, 255, 255 ) ) ) g.FillRectangle ( b, r );
			using ( Font f = UIFont ( 11 ) ) DrawText ( g, it.Title, f, Color.FromArgb ( bHover ? 255 : 190, 255, 255, 255 ), it.Rect, StringAlignment.Center );
			return;
		}
		if ( it.Link )
		{
			using ( Font f = UIFont ( 9.5f ) ) DrawText ( g, it.Title, f, bHover ? Color.White : Color.FromArgb ( 175, 160, 160 ), it.Rect, StringAlignment.Near );
			return;
		}
		// main buttons: dark glass, blood-red edge; the primary one (and any hovered one) filled red
		Color accent = CoopLauncher.Accent;
		Color fill = it.Primary || bHover ? Lighten ( accent, bDown ? -0.25f : ( bHover ? 0.22f : 0f ) ) : Color.FromArgb ( 150, 10, 6, 6 );
		using ( GraphicsPath p = Rounded ( r, 3 * m_Scale ) )
		{
			using ( SolidBrush b = new SolidBrush ( Color.FromArgb ( it.Primary || bHover ? 235 : fill.A, fill ) ) ) g.FillPath ( b, p );
			using ( Pen pen = new Pen ( Lighten ( accent, bHover ? 0.55f : 0.3f ), Math.Max ( 1f, m_Scale ) ) ) g.DrawPath ( pen, p );
		}
		float titleTop = it.Sub != null ? 0.12f : 0.25f;
		using ( Font f = TitleFont ( 15 ) )
			DrawText ( g, it.Title, f, Color.White, new RectangleF ( it.Rect.X, it.Rect.Y + it.Rect.Height * titleTop, it.Rect.Width, it.Rect.Height * 0.5f ), StringAlignment.Center );
		if ( it.Sub != null )
			using ( Font f = UIFont ( 8.5f ) )
				DrawText ( g, it.Sub, f, Color.FromArgb ( it.Primary || bHover ? 230 : 170, 235, 215, 215 ), new RectangleF ( it.Rect.X, it.Rect.Y + it.Rect.Height * 0.56f, it.Rect.Width, it.Rect.Height * 0.36f ), StringAlignment.Center );
	}

	static Color Lighten ( Color c, float f )
	{
		if ( f >= 0 ) return Color.FromArgb ( c.A, (int)( c.R + ( 255 - c.R ) * f ), (int)( c.G + ( 255 - c.G ) * f ), (int)( c.B + ( 255 - c.B ) * f ) );
		f = 1 + f;
		return Color.FromArgb ( c.A, (int)( c.R * f ), (int)( c.G * f ), (int)( c.B * f ) );
	}

	static GraphicsPath Rounded ( RectangleF r, float rad )
	{
		GraphicsPath p = new GraphicsPath();
		float d = rad * 2;
		p.AddArc ( r.X, r.Y, d, d, 180, 90 );
		p.AddArc ( r.Right - d, r.Y, d, d, 270, 90 );
		p.AddArc ( r.Right - d, r.Bottom - d, d, d, 0, 90 );
		p.AddArc ( r.X, r.Bottom - d, d, d, 90, 90 );
		p.CloseFigure();
		return p;
	}

	//
	// mouse / keys
	//

	Item HitTest ( Point pt )
	{
		PointF b = new PointF ( pt.X / m_Scale, pt.Y / m_Scale );
		for ( int i = m_Items.Count - 1; i >= 0; i-- ) if ( m_Items[i].Rect.Contains ( b ) ) return m_Items[i];
		return null;
	}

	protected override void OnMouseMove ( MouseEventArgs e )
	{
		Item it = HitTest ( e.Location );
		if ( it != m_Hover ) { m_Hover = it; Cursor = it != null ? Cursors.Hand : Cursors.Default; Invalidate(); }
		base.OnMouseMove ( e );
	}

	protected override void OnMouseLeave ( EventArgs e )
	{
		if ( m_Hover != null ) { m_Hover = null; Cursor = Cursors.Default; Invalidate(); }
		base.OnMouseLeave ( e );
	}

	protected override void OnMouseDown ( MouseEventArgs e )
	{
		if ( e.Button != MouseButtons.Left ) return;
		Item it = HitTest ( e.Location );
		if ( it != null ) { m_Pressed = it; Invalidate(); return; }
		// drag the borderless window from anywhere else
		ReleaseCapture();
		SendMessage ( Handle, 0xA1, (IntPtr)2, IntPtr.Zero );	// WM_NCLBUTTONDOWN, HTCAPTION
	}

	protected override void OnMouseUp ( MouseEventArgs e )
	{
		Item it = HitTest ( e.Location );
		Item pressed = m_Pressed;
		m_Pressed = null;
		Invalidate();
		if ( pressed != null && it == pressed && pressed.Click != null ) pressed.Click();
	}

	protected override void OnKeyDown ( KeyEventArgs e )
	{
		if ( e.KeyCode == Keys.Escape ) { if ( m_AddressMode || m_FriendsMode ) { SteamFriendsList.Stop(); ShowMenu(); } else Close(); e.Handled = true; }
		else if ( e.KeyCode == Keys.Enter && !m_AddressMode && !m_FriendsMode ) { Start ( "" ); e.Handled = true; }
		base.OnKeyDown ( e );
	}

	protected override CreateParams CreateParams
	{
		get
		{
			// borderless but still a normal taskbar window with a drop shadow, minimisable from the taskbar
			CreateParams cp = base.CreateParams;
			cp.ClassStyle |= 0x20000;		// CS_DROPSHADOW
			cp.Style |= 0x20000;			// WS_MINIMIZEBOX
			return cp;
		}
	}
}
