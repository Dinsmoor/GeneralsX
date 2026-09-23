/*
**	Reads the bot config INI described in BotConfig.h and applies it to
**	TheWritableGlobalData.
*/

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/BotConfig.h"
#include "GameClient/ClientInstance.h"
#include "Common/GlobalData.h"
#include "Common/UserPreferences.h"
#include "GameNetwork/NetworkDefs.h"	// LAN_LOBBY_PORT_DEFAULT, and the port pairing

namespace
{

/**
	UserPreferences::load() always resolves its argument inside the user data
	directory, which is right for preference files but wrong for a config the
	user names on the command line -- "-botconfig ./joiner.ini" should mean
	the file they are looking at. Take the path as given when it looks like
	one, and keep the base class's behaviour otherwise.
*/
class BotConfigFile : public UserPreferences
{
public:
	virtual Bool load( AsciiString fname ) override
	{
		const char *s = fname.str();
		const Bool hasPath = (strchr(s, '\\') != nullptr)
			|| (strchr(s, '/') != nullptr)
			|| (s[0] != 0 && s[1] == ':');

		if (!hasPath)
			return UserPreferences::load(fname);

		// The base class prepends the user data path unconditionally and
		// offers no way to opt out, so an arbitrary path has to be read
		// here -- with the same rules it uses: "key = value", blank and
		// valueless lines skipped.
		m_filename = fname;

		FILE *fp = fopen(m_filename.str(), "r");
		if (fp == nullptr)
			return FALSE;

		char buf[2048];
		while (fgets(buf, sizeof(buf), fp) != nullptr)
		{
			AsciiString line = buf;
			line.trim();

			// A line with no '=' would otherwise be read as a key with the
			// rest of the buffer as its value.
			if (strchr(line.str(), '=') == nullptr)
				continue;

			AsciiString key, val;
			line.nextToken(&key, "=");
			val = line.str() + 1;

			key.trim();
			val.trim();

			if (key.isEmpty() || val.isEmpty())
				continue;

			(*this)[key] = val;
		}
		fclose(fp);
		return TRUE;
	}
};

/**
	The file is written by people, so accept the key in whatever case they
	typed it. UserPreferences is a plain map keyed by AsciiString, so this
	walks it rather than indexing.
*/
static Bool findValue( const UserPreferences& prefs, const char *key, AsciiString *out )
{
	UserPreferences::const_iterator it;
	for (it = prefs.begin(); it != prefs.end(); ++it)
	{
		if (it->first.compareNoCase(key) == 0)
		{
			*out = it->second;
			return TRUE;
		}
	}
	return FALSE;
}

static Bool getString( const UserPreferences& prefs, const char *key, AsciiString *out )
{
	AsciiString val;
	if (!findValue(prefs, key, &val) || val.isEmpty())
		return FALSE;
	*out = val;
	return TRUE;
}

static Bool getInt( const UserPreferences& prefs, const char *key, Int *out )
{
	AsciiString val;
	if (!findValue(prefs, key, &val) || val.isEmpty())
		return FALSE;
	*out = atoi(val.str());
	return TRUE;
}

static Bool getBool( const UserPreferences& prefs, const char *key, Bool *out )
{
	AsciiString val;
	if (!findValue(prefs, key, &val) || val.isEmpty())
		return FALSE;

	val.toLower();
	*out = (val == "1" || val == "t" || val == "true"
		|| val == "y" || val == "yes" || val == "on");
	return TRUE;
}

}	// anonymous namespace

