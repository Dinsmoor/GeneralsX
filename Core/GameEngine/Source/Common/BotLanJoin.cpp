/*
**	Headless LAN joiner. See BotLanJoin.h for the rationale.
**
**	Three things make this different from the headless skirmish launcher:
**
**	1. THE LAN LAYER IS NORMALLY PUMPED BY A MENU. In a real client
**	   TheLAN->update() is called from the LAN lobby window
**	   (LanLobbyMenu.cpp). Headless has no windows, so the lobby phase here
**	   runs its own loop and pumps TheLAN itself.
**
**	2. THE MESSAGE STREAM IS NORMALLY PUMPED BY GameEngine::update(). The
**	   skirmish launcher can put MSG_NEW_GAME straight on TheCommandList
**	   because it builds the game itself; we cannot, because LANAPI::
**	   OnGameStart() -- which runs deep inside TheLAN->update() when the host
**	   presses start -- uses TheMessageStream->appendMessage(). So both loops
**	   here call propagateMessages(), or the match would never begin.
**
**	3. THE SLOT IS NOT KNOWN IN ADVANCE. In a skirmish you say -actplayer 2
**	   because you built the slot list. Joining a lobby, the slot depends on
**	   join order, so the observation and action servers are told to bind to
**	   the local player instead (-obsplayer -2, -actplayer -1).
**
**	The bot never touches the host's slot list directly. It asks, exactly the
**	way the options menu asks when a non-host changes their own faction:
**	RequestGameOptions("PlayerTemplate=%d"). The host applies it and echoes a
**	new slot list to everyone. Anything else would desync the lobby.
*/

#include "PreRTS.h"

#include <signal.h>				// the shutdown handlers, see leaveLobbyForShutdown

#include "Common/BotLanJoin.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/FramePacer.h"
#include "Common/MessageStream.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/Recorder.h"
#include "Common/PlayerTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"

#include <string>
#include <vector>
#include <ctype.h>

/**
	The menu's "a button was pushed, freeze the LAN layer until the screen
	settles" flag (LanLobbyMenu.cpp).

	TheSuperHackers @fix LANAPI::update() skips its ENTIRE receive loop while
	this is set, and the callbacks set it on every success -- creating a
	game, joining one, a player leaving. In the menu the next screen's init
	clears it; headless there is no next screen, so a host stopped hearing
	anything the moment it created its game and a joiner went permanently
	deaf the moment it got in. Both looked like packets vanishing.
*/
extern Bool LANbuttonPushed;