Bool BotConfig::load( const AsciiString& fname )
{
	if (fname.isEmpty())
		return FALSE;

	BotConfigFile prefs;
	if (!prefs.load(fname))
	{
		fprintf(stderr, "BotConfig: cannot open '%s'\n", fname.str());
		fflush(stderr);
		return FALSE;
	}

	GlobalData *gd = TheWritableGlobalData;

	// -- role ---------------------------------------------------------------
	//
	// The role decides which of the two entirely separate code paths in
	// BotLanJoin runs, so it is the one key worth being strict about: a
	// misspelling here would otherwise silently produce a bot that does
	// nothing at all.
	AsciiString role;
	if (getString(prefs, "role", &role))
	{
		role.toLower();
		if (role == "join" || role == "joiner" || role == "client")
		{
			gd->m_botJoinEnabled = TRUE;
		}
		else if (role == "host" || role == "server")
		{
			gd->m_botHostEnabled = TRUE;
		}
		else
		{
			fprintf(stderr, "BotConfig: role must be 'join' or 'host', not '%s'\n",
				role.str());
			fflush(stderr);
			return FALSE;
		}
	}

	AsciiString s;
	Int i = 0;
	Bool b = FALSE;

	// -- lobby --------------------------------------------------------------
	if (getString(prefs, "host", &s))
		gd->m_botJoinHost = s;
	if (getString(prefs, "map", &s))
		gd->m_botHostMap = s;
	if (getInt(prefs, "players", &i))
		gd->m_botHostPlayers = i;
	if (getBool(prefs, "lanLobby", &b))
		gd->m_botHostLanLobby = b;

	/*	Who sits in each slot, and who is on whose side. Both are host-only:
		only the host owns the slot list, exactly as in the options menu
		where every other player's combo box is disabled.

		Written as one comma-separated entry per slot STARTING AT SLOT 1 --
		slot 0 is the host itself, which is this process and cannot be
		anything else. "slots = medium,medium" therefore describes a
		three-player game: us, and two medium AIs.

		`teams` is 1-based to match what the menu shows a person; "-" means
		no team. It covers every slot INCLUDING slot 0, because the host does
		pick its own team.
	*/
	if (getString(prefs, "slots", &s))
		gd->m_botHostSlots = s;
	if (getString(prefs, "teams", &s))
		gd->m_botHostTeams = s;
	if (getString(prefs, "name", &s))
		gd->m_botJoinName = s;
	if (getString(prefs, "localIP", &s))
		gd->m_netLocalIP = s;

	/*	Which UDP port the LAN lobby binds. Omit it for retail behaviour.

		localIP above is enough to run two engines on one Windows box, because
		the lobby socket binds that specific address there. On POSIX the socket
		must bind INADDR_ANY to receive broadcasts at all, so the PORT is what
		collides and the second engine cannot bind 8086. Giving each engine its
		own lobby port fixes that; LANAPI answers every peer on the port it was
		heard from, so a host and a joiner on different ports still find each
		other. See LANAPI::m_lobbyPort.

		Range-checked because a port is 16 bits and 0 already means "unset": a
		silently truncated 70000 would bind 4464, and the failure would look
		like the peer simply never answering.
	*/
	if (getInt(prefs, "lanPort", &i))
	{
		/*	The upper bound is LAN_LOBBY_PORT_DEFAULT, not 65535, because the
			gameplay port is DERIVED from this one by counting the other way
			(LANGamePortFromLobbyPort): lobby 8086-n pairs with gameplay 8088+n.
			A lobby port above 8086 has no gameplay port to pair with -- the
			derivation would clamp it back to 8088 -- so two such instances would
			quietly share the gameplay port while looking configured, which is
			exactly the silent failure this range check exists to prevent.

			The floor keeps the pair inside a sane block; 8 instances need only
			8079..8086, and MAX_SLOTS is 8. */
		if ((i < LAN_LOBBY_PORT_DEFAULT - (MAX_SLOTS - 1)) || (i > LAN_LOBBY_PORT_DEFAULT))
		{
			fprintf(stderr, "BotConfig: lanPort must be %d..%d, not %d\n",
				(Int)(LAN_LOBBY_PORT_DEFAULT - (MAX_SLOTS - 1)), (Int)LAN_LOBBY_PORT_DEFAULT, i);
			fprintf(stderr, "BotConfig: it pairs with gameplay port %d..%d; see NetworkDefs.h\n",
				NETWORK_BASE_PORT_NUMBER, NETWORK_BASE_PORT_NUMBER + (MAX_SLOTS - 1));
			fflush(stderr);
			return FALSE;
		}
		gd->m_netLobbyPort = i;
	}

	if (getString(prefs, "lobbyScript", &s))
		gd->m_lobbyScript = s;

	/*	Kept as the word. There is no ThePlayerTemplateStore yet -- INI data
		loads long after the startup parse -- so this cannot be resolved to an
		index here. BotLanJoin resolves it once the lobby exists.
	*/
	if (getString(prefs, "faction", &s))
		gd->m_botFaction = s;

	// -- the agent driving us ------------------------------------------------
	if (getInt(prefs, "obsPort", &i))
		gd->m_observationPort = (UnsignedShort)i;
	if (getInt(prefs, "obsInterval", &i))
		gd->m_observationInterval = (UnsignedInt)i;
	if (getInt(prefs, "obsPlayer", &i))
		gd->m_observationPlayer = i;
	/*	obsDelta N: send changed fields only, with a full keyframe every N
		observations. The multiplayer path reaches the engine ONLY through
		this file -- bot-join-mp.sh builds it and passes -botconfig -- so
		without this key the command-line -obsdelta could never apply to a
		real network match, which is exactly where the volume matters most.
		See docs/OBS_PROTOCOL.md.
	*/
	if (getInt(prefs, "obsDelta", &i))
		gd->m_observationDelta = (UnsignedInt)(i > 0 ? i : 0);
	if (getInt(prefs, "actPort", &i))
		gd->m_actionPort = (UnsignedShort)i;
	if (getInt(prefs, "actPlayer", &i))
		gd->m_actionPlayer = i;
	if (getInt(prefs, "frames", &i))
		gd->m_skirmishMaxFrames = (UnsignedInt)i;

	/*	Both roles routinely run beside another engine on this machine -- a
		bot hosting for a bot, or a bot joining the game a person is hosting
		from the same box. The single-instance mutex would let only the first
		one live, and the second exits from WinMain before printing anything,
		which reads exactly like a silent crash.

		This is why the config has to be loaded during the STARTUP parse and
		not later: these two calls must happen before engine init.
	*/
	if (gd->m_botJoinEnabled || gd->m_botHostEnabled)
	{
		rts::ClientInstance::setMultiInstance(TRUE);
		rts::ClientInstance::skipPrimaryInstance();

		/*	No shell map, no intro, no sizzle.

			Shell::showShellMap() does not merely draw a menu background: if
			it finds the engine in "some other kind of game" it calls
			TheGameLogic->exitGame() and posts MSG_NEW_GAME(GAME_SHELL) --
			it TEARS DOWN a running match and replaces it with the shell.

			That is harmless in a normal client, where the shell map only
			comes up from the menus. A bot reaches it because it now runs
			the engine's own frame (GameEngine::update(), which updates
			TheGameClient), so the shell got a chance to run and promptly
			replaced the LAN game a frame after it started. The symptom was
			a bot that looked alive -- frames advancing, isInGame() true --
			while sitting in the main menu: no load progress sent, no
			objects, nothing observed, and a host that eventually called it
			a dropped player.

			Every other non-interactive mode does exactly this; see
			parseNoShellMap and the replay/save paths in CommandLine.cpp.
		*/
		gd->m_shellMapOn = FALSE;
		gd->m_playIntro = FALSE;
		gd->m_playSizzle = FALSE;
	}

	return TRUE;
}