namespace
{

/*	Which role this process is playing.

	The host and the joiner run the SAME engine, and almost every piece of
	lobby behaviour differs between them: who opens slots, who decides the
	match starts, who answers a join, who has to keep saying HELLO. Reading
	that from whichever global happened to be set (m_botJoinEnabled, say)
	silently ties shared code to one role -- which is how chat handling
	ended up working only for a joiner.

	Set once, by the entry point, and consulted everywhere else.
*/
enum BotRole { ROLE_NONE, ROLE_HOST, ROLE_JOINER };
BotRole s_role = ROLE_NONE;

/// -lobbyscript: chat lines a bot host says, to drive an automated test.
std::vector<std::string> s_script;
size_t s_scriptAt = 0;
DWORD s_scriptNextAt = 0;

void loadLobbyScript( const char *path )
{
	FILE *f = fopen(path, "r");
	if (f == nullptr)
	{
		printf("BotHost: cannot read lobby script '%s'\n", path);
		return;
	}
	char buf[512];
	while (fgets(buf, sizeof(buf), f) != nullptr)
	{
		std::string line(buf);
		while (!line.empty() && (line[line.size()-1] == '\n' || line[line.size()-1] == '\r'))
			line.erase(line.size()-1);
		if (!line.empty() && line[0] != '#')
			s_script.push_back(line);
	}
	fclose(f);
	printf("BotHost: lobby script has %d lines\n", (int)s_script.size());
	fflush(stdout);
}

/// Set once the host has started the match, by the chat callback path.
Bool s_gameStarted = FALSE;
/// Set when we have asked to be considered ready.
Bool s_accepted = FALSE;
Bool s_saidReady = FALSE;
Bool s_saidHello = FALSE;
/// Set once the config's faction has been asked for, so we ask only once.
Bool s_askedForFaction = FALSE;
/// The lobby state we last accepted for; see lobbySignature().
AsciiString s_acceptedFor;
AsciiString s_noMapSaidFor;
/// What we answer to in chat. Lower case, no spaces.
std::string s_botWord = "bot";

std::string toLowerNarrow( const UnicodeString& u )
{
	std::string out;
	AsciiString a;
	a.translate(u);
	for (const char *p = a.str(); p != nullptr && *p != '\0'; ++p)
		out += (char)tolower((unsigned char)*p);
	return out;
}

/**
	Ask the host for a slot setting.

	This is the non-host path from LanGameOptionsMenu: a joiner cannot write
	the slot list, it sends the single option it wants and the host decides.
*/
/**
	Change one setting on OUR OWN slot, whichever role we are.

	These are not the same operation:

	  - A JOINER has to ASK. RequestGameOptions("PlayerTemplate=3") is the
	    request a client sends the host, and the host applies it in
	    OnGameOptions under `AmIHost() && m_localIP != playerIP`.

	  - A HOST owns the slot list, so it writes the slot and publishes the
	    result. Sending itself a request does nothing at all: that same
	    guard excludes messages from its own IP, so a bot host asking
	    itself to change faction was silently ignored.
*/
void requestOption( const char *key, Int value )
{
	if (TheLAN == nullptr)
		return;

	if (s_role == ROLE_HOST)
	{
		LANGameInfo *game = TheLAN->GetMyGame();
		const Int slotNum = (game != nullptr) ? game->getLocalSlotNum() : -1;
		LANGameSlot *slot = (slotNum >= 0) ? game->getLANSlot(slotNum) : nullptr;
		if (slot == nullptr)
			return;

		const AsciiString k(key);
		if (k == "PlayerTemplate")
			slot->setPlayerTemplate(value);
		else if (k == "Team")
			slot->setTeamNumber(value);
		else if (k == "Color")
			slot->setColor(value);
		else if (k == "StartPos")
			slot->setStartPos(value);
		else
			return;

		// Publish the new slot list; that is what a menu click does.
		TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
		return;
	}

	AsciiString options;
	options.format("%s=%d", key, value);
	TheLAN->RequestGameOptions(options, true);
}

/**
	Say one line into the lobby, split across as many chat messages as it
	takes.

	A LAN chat message holds g_lanMaxChatLength (100) wide characters, and
	RequestChat wcslcpy's into that buffer -- so a longer line is silently
	TRUNCATED, not rejected. The bot's two most useful lines, the greeting
	and the command list, are both over 120 characters, so a person reading
	the lobby lost exactly the part that told them what to type.

	Split on spaces so words stay intact, and fall back to a hard cut for a
	single word longer than the limit.
*/
void say( const char *text )
{
	if (TheLAN == nullptr || text == nullptr)
		return;

	// Nobody can see a headless bot's chat window, so echo both halves of
	// the conversation to stdout: it is the only record of what the bot
	// was asked and what it answered, and it is what the lobby test reads.
	// Log the line WHOLE -- the wrapping below is a property of the
	// transport, and splitting the log too would only make it harder to read.
	printf("BotChat: > %s\n", text);
	fflush(stdout);

	const size_t limit = (size_t)g_lanMaxChatLength;
	std::string rest = text;

	while (!rest.empty())
	{
		std::string chunk;
		if (rest.size() <= limit)
		{
			chunk = rest;
			rest.clear();
		}
		else
		{
			// Break at the last space that fits, so a word is never split
			// across two messages.
			size_t cut = rest.rfind(' ', limit);
			if (cut == std::string::npos || cut == 0)
				cut = limit;		// one enormous word; cut it

			chunk = rest.substr(0, cut);
			// Drop the space we broke on, and any that follow it.
			rest.erase(0, cut);
			while (!rest.empty() && rest[0] == ' ')
				rest.erase(0, 1);
		}

		UnicodeString u;
		u.translate(AsciiString(chunk.c_str()));
		TheLAN->RequestChat(u, LANAPIInterface::LANCHAT_NORMAL);
	}
}

/**
	Split "a,b,c" into its trimmed, lower-cased pieces. Used for the host's
	`slots` and `teams` config lines, which are both one entry per slot.
*/
static std::vector<std::string> splitCommaList( const AsciiString& csv )
{
	std::vector<std::string> out;
	std::string cur;
	const char *p = csv.str();
	for (; *p != 0; ++p)
	{
		if (*p == ',')
		{
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur += (char)tolower((unsigned char)*p);
	}
	out.push_back(cur);

	for (size_t q = 0; q < out.size(); ++q)
	{
		std::string& t = out[q];
		while (!t.empty() && (t[0] == ' ' || t[0] == '\t'))
			t.erase(0, 1);
		while (!t.empty() && (t[t.size() - 1] == ' ' || t[t.size() - 1] == '\t'))
			t.erase(t.size() - 1, 1);
	}
	return out;
}

/**
	The slot contents a person picks from the host's player combo box, by the
	word they would say for it. The combo's entries ARE the SlotState enum in
	order (Open, Closed, EasyAI, MediumAI, HardAI), which is why the menu can
	pass the selected position straight to setState().

	Returns FALSE for a word we do not know, so the caller can complain with
	the offending text rather than silently seating something else.
*/
static Bool slotStateFromWord( const std::string& word, SlotState *out )
{
	if (word == "open")                              { *out = SLOT_OPEN;      return TRUE; }
	if (word == "closed" || word == "close")         { *out = SLOT_CLOSED;    return TRUE; }
	if (word == "easy")                              { *out = SLOT_EASY_AI;   return TRUE; }
	if (word == "medium" || word == "med")           { *out = SLOT_MED_AI;    return TRUE; }
	if (word == "hard" || word == "brutal")          { *out = SLOT_BRUTAL_AI; return TRUE; }
	return FALSE;
}

/**
	Write the configured team numbers onto the slot list.

	Teams are 1-based here, as the options menu shows them to a person, with
	"-" (or an empty entry) meaning no team. They are stored 0-based with -1
	for none, which is what reportMySlot prints back. Slot 0 is included:
	the host picks its own side like anyone else.

	This MUST be re-applied after anybody is seated, and that is why it is a
	function rather than a block that runs once.

	Seating a player REPLACES the whole slot. Both sides do it: the host's
	handleRequestJoin builds a fresh `LANGameSlot newSlot` and calls
	setSlot(player, newSlot), and the joiner's handleJoinAccept does the
	same for its own slot. A default-constructed slot has m_teamNumber = -1,
	so every team we had written for a still-empty slot was silently wiped
	the moment its player arrived. The host log looked perfect -- it printed
	the teams it had just set -- and the match then started with everybody
	on no team, which is a free-for-all, not the 2v2 that was asked for.

	`whenLabel` only says which pass is talking, so the log shows the
	re-apply rather than looking like a duplicate.
*/
static void applyConfiguredTeams( LANGameInfo *game, const char *whenLabel )
{
	if (game == nullptr || TheGlobalData->m_botHostTeams.isEmpty())
		return;

	const std::vector<std::string> want =
		splitCommaList(TheGlobalData->m_botHostTeams);
	for (size_t w = 0; w < want.size() && (Int)w < MAX_SLOTS; ++w)
	{
		if (want[w].empty())
			continue;

		GameSlot *slot = game->getLANSlot((Int)w);
		if (slot == nullptr)
			continue;

		if (want[w] == "-" || want[w] == "none")
		{
			slot->setTeamNumber(-1);
			continue;
		}
		const Int team = atoi(want[w].c_str());
		if (team < 1 || team > MAX_SLOTS)
		{
			printf("BotHost: teams entry %d is '%s'; teams are 1..%d"
				" or '-' for none\n", (Int)w, want[w].c_str(), MAX_SLOTS);
			continue;
		}
		slot->setTeamNumber(team - 1);
		printf("BotHost: slot %d on team %d (%s)\n",
			(Int)w, team, whenLabel);
	}
	fflush(stdout);
}

/**
	The faction names a person would actually type, mapped to the engine's
	player template index. "china" matches FactionChina; the side name is
	what the templates are keyed by.
*/
Int factionIndexFromWord( const std::string& word )
{
	if (ThePlayerTemplateStore == nullptr)
		return -1;

	const Int count = ThePlayerTemplateStore->getPlayerTemplateCount();
	for (Int i = 0; i < count; ++i)
	{
		const PlayerTemplate *t = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (t == nullptr)
			continue;

		// Match on the side name ("China", "America", "GLA") and on the
		// template's own name ("FactionChinaTankGeneral"), case-insensitively
		// and by substring, so "tank" finds the Tank General.
		// VC6 leaks a for-scoped variable into the enclosing block, so these
		// two loops cannot both declare "p".
		std::string side, name;
		for (const char *sp = t->getSide().str(); sp && *sp; ++sp)
			side += (char)tolower((unsigned char)*sp);
		for (const char *np = t->getName().str(); np && *np; ++np)
			name += (char)tolower((unsigned char)*np);

		if (side == word || name == word)
			return i;
		if (word.size() >= 3 && name.find(word) != std::string::npos)
			return i;
	}
	return -1;
}

/**
	Ask for the faction named in the bot config, once, as soon as we have a
	slot to ask about.

	Without this a bot always played as Random, whatever its config said. The
	faction a client joins with comes from its LANPreferences file, whose
	PlayerTemplate key is an index into the template store -- and for a
	headless bot that file is written by the engine with -1 (Random) and never
	edited. The index is also opaque from outside the engine, because the
	templates live inside the .big archives.

	So the config carries the WORD, and it is resolved here, where the
	template store finally exists, by the same function that reads "bot
	faction china" out of lobby chat. This is exactly what a person does with
	the dropdown after joining, which is why it goes through requestOption and
	not through the preferences file: the host applies it and echoes it back
	like any other slot change.
*/
void applyConfiguredFaction()
{
	if (s_askedForFaction)
		return;
	if (TheGlobalData == nullptr || TheGlobalData->m_botFaction.isEmpty())
	{
		s_askedForFaction = TRUE;	// nothing configured; never look again
		return;
	}

	std::string word;
	for (const char *p = TheGlobalData->m_botFaction.str(); p && *p; ++p)
		word += (char)tolower((unsigned char)*p);

	// "random" is a legitimate choice and is what the engine does anyway.
	if (word == "random" || word == "any")
	{
		s_askedForFaction = TRUE;
		return;
	}

	const Int idx = factionIndexFromWord(word);
	if (idx < 0)
	{
		// Say it where a person can see it AND in the log: an unknown faction
		// would otherwise show up only as a bot that is mysteriously Random.
		printf("BotJoin: config asked for faction '%s', which I do not know; "
			"staying Random\n", TheGlobalData->m_botFaction.str());
		fflush(stdout);
		s_askedForFaction = TRUE;
		return;
	}

	s_askedForFaction = TRUE;
	printf("BotJoin: asking for faction '%s' (template %d)\n",
		TheGlobalData->m_botFaction.str(), idx);
	fflush(stdout);
	requestOption("PlayerTemplate", idx);
}

/**
	A summary of the lobby state the bot reacts to, used to decide whether
	anything has actually changed since it last accepted.

	Deliberately NOT GameInfoToAsciiString(): that includes each slot's
	accept flag, which flips every time anyone accepts -- including us. Any
	guard keyed on it would fire on its own echo, which is the loop this
	exists to break.

	What matters is the map (can we play it), who is in the game, and what
	each of them picked. If none of that moved, our accept still stands and
	re-sending it would only generate another slot-list broadcast.
*/
AsciiString lobbySignature( const LANGameInfo *game )
{
	AsciiString sig;
	if (game == nullptr)
		return sig;

	sig = game->getMap();
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		AsciiString part;
		if (slot == nullptr)
		{
			part = "|-";
		}
		else
		{
			// State covers open/closed/AI/human; the rest is what a person
			// would have set in the lobby. The accept flag is omitted on
			// purpose -- see above.
			part.format("|%d,%X,%d,%d,%d,%d",
				(Int)slot->getState(), slot->getIP(),
				slot->getPlayerTemplate(), slot->getColor(),
				slot->getStartPos(), slot->getTeamNumber());
		}
		sig.concat(part);
	}
	return sig;
}

void reportSlot()
{
	if (TheLAN == nullptr)
		return;
	LANGameInfo *game = TheLAN->GetMyGame();
	if (game == nullptr)
		return;
	const Int slotNum = game->getLocalSlotNum();
	const GameSlot *slot = (slotNum >= 0) ? game->getConstSlot(slotNum) : nullptr;
	if (slot == nullptr)
		return;

	AsciiString faction("?");
	if (ThePlayerTemplateStore != nullptr)
	{
		const PlayerTemplate *t =
			ThePlayerTemplateStore->getNthPlayerTemplate(slot->getPlayerTemplate());
		if (t != nullptr)
			faction = t->getSide();
	}

	char line[256];
	// Report the numbers the lobby SHOWS, matching what the commands take:
	// team and start are stored 0-based with -1 meaning "none".
	char teamStr[16], posStr[16];
	if (slot->getTeamNumber() < 0)
		strcpy(teamStr, "none");
	else
		snprintf(teamStr, sizeof(teamStr), "%d", slot->getTeamNumber() + 1);
	if (slot->getStartPos() < 0)
		strcpy(posStr, "any");
	else
		snprintf(posStr, sizeof(posStr), "%d", slot->getStartPos() + 1);

	snprintf(line, sizeof(line),
		"slot %d, faction %s, team %s, colour %d, start %s, %s",
		slotNum, faction.str(), teamStr, slot->getColor(),
		posStr, s_accepted ? "ready" : "not ready");
	say(line);
}

/**
	Bring TheLAN up, bound to the address -netlocalip asked for.

	LANAPI assumes one engine per machine: the lobby port is a constant,
	because "LAN game, everyone has a unique IP, so it's ok to use the same
	port" (LANAPI.cpp). Two engines on ONE box -- a bot hosting for another
	bot, which is how this path gets tested at all -- both try to bind 8086
	and the second one's Transport::init() fails. Giving them distinct
	loopback addresses restores the engine's own assumption instead of
	fighting it: the whole 127/8 range is routable, so 127.0.0.1 and
	127.0.0.2 need no setup.
*/
/**
	Bring TheLAN up, exactly the way the Direct Connect menu does.

	NetworkDirectConnectInit: new LANAPI(), init(), then SetLocalIP() -- in
	that order, because init() binds the transport to whatever m_localIP is
	and SetLocalIP() re-binds it. The menu picks the address from
	IPEnumeration; -netlocalip overrides it, which is what lets two engines
	share one machine. LANAPI assumes one per box ("everyone has a unique
	IP, so it's ok to use the same port"), so distinct loopback addresses
	restore that assumption rather than fighting it -- the whole 127/8
	range is routable and needs no setup.
*/
/**
	Parse a dotted-quad into HOST byte order, rejecting out-of-range octets.

	sscanf("%d.%d.%d.%d") alone accepts "999.1.1.1" and "192.168.1.5xyz";
	the first shifts garbage into the high bits and the second silently
	ignores the tail. Either way we would bind to an address the user did
	not ask for, which is the failure this whole path exists to prevent.
*/
static Bool parseIPv4(const char *text, UnsignedInt *out)
{
	Int a = -1, b = -1, c = -1, d = -1;
	char tail = 0;
	if (text == nullptr)
		return FALSE;
	// The trailing %c catches anything after the fourth octet: a clean
	// parse consumes exactly four numbers and nothing else.
	if (sscanf(text, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4)
		return FALSE;
	if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255)
		return FALSE;
	*out = ((UnsignedInt)a << 24) | ((UnsignedInt)b << 16)
		 | ((UnsignedInt)c << 8)  | (UnsignedInt)d;
	return TRUE;
}

/**
	Parse "10.0.0.5" or "10.0.0.5:8087" -- an address with an OPTIONAL port.

	The port is needed only for direct connect, and only when the host moved
	off the default lobby port. Discovery does not need it: a host's announce
	packet teaches us its port before we ever answer, so the peer port table
	is already right by the time we send anything. Direct connect has no such
	packet -- we unicast the first datagram to an address that was typed in --
	so without the ":port" there is nothing to tell us where to aim, and the
	join request lands on a port nobody is listening to.

	*port is left ALONE when no suffix is given, so the caller's default
	survives. It is not defaulted here because "no port was specified" and
	"port 0 was specified" have to stay distinguishable.
*/
static Bool parseIPv4WithPort(const char *text, UnsignedInt *out, UnsignedShort *port)
{
	if (text == nullptr)
		return FALSE;

	const char *colon = strchr(text, ':');
	if (colon == nullptr)
		return parseIPv4(text, out);

	// Split at the colon so parseIPv4 still sees a bare quad and keeps its
	// own strictness about trailing junk.
	char addr[64];
	const size_t addrLen = (size_t)(colon - text);
	if (addrLen == 0 || addrLen >= sizeof(addr))
		return FALSE;
	memcpy(addr, text, addrLen);
	addr[addrLen] = 0;

	if (!parseIPv4(addr, out))
		return FALSE;

	Int p = -1;
	char tail = 0;
	if (sscanf(colon + 1, "%d%c", &p, &tail) != 1)
		return FALSE;
	if (p < 1 || p > 65535)
		return FALSE;

	*port = (UnsignedShort)p;
	return TRUE;
}

/**
	LEAVE THE LOBBY WHEN WE ARE KILLED.

	Ctrl-C, a `kill`, or the launcher script taking the engine down at the
	end of a run all stop this process wherever it happens to be -- and in
	the lobby that is inside a Sleep(16), with no chance to say anything.
	LANAPI is a UDP protocol with no connection to break, so the host never
	learns we are gone: our slot stays occupied and everybody waits out the
	host's own timeout before the game can start. That is a person sitting
	in front of a stuck lobby wondering what the bot is doing, and it
	happens every single time the bot is stopped by hand.

	The polite exit already exists and is already used on the lobby-timeout
	path: RequestGameLeave() sends MSG_REQUEST_GAME_LEAVE and then pumps
	m_transport itself, precisely so the packet is on the wire before
	anything else is torn down. Nothing called it on a signal, so this does.

	WHAT IS SAFE TO DO IN A HANDLER. Very little, and this is deliberately
	close to the edge: it sends one UDP datagram and returns. It is not
	re-entrant, so s_leaving latches to make a second signal a no-op rather
	than re-entering LANAPI while the first call is still inside it. It does
	not free anything, does not touch the game loop, and does not try to
	shut the engine down cleanly -- exiting is still the caller's job.

	ONLY WHILE WE ARE IN A LOBBY. Once the match starts the slot is no
	longer the thing holding anybody up, and a leave message mid-game would
	be read as a player quitting, which is a different event with different
	consequences for the remaining peers. So this does nothing after
	s_gameStarted.
*/
static volatile sig_atomic_t s_leaving = 0;

static void leaveLobbyForShutdown( const char *why )
{
	if (s_leaving)
		return;						// already on our way out
	s_leaving = 1;

	// Nothing to leave: no lobby yet, or the match is running and our slot
	// is not what anybody is waiting on.
	if (TheLAN == nullptr || s_gameStarted)
		return;

	printf("BotJoin: %s -- leaving the lobby\n", why);
	fflush(stdout);

	// Both of these send one datagram and pump the transport so it is on
	// the wire before we return; neither allocates or runs a callback.
	// RequestGameLeave's joiner branch then sets a pending action with a
	// timeout, which nothing will ever resolve because we are about to
	// exit -- that is fine, the packet the host needs has already gone.
	TheLAN->RequestGameLeave();
	TheLAN->RequestLobbyLeave(true);
}

#ifdef _WIN32
static BOOL WINAPI botConsoleCtrlHandler( DWORD type )
{
	switch (type)
	{
		case CTRL_C_EVENT:		leaveLobbyForShutdown("interrupted");   break;
		case CTRL_BREAK_EVENT:	leaveLobbyForShutdown("interrupted");   break;
		case CTRL_CLOSE_EVENT:	leaveLobbyForShutdown("console closed");break;
		case CTRL_SHUTDOWN_EVENT:
		case CTRL_LOGOFF_EVENT:	leaveLobbyForShutdown("shutting down"); break;
		default: return FALSE;
	}
	// FALSE: we only wanted to say goodbye, the default handler still ends
	// the process. Returning TRUE here would leave a killed bot running.
	return FALSE;
}
#endif

static void botSignalHandler( int sig )
{
	leaveLobbyForShutdown(sig == SIGINT ? "interrupted" : "terminated");
	// Restore the default and re-raise, so the exit status is the one the
	// caller expects from a signal rather than a plain 0.
	signal(sig, SIG_DFL);
	raise(sig);
}

/**
	Arm the shutdown handlers. Called once, from both roles, after TheLAN
	exists -- before that there is nothing to leave.

	Wine delivers a `kill` as a POSIX signal to the emulated process, and a
	console Ctrl-C through the Win32 console path, so both are wired: which
	one fires depends on how the bot was stopped, and the launcher script
	uses `kill`.
*/
static void armShutdownHandlers()
{
	static Bool armed = FALSE;
	if (armed)
		return;
	armed = TRUE;

	signal(SIGINT,  botSignalHandler);
	signal(SIGTERM, botSignalHandler);
#ifdef _WIN32
	SetConsoleCtrlHandler(botConsoleCtrlHandler, TRUE);
#endif
}

// peerIP 0 means "no particular peer" -- the host case.
Bool startLan(UnsignedInt peerIP)
{
	delete TheLAN;
	TheLAN = NEW LANAPI();
	TheLAN->init();

	// There is now a lobby to leave, so make being killed say so. Both
	// roles come through here, and a killed HOST strands its joiners the
	// same way a killed joiner strands the host.
	armShutdownHandlers();

	UnsignedInt ip = 0;
	if (!TheGlobalData->m_netLocalIP.isEmpty())
	{
		// Octets, not inet_addr, because LANAPI works in host byte order
		// throughout -- inet_addr would give network order and silently
		// reach the wrong machine.
		if (!parseIPv4(TheGlobalData->m_netLocalIP.str(), &ip))
		{
			printf("BotNet: '%s' is not an address I can parse\n",
				TheGlobalData->m_netLocalIP.str());
			return FALSE;
		}
	}
	else
	{
		/*	Pick the interface that can actually reach the peer.

			The menu falls back to "the first enumerated address", and that
			is a real trap on any machine with more than one interface:
			gethostbyname returns them in no meaningful order, so a VPN
			tunnel, a VM host-only adapter (192.168.56.x), a WSL vEthernet
			or a second NIC can easily come first. Bind to one of those and
			the failure is silent -- we stamp that address into the join
			request, the host addresses JOIN_ACCEPT back to it, and
			handleJoinAccept's `playerIP == m_localIP` test can never match
			for anyone on the real LAN. No error is printed anywhere.

			When we know who we are trying to reach we do not have to
			guess: prefer an address on the same /24, then the same /16.
			That is the same containment test a human makes by eye when
			they notice one box is 174.x and the other is 192.168.x.
		*/
		IPEnumeration IPs;
		EnumeratedIP *list = IPs.getAddresses();

		/*	Only a /24 is treated as proof; a /16 is a guess.

			A /16 tier looks helpful and is actively harmful on a home
			LAN: 192.168.1.x (the real network) and 192.168.56.x (the
			usual VirtualBox host-only adapter) share 192.168/16, so the
			adapter would score a match and be chosen with no warning --
			exactly the silent wrong-interface failure this code exists
			to prevent, one octet subtler. So a /16 is still preferred
			over nothing, but it is NOT treated as certain: anything less
			than a /24 match warns and shows the alternatives.
		*/
		EnumeratedIP *best = nullptr;
		Int bestScore = -1;
		for (EnumeratedIP *e = list; e != nullptr; e = e->getNext())
		{
			const UnsignedInt cand = e->getIP();
			Int score = 0;
			if (peerIP != 0)
			{
				if ((cand & 0xffffff00) == (peerIP & 0xffffff00))
					score = 3;					// same /24 -- same wire
				else if ((cand & 0xffff0000) == (peerIP & 0xffff0000))
					score = 2;					// same /16 -- plausible, not proof
			}
			else
			{
				/*	No peer to match: we are DISCOVERING, and a broadcast
					has to leave the machine. Loopback cannot reach another
					host, so binding to 127.x -- which is what "first
					enumerated address" gave us -- meant the search ran
					forever with nobody able to answer. Prefer a private
					LAN address, take any other real one over loopback, and
					keep loopback only as a last resort so joining a game
					hosted on this same box still works.
				*/
				const UnsignedInt a = (cand >> 24) & 0xff;
				const UnsignedInt b = (cand >> 16) & 0xff;
				if (a == 127)
					score = 0;					// loopback: last resort
				else if (a == 192 && b == 168)
					score = 3;					// home LAN
				else if (a == 10 || (a == 172 && b >= 16 && b <= 31))
					score = 3;					// the other private ranges
				else
					score = 1;					// routable: better than loopback
			}
			if (score > bestScore)
			{
				bestScore = score;
				best = e;
			}
		}

		if (best != nullptr)
			ip = best->getIP();

		// Say what we chose and what else was on offer whenever we are
		// not certain. A wrong interface is the single most common reason
		// two machines never find each other, and it must never be silent.
		if (peerIP != 0 && bestScore < 3)
		{
			if (bestScore == 2)
				printf("BotNet: WARNING no interface on the same /24 as %d.%d.%d.%d;"
					" guessing %s\n",
					PRINTF_IP_AS_4_INTS(peerIP),
					best ? best->getIPstring().str() : "(none)");
			else
				printf("BotNet: WARNING no local interface shares a subnet with %d.%d.%d.%d\n",
					PRINTF_IP_AS_4_INTS(peerIP));
			printf("BotNet: local addresses are:\n");
			for (EnumeratedIP *e = list; e != nullptr; e = e->getNext())
				printf("BotNet:   %s\n", e->getIPstring().str());
			printf("BotNet: pass -netlocalip <address> to choose one explicitly.\n");
			fflush(stdout);
		}
	}

	if (ip != 0 && !TheLAN->SetLocalIP(ip))
	{
		printf("BotNet: could not bind to %d.%d.%d.%d\n", PRINTF_IP_AS_4_INTS(ip));
		return FALSE;
	}
	printf("BotNet: bound to %d.%d.%d.%d\n", PRINTF_IP_AS_4_INTS(TheLAN->GetLocalIP()));
	if (peerIP != 0)
		printf("BotNet: peer is %d.%d.%d.%d\n", PRINTF_IP_AS_4_INTS(peerIP));

	UnicodeString botName;
	botName.translate(TheGlobalData->m_botJoinName);
	botName.truncateTo(g_lanPlayerNameLength);
	TheLAN->RequestSetName(botName);

	AsciiString a2;
	a2.translate(botName);
	std::string w;
	for (const char *p = a2.str(); p && *p; ++p)
		w += (char)tolower((unsigned char)*p);
	if (!w.empty())
		s_botWord = w;

	// The menu leaves the lobby before hosting or joining directly.
	TheLAN->RequestLobbyLeave(true);
	return TRUE;
}

/**
	Resolve a plain map name ("Alpine Assault") to its map cache entry.

	Same two-step lookup the skirmish launcher does: the cache is keyed by
	full lower case path, and those differ between the user maps directory
	and the .big archives, so an exact key cannot be built from a plain
	name. Match the "<name>/<name>.map" tail, which is unique to both.
*/
const MapMetaData *findMapByName( const AsciiString& mapName, AsciiString& keyOut )
{
	TheMapCache->updateCache();

	AsciiString lower = mapName;
	lower.toLower();
	const MapMetaData *md = TheMapCache->findMap(lower);
	if (md != nullptr)
	{
		keyOut = lower;
		return md;
	}

	AsciiString tail, tailFwd;
	tail.format("%s\\%s.map", lower.str(), lower.str());
	tailFwd.format("%s/%s.map", lower.str(), lower.str());

	for (MapCache::const_iterator it = TheMapCache->begin();
			 it != TheMapCache->end(); ++it)
	{
		if (it->first.endsWithNoCase(tail.str()) ||
				it->first.endsWithNoCase(tailFwd.str()))
		{
			keyOut = it->first;
			return &it->second;
		}
	}
	return nullptr;
}

/**
	Pump the match to its end.

	TheNetwork is live here, so the ordering in GameEngine::update() matters:
	the action server is polled just before TheNetwork->update() drains
	TheCommandList, which is the only reason an agent's orders reach the
	other players at all. propagateMessages() is needed because the LAN
	layer posts to TheMessageStream, not TheCommandList.
*/
Int playMatch( const char *who )
{
	const DWORD startMillis = GetTickCount();

	while (!TheGameEngine->getQuitting())
	{
		LANbuttonPushed = FALSE;
		TheLAN->update();

		/*	Run the engine's own frame, rather than a copy of it.

			GameEngine::update() is public and virtual, and it is exactly
			the per-frame contract every other player on the wire is
			keeping: radar, audio, client, propagateMessages, then (with a
			network) the action server and TheNetwork, and only then the
			logic -- gated by canUpdateGameLogic(), which also calls
			GameLogic::preUpdate() and drives TheFramePacer.

			A hand-written version of this loop is what dropped the bot
			from real matches, in two ways that bot-vs-bot could never
			show, because both bots had the same defect and so agreed with
			each other:

			  * It called propagateMessages() BEFORE TheNetwork->update().
			    Network::update() ends by relaying the frame's commands ONTO
			    TheCommandList; draining first meant every frame ran a frame
			    behind the host's, which is a desync, which the host reports
			    as a player that dropped.

			  * It never called TheActionServer->update(). GameLogic polls
			    the action server only when TheNetwork is null, so on this
			    path the agent's orders were never picked up at all.

			TheFramePacer->update() lives in GameEngine::execute(), outside
			update(), and is the only sleep in the engine -- so it belongs
			here too or this process spins a core.
		*/
		TheGameEngine->update();
		TheFramePacer->update();

		const UnsignedInt frame = TheGameLogic->getFrame();
		if (frame > 1 && !TheGameLogic->isInGame())
		{
			printf("%s: game over\n", who);
			break;
		}

		const UnsignedInt limit = TheGlobalData->m_skirmishMaxFrames;
		if (limit != 0 && frame >= limit)
		{
			printf("%s: reached the frame limit of %d\n", who, limit);
			break;
		}

		/*	Report on a CHANGE of interval, not on the frame number itself.
			GameEngine::update() advances the logic only when the frame
			clock allows, so most iterations leave the frame where it was --
			and "frame % reportEvery == 0" then prints on every one of them,
			which reads exactly like a frozen game.
		*/
		static UnsignedInt lastReported = 0;
		const Int reportEvery = 5 * 60 * LOGICFRAMES_PER_SECOND;
		if (frame != 0 && frame / reportEvery != lastReported / reportEvery)
		{
			lastReported = frame;
			const UnsignedInt gameSec = frame / LOGICFRAMES_PER_SECOND;
			const UnsignedInt realSec = (GetTickCount() - startMillis) / 1000;
			printf("Elapsed Time: %02d:%02d Game Time: %02d:%02d\n",
				realSec / 60, realSec % 60, gameSec / 60, gameSec % 60);
			fflush(stdout);
		}
	}

	if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
	{
		TheRecorder->stopRecording();
		printf("%s: replay written to %s%s%s\n", who,
			RecorderClass::getReplayDir().str(),
			RecorderClass::getLastReplayFileName().str(),
			RecorderClass::getReplayExtention().str());
	}

	printf("%s: finished\n", who);
	return 0;
}

} // namespace

//-------------------------------------------------------------------------------------------------
Bool BotLanJoin::handleChat( const UnicodeString& speaker, const UnicodeString& text )
{
	// Either role answers chat: a bot host takes the same commands a bot
	// joiner does. This used to test m_botJoinEnabled, which meant a bot
	// HOST ignored everything said to it.
	if (s_role == ROLE_NONE)
		return FALSE;

	{
		AsciiString heard;
		heard.translate(text);
		printf("BotChat: < %s\n", heard.str());
		fflush(stdout);
	}

	std::string line = toLowerNarrow(text);

	// Strip leading spaces, then require our name as the first word, so
	// people can talk to each other without tripping the bot.
	size_t i = line.find_first_not_of(' ');
	if (i == std::string::npos)
		return FALSE;
	line = line.substr(i);

	if (line.compare(0, s_botWord.size(), s_botWord) != 0)
		return FALSE;
	line = line.substr(s_botWord.size());
	i = line.find_first_not_of(' ');
	if (i == std::string::npos)
	{
		reportSlot();
		return TRUE;
	}
	line = line.substr(i);

	// One verb, optionally one argument.
	std::string verb = line;
	std::string arg;
	size_t sp = line.find(' ');
	if (sp != std::string::npos)
	{
		verb = line.substr(0, sp);
		size_t j = line.find_first_not_of(' ', sp);
		if (j != std::string::npos)
			arg = line.substr(j);
	}

	if (verb == "help")
	{
		{
			std::string m = "commands (say '" + s_botWord + " <cmd>'): "
				"faction <china|america|gla>, team <1-4|none>, "
				"color <n>, start <1-8|any>, status";
			m += (s_role == ROLE_HOST) ? ", start" : ", ready, unready";
			say(m.c_str());
		}
		return TRUE;
	}
	if (verb == "status")
	{
		reportSlot();
		return TRUE;
	}
	if (verb == "faction" || verb == "side" || verb == "army")
	{
		const Int idx = factionIndexFromWord(arg);
		if (idx < 0)
		{
			say("I do not know that faction");
			return TRUE;
		}
		requestOption("PlayerTemplate", idx);
		return TRUE;
	}
	if (verb == "team")
	{
		/*	Say the number the LOBBY shows, not the internal one.

			The team combo box labels its entries "Team:c+1" while storing
			c, and the load screen prints getTeamNumber()+1 -- so the UI's
			"Team 1" is stored as 0. Passing the user's number straight
			through made "team 1" land on the slot the lobby calls Team 2.

			Entry 0 of that combo is "no team", stored as -1, so accept 0
			and "none" for that.
		*/
		const std::string a = arg;
		Int shown = atoi(a.c_str());
		if (a == "none" || a == "no" || a == "-" || shown == 0)
		{
			requestOption("Team", -1);
			say("team none");
			return TRUE;
		}
		if (shown < 1 || shown > MAX_SLOTS / 2)
		{
			char m[96];
			snprintf(m, sizeof(m), "team must be 1-%d, or none", MAX_SLOTS / 2);
			say(m);
			return TRUE;
		}
		requestOption("Team", shown - 1);
		return TRUE;
	}
	if (verb == "color" || verb == "colour")
	{
		requestOption("Color", atoi(arg.c_str()));
		return TRUE;
	}
	if (verb == "start" || verb == "pos" || verb == "position")
	{
		/*	Start spots are shown 1-based on the map preview, but stored
			0-based (ButtonMapStartPosition0 is the first). Same treatment
			as team: take the number the human reads off the screen.
		*/
		{
			const std::string a = arg;
			Int shown = atoi(a.c_str());
			if (a == "none" || a == "any" || a == "-" || shown == 0)
			{
				requestOption("StartPos", -1);
				say("start position any");
				return TRUE;
			}
			if (shown < 1 || shown > MAX_SLOTS)
			{
				char m[96];
				snprintf(m, sizeof(m), "start must be 1-%d, or any", MAX_SLOTS);
				say(m);
				return TRUE;
			}
			requestOption("StartPos", shown - 1);
			return TRUE;
		}
	}
	if (verb == "ready" || verb == "accept")
	{
		if (TheLAN == nullptr)
			return TRUE;

		if (s_role == ROLE_HOST)
		{
			// A host has nothing to accept -- setSlot marks slot 0
			// accepted already. "ready" from a host means START, which is
			// the host's decision to make and nobody else's.
			say("I am the host; say 'start' when everyone is ready");
			return TRUE;
		}

		TheLAN->RequestAccept();
		s_accepted = TRUE;
		say("ready");
		return TRUE;
	}
	if (verb == "unready")
	{
		if (s_role == ROLE_HOST)
		{
			say("I am the host; I do not ready up");
			return TRUE;
		}

		// There is no "unaccept" message; changing any option clears the
		// accepted flag for everyone (GameInfo::resetAccepted), so ask for
		// the colour we already have. The host echoes a fresh slot list.
		LANGameInfo *game = (TheLAN != nullptr) ? TheLAN->GetMyGame() : nullptr;
		const Int slotNum = (game != nullptr) ? game->getLocalSlotNum() : -1;
		const GameSlot *slot = (slotNum >= 0) ? game->getConstSlot(slotNum) : nullptr;
		if (slot != nullptr)
			requestOption("Color", slot->getColor());
		s_accepted = FALSE;
		say("not ready");
		return TRUE;
	}
	if (verb == "say")
	{
		say(arg.c_str());
		return TRUE;
	}

	{
		std::string m = "I did not understand that; try: " + s_botWord + " help";
		say(m.c_str());
	}
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Int BotLanJoin::runLanHost()
{
	s_role = ROLE_HOST;

	if (!TheGlobalData->m_lobbyScript.isEmpty())
		loadLobbyScript(TheGlobalData->m_lobbyScript.str());

	const AsciiString mapName = TheGlobalData->m_botHostMap;
	if (mapName.isEmpty())
	{
		printf("BotHost: no map, use -nethost \"Alpine Assault\"\n");
		return 1;
	}

	if (!startLan(0))
		return 1;					// startLan already said why

	AsciiString mapKey;
	const MapMetaData *md = findMapByName(mapName, mapKey);
	if (md == nullptr)
	{
		printf("BotHost: could not find map \"%s\"\n", mapName.str());
		return 1;
	}

	printf("BotHost: hosting \"%s\" as '%s'\n", mapName.str(),
		TheGlobalData->m_botJoinName.str());
	fflush(stdout);

	// HostDirectConnectGame(): the game's NAME is the host's own IP string
	// and isDirectConnect is TRUE. Both matter -- a joiner reaches a direct
	// connect game by address, and handleRequestGameInfo replies only for
	// the game whose slot 0 IP is ours.
	UnicodeString localIPString;
	localIPString.format(L"%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(TheLAN->GetLocalIP()));
	// A plain LAN game (what the Network menu makes) answers joins by
	// broadcast; a direct connect game unicasts to each slot. They are
	// different code paths and the bot must work in both.
	const Bool directConnect = !TheGlobalData->m_botHostLanLobby;
	TheLAN->RequestGameCreate(localIPString, directConnect);
	printf("BotHost: %s game\n", directConnect ? "direct connect" : "LAN lobby");
	fflush(stdout);

	LANGameInfo *game = TheLAN->GetMyGame();
	if (game == nullptr)
	{
		printf("BotHost: could not create the game\n");
		return 1;
	}

	// RequestGameCreate takes the map from LANPreferences; say what we
	// actually want, with the CRC and size a joiner checks against.
	game->setMap(mapKey);
	game->setMapCRC(md->m_CRC);
	game->setMapSize(md->m_filesize);
	game->setSeed(GetTickCount());

	/*	Open the slots we want other people to be able to take.

		RequestGameCreate leaves every slot but the host's CLOSED, and the
		LAN options menu never opens them directly either -- the commented
		out setState(SLOT_OPEN) in LanGameOptionsMenu.cpp is replaced by a
		side effect: it adds "Open" as entry 0 of each player combo box and
		selects position 0, and the gadget callback is what moves the slot
		to SLOT_OPEN. With no shell there are no combo boxes and no
		callback, so the slots stay closed, handleRequestJoin finds no
		isOpen() slot, and every join is answered RET_GAME_FULL -- which
		looks exactly like a network failure from the joiner's side,
		because the deny carries no text.

		-nethostplayers says how many humans this game seats, the host
		included; open that many minus ourselves.
	*/
	const Int seats = max(2, TheGlobalData->m_botHostPlayers);
	for (Int q = 1; q < MAX_SLOTS; ++q)
		game->getLANSlot(q)->setState(q < seats ? SLOT_OPEN : SLOT_CLOSED);
	printf("BotHost: %d seats, slots 1..%d open\n", seats, seats - 1);
	fflush(stdout);

	/*	`slots` seats AI players, so a bot can host a 2v2 against the skirmish
		AI with only one other machine in the lobby. One entry per slot from
		slot 1 on; slot 0 is us. Anything not named stays as the seat loop
		above left it.

		This is the host branch of the options menu: only the host may write
		another player's slot, and it publishes the whole list afterwards.
		The combo box entries are the SlotState enum in order, which is why
		the menu hands the selected position straight to setState() -- we do
		the same thing, having turned a word into that same enum.

		`players` must still count every SEAT the lobby has, humans and AI
		alike, or the host opens fewer slots than the config describes and
		then waits for a joiner that has nowhere to sit.
	*/
	Int aiSeated = 0;
	if (!TheGlobalData->m_botHostSlots.isEmpty())
	{
		const std::vector<std::string> want =
			splitCommaList(TheGlobalData->m_botHostSlots);
		for (size_t w = 0; w < want.size(); ++w)
		{
			const Int slotNum = (Int)w + 1;		// slot 0 is the host
			if (slotNum >= MAX_SLOTS)
			{
				printf("BotHost: slots list is longer than the %d slots a game"
					" has; ignoring the rest\n", MAX_SLOTS);
				break;
			}
			if (want[w].empty())
				continue;

			SlotState st;
			if (!slotStateFromWord(want[w], &st))
			{
				printf("BotHost: slots entry %d is '%s', which I do not know;"
					" leaving that slot alone. Use open, closed, easy,"
					" medium or hard.\n", slotNum, want[w].c_str());
				continue;
			}

			GameSlot *slot = game->getLANSlot(slotNum);
			if (slot == nullptr)
				continue;
			slot->setState(st);
			if (slot->isAI())
				++aiSeated;
			printf("BotHost: slot %d = %s\n", slotNum, want[w].c_str());
		}

		/*	Every AI takes one of the seats we opened, so a config that
			seats as many AI as it has seats leaves nowhere for a joiner --
			the host would start alone against them.
		*/
		if (aiSeated > 0 && seats - aiSeated < 2)
			printf("BotHost: WARNING players=%d with %d AI seated leaves no"
				" seat for a joiner; raise players to %d\n",
				seats, aiSeated, aiSeated + 2);
	}

	applyConfiguredTeams(game, "seating");

	TheLAN->RequestGameAnnounce();

	// A host already owns slot 0, so it can take its faction right away --
	// no waiting to be seated, as a joiner has to.
	applyConfiguredFaction();

	// -- lobby: wait for the other players, then start ---------------------
	// Same floor as `seats` above: the two must agree, or the host opens
	// two seats and then waits for a number of players it never sized the
	// game for.
	/*	How many HUMAN seats to wait for.

		`present` below counts only human slots, because an AI is never
		"present" and never accepts -- it is simply written into the slot
		list by the host. Our own bots are human players in every sense the
		lobby cares about: they take a SLOT_PLAYER seat, and they accept.

		So the number to wait for is the seats we opened MINUS the ones we
		filled with AI ourselves. Leaving this as `seats` meant a 2v2 against
		two medium AIs waited for four humans, and sat in the lobby until the
		ten-minute timeout.
	*/
	const Int wantPlayers = max(1, seats - aiSeated);
	const DWORD lobbyStart = GetTickCount();
	const DWORD lobbyTimeoutMs = 10 * 60 * 1000;
	Bool started = FALSE;

	// Per-lobby state. These were function-level statics, which would have
	// carried readiness and the last printed counts into a second lobby in
	// the same process.
	// VC6 leaks for-scoped variables into the enclosing block, and `q` is
	// already taken by the seat-opening loop above.
	Bool everReady[MAX_SLOTS];
	for (Int r = 0; r < MAX_SLOTS; ++r)
		everReady[r] = FALSE;
	Int lastPresent = -1, lastReady = -1;

	while (!TheGameEngine->getQuitting() && !TheGameLogic->isInGame())
	{
		// Nothing clears this headless; see the note on the extern above.
		LANbuttonPushed = FALSE;
		TheLAN->setIsActive(TRUE);
		TheLAN->update();
		TheMessageStream->propagateMessages();

		/*	Once the start message is out, pump the logic.

			OnGameStart posts MSG_NEW_GAME to TheMessageStream, and the
			only thing that consumes it is GameLogicDispatch, reached
			from TheGameLogic->UPDATE(). A lobby loop that waits for
			isInGame() while never updating the logic waits forever: the
			message that would set it is sitting in the queue. (The
			skirmish launcher sidesteps this by putting MSG_NEW_GAME
			straight on TheCommandList and pumping the logic itself.)
		*/
		/*	Once the match is underway, hand the frame to the engine
			rather than pumping the logic by hand: from here on this
			process is a peer on a shared frame clock, and the ordering
			inside GameEngine::update() is the contract. See playMatch().
		*/
		if (started)
		{
			TheGameEngine->update();
			TheFramePacer->update();
		}

		/*	Drive the scripted lobby, if we were given one.

			-lobbyscript is how the automated test plays the part of a
			person in the lobby: it says a line, waits, says the next.
			Lines go out as ordinary chat, so the joiner sees exactly
			what it would see from a human typing.

			A line starting with "!" is a local directive rather than
			chat: "!wait <ms>" pauses, "!start" forces the match to
			start. Anything else is said out loud.
		*/
		if (!s_script.empty() && GetTickCount() >= s_scriptNextAt)
		{
			while (s_scriptAt < s_script.size())
			{
				const std::string line = s_script[s_scriptAt++];
				if (line.empty())
					continue;
				if (line[0] == '!')
				{
					if (line.compare(0, 5, "!wait") == 0)
					{
						s_scriptNextAt = GetTickCount()
							+ (DWORD)atoi(line.c_str() + 5);
						break;
					}
					if (line == "!start")
					{
						printf("BotHost: script says start\n"); fflush(stdout);
						TheLAN->RequestGameAnnounce();
						TheLAN->RequestGameStart();
						started = TRUE;
						break;
					}
					continue;
				}
				printf("BotHost: script says '%s'\n", line.c_str());
				fflush(stdout);
				say(line.c_str());
				s_scriptNextAt = GetTickCount() + 500;
				break;
			}
		}

		// A scripted host starts only when the script says so, so the
		// test can exercise the lobby for as long as it likes.
		if (!started && s_script.empty())
		{
			/*	Wait for everyone to be present AND ready.

				Counting humans is not enough: the host refuses to start
				while any human slot is unaccepted -- that is
				LanGameOptionsMenu's start gate,
				`if (slot->isHuman() && !slot->isAccepted()) isReady=false`.
				So a lobby with every seat filled can still sit there
				forever. Bots accept as soon as they are seated; a human
				clicks Accept.
			*/
			/*	Latch readiness per slot.

				Accepts do not all hold true in the same tick: a joiner
				accepts, the host records it and broadcasts the new slot
				list, and the join/options path resets accepts again --
				so polling for "everyone accepted right now" can spin
				forever while each player is, in fact, ready. Remember
				that a slot HAS accepted since it was seated, and clear
				the latch only when the slot stops being human (someone
				left and the seat reopened).
			*/
			Int present = 0, ready = 0;
			for (Int i = 0; i < MAX_SLOTS; ++i)
			{
				const GameSlot *slot = game->getConstSlot(i);
				if (slot == nullptr || !slot->isHuman())
				{
					everReady[i] = FALSE;
					continue;
				}
				++present;
				if (slot->isAccepted())
					everReady[i] = TRUE;
				if (everReady[i])
					++ready;
			}

			if (present != lastPresent || ready != lastReady)
			{
				lastPresent = present;
				lastReady = ready;
				printf("BotHost: %d/%d present, %d ready\n",
					present, wantPlayers, ready);
				fflush(stdout);
			}

			if (present >= wantPlayers && ready >= present)
			{
				printf("BotHost: %d players ready, starting\n", present);
				fflush(stdout);

				/*	Re-apply the teams now that everybody is seated.

					Seating REPLACES a slot wholesale -- handleRequestJoin
					builds a fresh LANGameSlot and setSlot()s it -- and a
					fresh slot has no team. The pass we did before opening
					the lobby therefore only stuck for slots nobody joined,
					which in a 2v2 meant the two AI kept their teams and
					the two human players lost theirs. Doing it here, after
					the last join and before the announce, is the only
					point where the slot list is both complete and still
					ours to change.
				*/
				applyConfiguredTeams(game, "start");

				/*	Say what the slot list ACTUALLY holds, not what we
					asked for. The teams bug hid behind a log that printed
					the write rather than the result, so it read as correct
					for a whole 30-minute match that was in fact a
					free-for-all. Print state, not intent.
				*/
				for (Int sl = 0; sl < MAX_SLOTS; ++sl)
				{
					const GameSlot *gs = game->getConstSlot(sl);
					if (gs == nullptr || !gs->isOccupied())
						continue;
					char t[16];
					if (gs->getTeamNumber() < 0)
						strcpy(t, "none");
					else
						snprintf(t, sizeof(t), "%d", gs->getTeamNumber() + 1);
					printf("BotHost: FINAL slot %d: %s, team %s\n", sl,
						gs->isAI() ? "AI" : "player", t);
				}
				fflush(stdout);

				// Everyone must be marked ready or the host refuses; the
				// bots accept as soon as they are told to, and a human
				// clicks it. Announce first so the final slot list is out.
				TheLAN->RequestGameAnnounce();
				TheLAN->RequestGameStart();
				started = TRUE;
			}
		}

		if (GetTickCount() - lobbyStart > lobbyTimeoutMs)
		{
			printf("BotHost: nobody joined; giving up\n");
			TheLAN->RequestGameLeave();
			return 1;
		}

		Sleep(16);
	}

	if (!TheGameLogic->isInGame())
		return 1;

	printf("BotHost: the game has started\n");
	fflush(stdout);
	return playMatch("BotHost");
}

//-------------------------------------------------------------------------------------------------
Int BotLanJoin::runLanJoin()
{
	s_role = ROLE_JOINER;

	AsciiString host = TheGlobalData->m_botJoinHost;
	UnsignedInt hostIP = 0;
	UnsignedShort hostPort = 0;		///< 0 = not told; use the default lobby port

	/*	"any": find a lobby instead of being told where one is.

		Direct connect needs a dotted quad, which means knowing in advance
		which machine is hosting -- fine for a scripted test, useless when
		somebody just opened a game and wants the bot in it. LANAPI already
		does discovery for the game's own LAN browser: RequestLocations()
		broadcasts, hosts answer, and the replies accumulate in the game
		list. This waits on that list and takes the first game that has a
		host address, which is exactly what a player does when they see one
		entry in the browser and double-click it.

		Discovery is read-only -- GetGames() walks the list LANAPI already
		built and creates nothing.
	*/
	const Bool discover = host.isEmpty() ||
		!stricmp(host.str(), "any") || !stricmp(host.str(), "auto");

	if (discover)
	{
		// No peer to match an interface against, so LANAPI picks by its own
		// rule (and -netlocalip still overrides). The host path does the
		// same with startLan(0).
		if (!startLan(0))
			return 1;					// startLan already said why

		printf("BotJoin: looking for a game on the LAN\n");
		fflush(stdout);

		const DWORD findStart = GetTickCount();
		const DWORD findTimeoutMs = 10 * 60 * 1000;
		DWORD lastAsk = 0;
		Int asks = 0;
		while (hostIP == 0)
		{
			if (GetTickCount() - findStart > findTimeoutMs)
			{
				printf("BotJoin: no game appeared on the LAN in %d minutes\n",
					(Int)(findTimeoutMs / 60000));
				return 1;
			}
			// Ask periodically: a host that starts AFTER we do must still
			// be found, and a single broadcast at startup would miss it.
			if (lastAsk == 0 || GetTickCount() - lastAsk > 2000)
			{
				lastAsk = GetTickCount();
				++asks;
				TheLAN->RequestLocations();
				if (asks % 15 == 0)
				{
					printf("BotJoin: still looking (%d)\n", asks);
					fflush(stdout);
				}
			}
			TheLAN->update();
			TheFramePacer->update();

			for (LANGameInfo *g = TheLAN->GetGames(); g != nullptr; g = g->getNext())
			{
				const UnsignedInt ip = g->getIP(0);		// slot 0 is the host
				if (ip == 0 || ip == TheLAN->GetLocalIP())
					continue;						// no address, or ourselves
				hostIP = ip;
				AsciiString found;
				found.translate(g->getName());
				printf("BotJoin: found '%s' at %d.%d.%d.%d\n",
					found.str(), PRINTF_IP_AS_4_INTS(hostIP));
				fflush(stdout);
				break;
			}
		}
		host.format("%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(hostIP));
	}
	else
	{
		// JoinDirectConnectGame(): octets shifted into host byte order, which
		// is what LANAPI works in throughout. inet_addr would give network
		// order and silently reach the wrong machine.
		//
		// "host = 10.0.0.5:8087" names the host's lobby port as well, for a
		// host that moved off the default. 0 means "not given"; see below.
		if (!parseIPv4WithPort(host.str(), &hostIP, &hostPort) || hostIP == 0)
		{
			printf("BotJoin: '%s' is not an address I can parse\n", host.str());
			return 1;
		}

		if (!startLan(hostIP))
			return 1;					// startLan already said why
	}

	/*	Refuse to share BOTH an address and a port with the host.

		LANAPI was written assuming one engine per machine -- "everyone has a
		unique IP, so it's ok to use the same port" -- so originally sharing an
		address at all was fatal: two engines fought over the lobby port and
		the transport bind, and the host was the one that died.

		Now that the lobby port is configurable, the real requirement is
		narrower: the pair (address, port) has to be unique, not the address.
		Distinct loopback addresses still work and remain the simplest answer,
		but a distinct lanPort is now equally valid -- which matters on a host
		with no spare addresses to hand out.

		The game transport is the reason an address clash is still checked at
		all: ConnectionManager binds NETWORK_BASE_PORT_NUMBER on the local
		address, and that port is NOT configurable, so two engines on one
		address would still collide once the match itself started -- just later
		and far more confusingly than a refused bind.
	*/
	if (TheLAN->GetLocalIP() == hostIP)
	{
		printf("BotNet: I bound to %d.%d.%d.%d, which is the host's own address.\n",
			PRINTF_IP_AS_4_INTS(hostIP));
		printf("BotNet: two engines cannot share one address: the in-game transport\n");
		printf("BotNet: binds port %d on it and that port is not configurable.\n",
			NETWORK_BASE_PORT_NUMBER);
		printf("BotNet: If the host is on THIS machine, give us a different one --\n");
		printf("BotNet: the whole 127/8 range is routable and needs no setup:\n");
		printf("BotNet:     host = 127.0.0.1   localIP = 127.0.0.2\n");
		return 1;
	}

	/*	Aim the first datagram at the host's real port.

		Everything after this learns ports from inbound packets, but the join
		request IS the first packet, so there is nothing to have learned yet
		and peerPort() would fall back to the default. Only the explicit
		host=ip:port path can know better; discovery does not need to, because
		the announce that revealed the host also taught us its port.
	*/
	if (hostPort != 0)
	{
		printf("BotJoin: host lobby port is %d\n", hostPort);
		TheLAN->SetPeerPort(hostIP, hostPort);
	}

	printf("BotJoin: joining %s as '%s'\n", host.str(), TheGlobalData->m_botJoinName.str());
	printf("BotJoin: say '%s help' in the lobby for commands\n", s_botWord.c_str());
	fflush(stdout);

	TheLAN->RequestGameJoinDirectConnect(hostIP);

	// -- lobby: pump the LAN layer until the host starts the match ----------
	//
	// A real client does this from the lobby window. There is no window here,
	// so this loop is the lobby. propagateMessages() matters because
	// LANAPI::OnGameStart() posts MSG_NEW_GAME to TheMessageStream, not to
	// TheCommandList.
	const DWORD lobbyStart = GetTickCount();
	const DWORD lobbyTimeoutMs = 15 * 60 * 1000;

	/*	Keep asking until a host answers.

		RequestGameJoinDirectConnect sends ONE unicast MSG_REQUEST_GAME_INFO
		and nothing retransmits it. If the host is not up yet -- which is the
		normal case, since a human needs time to get to the lobby -- that
		datagram lands on a closed port and is gone. The loop below would then
		pump an empty lobby for fifteen minutes looking perfectly healthy:
		GetMyGame() stays null and nothing ever asks again. That is a pure
		startup race, and on a real LAN it is worse than on loopback, where
		both ends are at least started by the same script.

		So re-send while we have no game. The interval sits just past LANAPI's
		own m_actionTimeout (5s), because a request issued while
		m_pendingAction is still set is answered with RET_BUSY and never
		reaches the wire -- retrying faster than the timeout would spin
		without sending anything.
	*/
	const DWORD joinRetryMs = 6000;
	DWORD lastJoinAttempt = GetTickCount();
	Int joinAttempts = 1;

	while (!TheGameEngine->getQuitting() && !TheGameLogic->isInGame())
	{
		LANbuttonPushed = FALSE;
		TheLAN->setIsActive(TRUE);
		TheLAN->update();
		TheMessageStream->propagateMessages();

		/*	Pump the logic once the host has started us.

			Same reason as the host loop: OnGameStart (driven here by the
			host's MSG_GAME_START) posts MSG_NEW_GAME to TheMessageStream,
			and only TheGameLogic->UPDATE() consumes it. TheNetwork is
			created by OnGameStart, so its presence is the signal that the
			match is underway.
		*/
		if (TheNetwork != nullptr)
		{
			// As in the host loop: the engine owns the frame now.
			TheGameEngine->update();
			TheFramePacer->update();
		}

		/*	Mark ourselves ready as soon as we have a slot.

			The host cannot start until EVERY human slot is accepted --
			LanGameOptionsMenu's start gate is
			`if (slot->isHuman() && !slot->isAccepted()) isReady = false`.
			A human clicks Accept; a bot has to say so itself, or the
			lobby sits there forever with everyone present and nobody
			ready. Re-send whenever the flag clears: the host resets all
			accepts on every options change (resetAccepted), so a single
			accept at join time would be silently undone by the next
			slot-list update.

			The same gate also consults hasMap(). Answer it HONESTLY: look
			the host's map up in our own cache and tell the host what we
			actually found, via RequestHasMap (the flag alone is local --
			the host learns it only from MSG_MAP_AVAILABILITY). Claiming
			a map we do not have gets us to the start line and then fails
			obscurely, because a headless client cannot receive a map
			transfer.
		*/
		LANGameInfo *myGame = TheLAN->GetMyGame();

		/*	No game yet: ask again, periodically, forever.

			See joinRetryMs above -- the first request is very likely to have
			been sent before the host existed. Announce each attempt, because
			the silent version of this is indistinguishable from a wrong
			interface or a firewall, and those are the other two reasons a LAN
			join goes nowhere.
		*/
		if (myGame == nullptr && GetTickCount() - lastJoinAttempt > joinRetryMs)
		{
			lastJoinAttempt = GetTickCount();
			++joinAttempts;
			printf("BotJoin: no answer from %s yet; retrying (attempt %d)\n",
				host.str(), joinAttempts);
			fflush(stdout);
			TheLAN->RequestGameJoinDirectConnect(hostIP);
		}

		if (myGame != nullptr)
		{
			const Int mySlot = myGame->getLocalSlotNum();
			if (mySlot >= 0)
			{
				/*	Say hello in the lobby the first time we have a slot.

					Nobody can see our stdout, so the lobby chat is the
					only place a human finds out this player is a bot and
					how to talk to it.
				*/
				if (!s_saidHello)
				{
					s_saidHello = TRUE;
					std::string greet = "hi, I am a bot -- say '" + s_botWord
						+ " help'. commands: faction <china|america|gla>, "
						"team <n>, color <n>, start <n>, ready, unready, status";
					say(greet.c_str());
				}

				/*	Ask for our configured faction before readying up.

					Order matters: the host resets every accept when the
					slot list changes (resetAccepted), so asking AFTER
					accepting would immediately un-ready us and the accept
					below would have to happen all over again.
				*/
				applyConfiguredFaction();

				LANGameSlot *me = myGame->getLANSlot(mySlot);
				if (me != nullptr && !me->isAccepted())
				{
					// The cache is keyed by LOWERCASE full path (see
					// findMapByName), so match its convention or a
					// mixed-case name reads as "missing".
					const AsciiString wanted = myGame->getMap();
					AsciiString wantedKey = wanted;
					wantedKey.toLower();
					const Bool haveMap = (TheMapCache != nullptr)
						&& (TheMapCache->findMap(wantedKey) != nullptr);

					if (!haveMap)
					{
						// Say it once, in BOTH places -- the lobby needs
						// to know why this player will not ready up, and
						// leaving would look like a network fault. The
						// host can change the map and we pick it up.
						if (s_noMapSaidFor != wanted)
						{
							s_noMapSaidFor = wanted;
							printf("BotJoin: I do not have the map '%s'; "
								"cannot ready up (headless cannot receive a "
								"map transfer)\n", wanted.str());
							fflush(stdout);
							std::string m = "I do not have the map '";
							m += wanted.str();
							m += "' -- please pick another; I cannot receive a transfer";
							say(m.c_str());
						}
						me->setMapAvailability(FALSE);
						TheLAN->RequestHasMap();
					}
					else
					{
						/*	Accept, but ONLY when the lobby actually changed
							since the last time we did.

							Accepting is not free: the host answers every
							accept with RequestGameOptions, and a client
							applies that by overwriting its whole slot list
							from the host's copy -- which CLEARS the accept
							flag we just set. Re-accepting on the strength of
							`!isAccepted()` alone therefore feeds itself:
							accept -> host republishes -> flag clears ->
							accept... The bot flooded a real lobby with
							accepts and map-status messages until the host
							gave up on it, which the host reports as a player
							that stopped responding.

							So key the decision on the lobby state we are
							responding to, not on our own flag. When the host
							genuinely changes something (map, a colour,
							someone joining) that signature changes and we
							accept exactly once more. A human behaves the same
							way: they click Accept when something changed, not
							continuously.
						*/
						const AsciiString sig = lobbySignature(myGame);
						if (sig != s_acceptedFor)
						{
							s_acceptedFor = sig;
							me->setMapAvailability(TRUE);
							TheLAN->RequestHasMap();
							TheLAN->RequestAccept();
							s_accepted = TRUE;
							s_noMapSaidFor.clear();
							if (!s_saidReady)
							{
								s_saidReady = TRUE;
								printf("BotJoin: seated in slot %d, ready\n", mySlot);
								fflush(stdout);
								say("ready");
							}
							else
							{
								/*	Re-ready after a real lobby change.

									Deliberately SILENT in chat: a lobby
									settles through several changes in a row
									and announcing each would bury the
									humans' own chat. stdout still records
									every one.
								*/
								printf("BotJoin: re-ready after a lobby change\n");
								fflush(stdout);
							}
						}
					}
				}
			}
		}

		if (GetTickCount() - lobbyStart > lobbyTimeoutMs)
		{
			printf("BotJoin: nobody started a game; giving up\n");
			TheLAN->RequestGameLeave();
			return 1;
		}

		// The lobby is not frame-paced; do not spin a core for it.
		Sleep(16);
	}

	if (!TheGameLogic->isInGame())
		return 1;

	s_gameStarted = TRUE;
	printf("BotJoin: the game has started\n");
	fflush(stdout);
	return playMatch("BotJoin");
}
