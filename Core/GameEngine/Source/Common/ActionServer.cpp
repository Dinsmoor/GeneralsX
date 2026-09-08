/*
**	Action server implementation. See ActionServer.h for the rationale.
**
**	Orders are injected as GameMessages on TheCommandList, exactly the way
**	Network.cpp injects commands arriving from remote players. That is the
**	single choke point GameLogic::update() consumes, so an injected order is
**	indistinguishable from a human's: it is CRC'd, recorded into replays and
**	validated for ownership by the normal dispatcher.
*/

#include "PreRTS.h"

#include "Common/ActionServer.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"        // getDefaultTeam(), for the sandbox spawn verb
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/SpecialPower.h"
#include "Common/Science.h"
#include "Common/Upgrade.h"
#include "Common/GameType.h"
#include "Common/NameKeyGenerator.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "Common/BuildAssistant.h"
#include "Common/ProductionPrerequisite.h"
#include "GameClient/ControlBar.h"

#include <string>
#include <vector>
#include <winsock2.h>

ActionServer *TheActionServer = nullptr;

// An order line longer than this is malformed; drop the client rather than
// grow without bound on a misbehaving agent.
// AsciiString caps out at MAX_LEN (32767) characters, so the pending buffer must
// stay below that; a line longer than this is malformed either way.
static const Int MAX_PENDING = 16 * 1024;

//-------------------------------------------------------------------------------------------------
// Minimal JSON output.
//
// Only the query verbs need to build a variable length reply; everything else
// answers through the fixed status/detail form. AsciiString::concat reallocates
// on every call, so bulk payloads are assembled in a std::string instead.
//-------------------------------------------------------------------------------------------------

static void appendInt( std::string& out, Int value )
{
	char buf[32];
	sprintf(buf, "%d", value);
	out.append(buf);
}

/**
	Append a JSON string body, escaping what the spec requires.

	Template and button names are plain identifiers, but a prerequisite's
	"Requires: ..." text is localized display text and may contain anything, so
	it cannot be pasted into a reply unescaped. Control characters are emitted as
	\u00XX; bytes above 0x7f are passed through, since the payload is read as
	UTF-8 and non-ASCII display text would otherwise be mangled.
*/
static void appendEscaped( std::string& out, const char *text )
{
	if (text == nullptr)
		return;

	for (const char *p = text; *p != '\0'; ++p)
	{
		const unsigned char c = (unsigned char)*p;
		switch (c)
		{
			case '"':  out.append("\\\""); break;
			case '\\': out.append("\\\\"); break;
			case '\n': out.append("\\n");  break;
			case '\r': out.append("\\r");  break;
			case '\t': out.append("\\t");  break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					sprintf(buf, "\\u%04x", (Int)c);
					out.append(buf);
				}
				else
				{
					out.append(1, (char)c);
				}
				break;
		}
	}
}

//-------------------------------------------------------------------------------------------------
// Minimal JSON scanning.
//
// The order format is deliberately flat -- a single object of scalar fields --
// so a full parser would be a liability rather than an asset here. These helpers
// find a top level key and read the scalar after it. Anything they cannot make
// sense of reports failure and the order is rejected.
//-------------------------------------------------------------------------------------------------

/**
	Find the value text following "key": in a flat JSON object, or nullptr.

	A quoted name can appear as either a key or a value -- {"action":"upgrade",
	"upgrade":"..."} contains "upgrade" twice -- so every occurrence is checked
	and only one actually followed by a colon is accepted. Matching the first
	occurrence blindly would silently read the wrong field.
*/
static const char *findValue( const char *json, const char *key )
{
	std::string needle = "\"";
	needle += key;
	needle += "\"";

	const char *p = json;
	while ((p = strstr(p, needle.c_str())) != nullptr)
	{
		const char *after = p + needle.length();
		while (*after == ' ' || *after == '\t')
			++after;

		if (*after != ':')
		{
			// This was a value, not a key; keep looking.
			p += needle.length();
			continue;
		}

		++after;
		while (*after == ' ' || *after == '\t')
			++after;

		return after;
	}

	return nullptr;
}

static Bool readString( const char *json, const char *key, AsciiString& out )
{
	const char *p = findValue(json, key);
	if (p == nullptr || *p != '"')
		return FALSE;

	++p;
	std::string value;
	while (*p != '\0' && *p != '"')
	{
		// Only the escapes the protocol can actually produce are honoured;
		// unit and template names never contain anything more exotic.
		if (*p == '\\' && *(p + 1) != '\0')
			++p;
		value += *p++;
	}
	if (*p != '"')
		return FALSE;

	out = value.c_str();
	return TRUE;
}

static Bool readInt( const char *json, const char *key, Int& out )
{
	const char *p = findValue(json, key);
	if (p == nullptr)
		return FALSE;

	char *end = nullptr;
	long value = strtol(p, &end, 10);
	if (end == p)
		return FALSE;

	out = (Int)value;
	return TRUE;
}

static Bool readReal( const char *json, const char *key, Real& out )
{
	const char *p = findValue(json, key);
	if (p == nullptr)
		return FALSE;

	char *end = nullptr;
	double value = strtod(p, &end);
	if (end == p)
		return FALSE;

	out = (Real)value;
	return TRUE;
}

/**
	Read an array of integers, as in "ids":[1,2,3]. Returns FALSE if the key is
	absent or is not an array.
*/
static Bool readIntArray( const char *json, const char *key, std::vector<Int>& out )
{
	const char *p = findValue(json, key);
	if (p == nullptr || *p != '[')
		return FALSE;

	++p;
	while (*p != '\0' && *p != ']')
	{
		while (*p == ' ' || *p == ',' || *p == '\t')
			++p;
		if (*p == ']' || *p == '\0')
			break;

		char *end = nullptr;
		long value = strtol(p, &end, 10);
		if (end == p)
			return FALSE;

		out.push_back((Int)value);
		p = end;
	}

	return *p == ']';
}

//-------------------------------------------------------------------------------------------------

ActionServer::ActionServer()
{
	m_enabled = FALSE;
	m_playerIndex = -1;
	m_listenSocket = (UnsignedInt)INVALID_SOCKET;
	m_clientSocket = (UnsignedInt)INVALID_SOCKET;
	m_pending = nullptr;
}

ActionServer::~ActionServer()
{
	shutdown();
}

//-------------------------------------------------------------------------------------------------
void ActionServer::init( UnsignedShort port, Int playerIndex )
{
	if (m_enabled)
		return;

	WORD verReq = MAKEWORD(2, 2);
	WSADATA wsadata;
	if (WSAStartup(verReq, &wsadata) != 0)
	{
		DEBUG_LOG(("ActionServer: WSAStartup failed"));
		return;
	}

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		DEBUG_LOG(("ActionServer: socket() failed"));
		WSACleanup();
		return;
	}

	// Allow an immediate rebind, so consecutive headless runs on the same
	// port do not fail while the previous socket lingers in TIME_WAIT.
	BOOL reuse = TRUE;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	// loopback only, never exposed

	if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
			listen(listenSock, 1) != 0)
	{
		DEBUG_LOG(("ActionServer: could not listen on port %d", port));
		closesocket(listenSock);
		WSACleanup();
		return;
	}

	// Never block the logic thread waiting for an agent.
	u_long nonBlocking = 1;
	ioctlsocket(listenSock, FIONBIO, &nonBlocking);

	m_listenSocket = (UnsignedInt)listenSock;
	m_playerIndex = playerIndex;
	m_pending = NEW AsciiString;
	m_enabled = TRUE;

	DEBUG_LOG(("ActionServer: listening on 127.0.0.1:%d, acting as player %d",
		port, m_playerIndex));
}

//-------------------------------------------------------------------------------------------------
void ActionServer::shutdown()
{
	if (m_clientSocket != (UnsignedInt)INVALID_SOCKET)
	{
		closesocket((SOCKET)m_clientSocket);
		m_clientSocket = (UnsignedInt)INVALID_SOCKET;
	}
	if (m_listenSocket != (UnsignedInt)INVALID_SOCKET)
	{
		closesocket((SOCKET)m_listenSocket);
		m_listenSocket = (UnsignedInt)INVALID_SOCKET;
	}
	if (m_enabled)
		WSACleanup();

	if (m_pending != nullptr)
	{
		delete m_pending;
		m_pending = nullptr;
	}

	m_enabled = FALSE;
}

//-------------------------------------------------------------------------------------------------
void ActionServer::acceptClient()
{
	SOCKET sock = accept((SOCKET)m_listenSocket, nullptr, nullptr);
	if (sock == INVALID_SOCKET)
		return;	// nothing pending; the listen socket is non blocking

	u_long nonBlocking = 1;
	ioctlsocket(sock, FIONBIO, &nonBlocking);

	// Orders are small and latency sensitive, so coalescing hurts here.
	BOOL noDelay = TRUE;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&noDelay, sizeof(noDelay));

	m_clientSocket = (UnsignedInt)sock;
	if (m_pending != nullptr)
		m_pending->clear();
	DEBUG_LOG(("ActionServer: agent connected"));
}

//-------------------------------------------------------------------------------------------------
void ActionServer::reply( const char *status, const char *detail )
{
	if (m_clientSocket == (UnsignedInt)INVALID_SOCKET)
		return;

	// Both strings are bounded: detail can echo the verb an agent sent, and an
	// unbounded sprintf into a stack buffer is how the observation server's map
	// header once overwrote its own return address.
	char line[512];
	snprintf(line, sizeof(line),
		"{\"status\":\"%.64s\",\"detail\":\"%.256s\",\"frame\":%d}\n",
		status, detail, TheGameLogic ? (Int)TheGameLogic->getFrame() : -1);

	// Best effort only. An agent that does not read its replies must never be
	// able to stall the simulation, so a full send buffer simply drops the ack.
	send((SOCKET)m_clientSocket, line, (int)strlen(line), 0);
}

//-------------------------------------------------------------------------------------------------
void ActionServer::replyRaw( const std::string& body )
{
	if (m_clientSocket == (UnsignedInt)INVALID_SOCKET)
		return;

	std::string line;
	line.reserve(body.size() + 2);
	line.append(body);
	line.append("\n");

	// Same best-effort contract as reply(): a query answer is never worth
	// stalling the simulation for, so a full send buffer simply drops it.
	send((SOCKET)m_clientSocket, line.c_str(), (int)line.size(), 0);
}

//-------------------------------------------------------------------------------------------------
void ActionServer::receive()
{
	char chunk[4096];

	for (;;)
	{
		int got = recv((SOCKET)m_clientSocket, chunk, sizeof(chunk) - 1, 0);
		if (got > 0)
		{
			chunk[got] = '\0';
			m_pending->concat(chunk);
			if (m_pending->getLength() > MAX_PENDING)
			{
				DEBUG_LOG(("ActionServer: order line too long, dropping agent"));
				closesocket((SOCKET)m_clientSocket);
				m_clientSocket = (UnsignedInt)INVALID_SOCKET;
				return;
			}
			continue;
		}

		if (got == 0)
		{
			// Orderly shutdown by the agent.
			DEBUG_LOG(("ActionServer: agent disconnected"));
			closesocket((SOCKET)m_clientSocket);
			m_clientSocket = (UnsignedInt)INVALID_SOCKET;
			return;
		}

		if (WSAGetLastError() == WSAEWOULDBLOCK)
			return;	// nothing more this frame, which is the normal case

		DEBUG_LOG(("ActionServer: recv failed, dropping agent"));
		closesocket((SOCKET)m_clientSocket);
		m_clientSocket = (UnsignedInt)INVALID_SOCKET;
		return;
	}
}

//-------------------------------------------------------------------------------------------------
GameMessage *ActionServer::beginMessage( GameMessage::Type type )
{
	// Only commands the network layer itself is willing to carry may be
	// injected. Anything outside this range is either a client side UI event or
	// a logic-to-client notification, and forging one would desync the game.
	if (type <= GameMessage::MSG_BEGIN_NETWORK_MESSAGES ||
			type >= GameMessage::MSG_END_NETWORK_MESSAGES)
		return nullptr;

	GameMessage *msg = newInstance(GameMessage)(type);

	// The constructor attributes the message to the local player; an agent
	// acts as its own player, which may not be the local one.
	msg->friend_setPlayerIndex(m_playerIndex);

	return msg;
}

//-------------------------------------------------------------------------------------------------
/**
	Commands apply to the issuing player's current selection, so an order that
	names objects has to establish that selection first. This mirrors what the
	UI does when the user drags a box and then right clicks.

	Ownership is deliberately not checked here: the dispatcher already calls
	removeAnyObjectsNotOwnedByPlayer(), so a foreign object simply drops out.
*/
Bool ActionServer::selectObjects( const char *json )
{
	std::vector<Int> ids;
	if (!readIntArray(json, "ids", ids) || ids.empty())
		return FALSE;

	GameMessage *msg = beginMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
	if (msg == nullptr)
		return FALSE;

	msg->appendBooleanArgument(TRUE);	// create a new group rather than augmenting
	for (size_t i = 0; i < ids.size(); ++i)
		msg->appendObjectIDArgument((ObjectID)ids[i]);

	TheCommandList->appendMessage(msg);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void ActionServer::executeLine( const char *line )
{
	AsciiString action;
	if (!readString(line, "action", action))
	{
		reply("error", "missing action");
		return;
	}

	const char *verb = action.str();

	// ---- selection -------------------------------------------------------

	if (strcmp(verb, "select") == 0)
	{
		if (!selectObjects(line))
			reply("error", "select needs a non-empty ids array");
		else
			reply("ok", "select");
		return;
	}

	// ---- sandbox: staging a controlled fight ------------------------------

	if (strcmp(verb, "spawn") == 0)
	{
		// Gated behind -sandbox, and deliberately so. Conjuring units is not
		// playing the game, and doing it during a real match would desync
		// every other client the moment their simulation disagreed about what
		// exists. This is here so a harness can stage a matchup and let the
		// engine itself settle what a fight actually costs -- which no amount
		// of arithmetic over template stats can tell you, because crushing,
		// clip reloads and multi-weapon units are simulation behaviour.
		if (!TheGlobalData->m_combatSandbox)
		{
			reply("error", "spawn requires the engine to be started with -sandbox");
			return;
		}

		AsciiString what;
		Real x = 0.0f, y = 0.0f;
		Int owner = 0, count = 1;
		if (!readString(line, "unit", what) ||
				!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "spawn needs unit, x and y");
			return;
		}
		readInt(line, "player", owner);
		readInt(line, "count", count);
		if (count < 1)
			count = 1;
		if (count > 200)
			count = 200;

		const ThingTemplate *tmpl = TheThingFactory->findTemplate(what);
		if (tmpl == nullptr)
		{
			reply("error", "spawn: no such template");
			return;
		}

		Player *forWhom = (ThePlayerList != nullptr)
			? ThePlayerList->getNthPlayer(owner) : nullptr;
		if (forWhom == nullptr)
		{
			reply("error", "spawn: no such player");
			return;
		}
		Team *team = forWhom->getDefaultTeam();
		if (team == nullptr)
		{
			reply("error", "spawn: player has no team");
			return;
		}

		// Lay them out in a block so they do not all land on one another; the
		// engine will sort out the overlap but a tidy start makes a measured
		// fight repeatable.
		const Real spacing = 15.0f;
		const Int perRow = 5;
		Int made = 0;
		for (Int i = 0; i < count; ++i)
		{
			Object *obj = TheThingFactory->newObject(tmpl, team);
			if (obj == nullptr)
				continue;
			Coord3D where;
			where.x = x + (Real)(i % perRow) * spacing;
			where.y = y + (Real)(i / perRow) * spacing;
			where.z = 0.0f;
			obj->setPosition(&where);
			++made;
		}

		AsciiString note;
		note.format("spawn %d", made);
		reply("ok", note.str());
		return;
	}

	// ---- orders taking a location ----------------------------------------

	if (strcmp(verb, "move") == 0 ||
			strcmp(verb, "attack_move") == 0 ||
			strcmp(verb, "force_move") == 0 ||
			strcmp(verb, "attack_ground") == 0 ||
			strcmp(verb, "add_waypoint") == 0 ||
			strcmp(verb, "salvage") == 0 ||
			strcmp(verb, "combat_drop_at") == 0 ||
			strcmp(verb, "guard_position") == 0)
	{
		Real x = 0.0f, y = 0.0f;
		if (!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "move needs x and y");
			return;
		}

		if (!selectObjects(line))
		{
			reply("error", "move needs a non-empty ids array");
			return;
		}

		GameMessage::Type type = GameMessage::MSG_DO_MOVETO;
		if (strcmp(verb, "attack_move") == 0)
			type = GameMessage::MSG_DO_ATTACKMOVETO;
		else if (strcmp(verb, "force_move") == 0)
			type = GameMessage::MSG_DO_FORCEMOVETO;
		else if (strcmp(verb, "attack_ground") == 0)
			type = GameMessage::MSG_DO_FORCE_ATTACK_GROUND;
		else if (strcmp(verb, "add_waypoint") == 0)
			type = GameMessage::MSG_ADD_WAYPOINT;
		else if (strcmp(verb, "salvage") == 0)
			type = GameMessage::MSG_DO_SALVAGE;
		else if (strcmp(verb, "combat_drop_at") == 0)
			type = GameMessage::MSG_COMBATDROP_AT_LOCATION;
		else if (strcmp(verb, "guard_position") == 0)
			type = GameMessage::MSG_DO_GUARD_POSITION;

		GameMessage *msg = beginMessage(type);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		// The z of a ground order is resolved by the pathfinder, so the height
		// an agent supplies (if any) is advisory only.
		Coord3D dest;
		dest.x = x;
		dest.y = y;
		dest.z = 0.0f;
		readReal(line, "z", dest.z);
		msg->appendLocationArgument(dest);

		if (type == GameMessage::MSG_DO_GUARD_POSITION)
		{
			Int mode = 0;	// GUARDMODE_NORMAL
			readInt(line, "mode", mode);
			msg->appendIntegerArgument(mode);
		}

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- orders taking a target object -----------------------------------

	// "capture" is deliberately absent: MSG_CAPTUREBUILDING exists only as a
	// cursor hint and is never dispatched by the logic. Capturing a building is
	// done by sending infantry into it, which is "enter".
	if (strcmp(verb, "attack") == 0 ||
			strcmp(verb, "force_attack") == 0 ||
			strcmp(verb, "enter") == 0 ||
			strcmp(verb, "repair") == 0 ||
			strcmp(verb, "get_repaired") == 0 ||
			strcmp(verb, "get_healed") == 0 ||
			strcmp(verb, "dock") == 0 ||
			strcmp(verb, "combat_drop") == 0 ||
			strcmp(verb, "guard_object") == 0)
	{
		Int target = 0;
		if (!readInt(line, "target", target))
		{
			reply("error", "order needs a target id");
			return;
		}

		if (!selectObjects(line))
		{
			reply("error", "order needs a non-empty ids array");
			return;
		}

		GameMessage::Type type = GameMessage::MSG_DO_ATTACK_OBJECT;
		if (strcmp(verb, "force_attack") == 0)
			type = GameMessage::MSG_DO_FORCE_ATTACK_OBJECT;
		else if (strcmp(verb, "enter") == 0)
			type = GameMessage::MSG_ENTER;
		else if (strcmp(verb, "repair") == 0)
			type = GameMessage::MSG_DO_REPAIR;
		else if (strcmp(verb, "get_repaired") == 0)
			type = GameMessage::MSG_GET_REPAIRED;
		else if (strcmp(verb, "get_healed") == 0)
			type = GameMessage::MSG_GET_HEALED;
		else if (strcmp(verb, "dock") == 0)
			type = GameMessage::MSG_DOCK;
		else if (strcmp(verb, "combat_drop") == 0)
			type = GameMessage::MSG_COMBATDROP_AT_OBJECT;
		else if (strcmp(verb, "guard_object") == 0)
			type = GameMessage::MSG_DO_GUARD_OBJECT;

		GameMessage *msg = beginMessage(type);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		// MSG_ENTER carries a leading INVALID_ID ("the current selection") and
		// the logic reads the target from argument 1; see
		// CommandTranslator::createEnterMessage and GameLogic::onEnter. Every
		// other order here takes the target as argument 0.
		if (type == GameMessage::MSG_ENTER)
			msg->appendObjectIDArgument(INVALID_ID);
		msg->appendObjectIDArgument((ObjectID)target);

		if (type == GameMessage::MSG_DO_GUARD_OBJECT)
		{
			Int mode = 0;	// GUARDMODE_NORMAL
			readInt(line, "mode", mode);
			msg->appendIntegerArgument(mode);
		}

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- orders acting on the selection alone ----------------------------

	if (strcmp(verb, "stop") == 0 ||
			strcmp(verb, "scatter") == 0 ||
			strcmp(verb, "sell") == 0 ||
			strcmp(verb, "evacuate") == 0 ||
			strcmp(verb, "cheer") == 0 ||
			strcmp(verb, "hack_internet") == 0 ||
			strcmp(verb, "toggle_overcharge") == 0 ||
			strcmp(verb, "cancel_construct") == 0 ||
			strcmp(verb, "railed_transport") == 0 ||
			strcmp(verb, "resume_construction") == 0)
	{
		if (!selectObjects(line))
		{
			reply("error", "order needs a non-empty ids array");
			return;
		}

		GameMessage::Type type = GameMessage::MSG_DO_STOP;
		if (strcmp(verb, "scatter") == 0)
			type = GameMessage::MSG_DO_SCATTER;
		else if (strcmp(verb, "sell") == 0)
			type = GameMessage::MSG_SELL;
		else if (strcmp(verb, "evacuate") == 0)
			type = GameMessage::MSG_EVACUATE;
		else if (strcmp(verb, "cheer") == 0)
			type = GameMessage::MSG_DO_CHEER;
		else if (strcmp(verb, "hack_internet") == 0)
			type = GameMessage::MSG_INTERNET_HACK;
		else if (strcmp(verb, "toggle_overcharge") == 0)
			type = GameMessage::MSG_TOGGLE_OVERCHARGE;
		else if (strcmp(verb, "cancel_construct") == 0)
			type = GameMessage::MSG_DOZER_CANCEL_CONSTRUCT;
		else if (strcmp(verb, "railed_transport") == 0)
			type = GameMessage::MSG_EXECUTE_RAILED_TRANSPORT;
		else if (strcmp(verb, "resume_construction") == 0)
			type = GameMessage::MSG_RESUME_CONSTRUCTION;

		GameMessage *msg = beginMessage(type);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- production ------------------------------------------------------

	if (strcmp(verb, "build_unit") == 0 || strcmp(verb, "cancel_unit") == 0)
	{
		AsciiString what;
		if (!readString(line, "unit", what))
		{
			reply("error", "build_unit needs a unit name");
			return;
		}

		const ThingTemplate *tmpl = TheThingFactory->findTemplate(what, FALSE);
		if (tmpl == nullptr)
		{
			reply("error", "unknown unit name");
			return;
		}

		// Production reads a single object out of the selection, so a producer
		// must be named explicitly rather than inferred from a group.
		Int producer = 0;
		if (!readInt(line, "producer", producer))
		{
			reply("error", "build_unit needs a producer id");
			return;
		}

		GameMessage *sel = beginMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		if (sel == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		sel->appendBooleanArgument(TRUE);
		sel->appendObjectIDArgument((ObjectID)producer);
		TheCommandList->appendMessage(sel);

		GameMessage *msg = beginMessage(strcmp(verb, "build_unit") == 0
			? GameMessage::MSG_QUEUE_UNIT_CREATE
			: GameMessage::MSG_CANCEL_UNIT_CREATE);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		msg->appendIntegerArgument(tmpl->getTemplateID());

		Int productionID = 0;	// PRODUCTIONID_INVALID; only cancel needs a real one
		readInt(line, "production_id", productionID);
		msg->appendIntegerArgument(productionID);

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- construction ----------------------------------------------------

	if (strcmp(verb, "build_structure") == 0)
	{
		AsciiString what;
		Int dozer = 0;
		Real x = 0.0f, y = 0.0f;

		if (!readString(line, "structure", what) ||
				!readInt(line, "dozer", dozer) ||
				!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "build_structure needs structure, dozer, x and y");
			return;
		}

		const ThingTemplate *tmpl = TheThingFactory->findTemplate(what, FALSE);
		if (tmpl == nullptr)
		{
			reply("error", "unknown structure name");
			return;
		}

		GameMessage *sel = beginMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		if (sel == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		sel->appendBooleanArgument(TRUE);
		sel->appendObjectIDArgument((ObjectID)dozer);
		TheCommandList->appendMessage(sel);

		GameMessage *msg = beginMessage(GameMessage::MSG_DOZER_CONSTRUCT);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		Coord3D loc;
		loc.x = x;
		loc.y = y;
		loc.z = 0.0f;
		readReal(line, "z", loc.z);

		Real angle = 0.0f;
		readReal(line, "angle", angle);

		msg->appendIntegerArgument(tmpl->getTemplateID());
		msg->appendLocationArgument(loc);
		msg->appendRealArgument(angle);

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- rally point -----------------------------------------------------

	if (strcmp(verb, "set_rally_point") == 0)
	{
		Int building = 0;
		Real x = 0.0f, y = 0.0f;
		if (!readInt(line, "building", building) ||
				!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "set_rally_point needs building, x and y");
			return;
		}

		GameMessage *msg = beginMessage(GameMessage::MSG_SET_RALLY_POINT);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		Coord3D loc;
		loc.x = x;
		loc.y = y;
		loc.z = 0.0f;
		readReal(line, "z", loc.z);

		msg->appendObjectIDArgument((ObjectID)building);
		msg->appendLocationArgument(loc);

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- special powers --------------------------------------------------
	//
	// This is the single most important family for Zero Hour: generals'
	// powers, superweapons and most unit abilities (sniper shot, hijack,
	// hack, car bomb, ranger clearing a building) are all special powers,
	// not distinct message types. They are addressed by the power's INI name.
	//
	// The source object is passed explicitly rather than relying on the
	// selection, which is how the shortcut buttons on the command bar work:
	// the power fires from the named object no matter what is selected.

	if (strcmp(verb, "special_power") == 0 ||
			strcmp(verb, "special_power_at") == 0 ||
			strcmp(verb, "special_power_at_object") == 0)
	{
		AsciiString powerName;
		if (!readString(line, "power", powerName))
		{
			reply("error", "special power needs a power name");
			return;
		}

		const SpecialPowerTemplate *sp =
			TheSpecialPowerStore->findSpecialPowerTemplate(powerName);
		if (sp == nullptr)
		{
			reply("error", "unknown special power name");
			return;
		}

		// Command button options ride along; a power that cares about variants
		// reads them, the rest ignore them.
		Int options = 0;
		readInt(line, "options", options);

		// The firing object. Optional, but without it the power falls back to
		// the current selection.
		Int source = 0;
		readInt(line, "source", source);

		if (strcmp(verb, "special_power") == 0)
		{
			GameMessage *msg = beginMessage(GameMessage::MSG_DO_SPECIAL_POWER);
			if (msg == nullptr)
			{
				reply("error", "unsupported order");
				return;
			}
			msg->appendIntegerArgument(sp->getID());
			msg->appendIntegerArgument(options);
			msg->appendObjectIDArgument((ObjectID)source);
			TheCommandList->appendMessage(msg);
			reply("ok", verb);
			return;
		}

		if (strcmp(verb, "special_power_at_object") == 0)
		{
			Int target = 0;
			if (!readInt(line, "target", target))
			{
				reply("error", "special_power_at_object needs a target id");
				return;
			}

			GameMessage *msg = beginMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
			if (msg == nullptr)
			{
				reply("error", "unsupported order");
				return;
			}
			msg->appendIntegerArgument(sp->getID());
			msg->appendObjectIDArgument((ObjectID)target);
			msg->appendIntegerArgument(options);
			msg->appendObjectIDArgument((ObjectID)source);
			TheCommandList->appendMessage(msg);
			reply("ok", verb);
			return;
		}

		// special_power_at <location>
		Real x = 0.0f, y = 0.0f;
		if (!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "special_power_at needs x and y");
			return;
		}

		GameMessage *msg = beginMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		Coord3D loc;
		loc.x = x;
		loc.y = y;
		loc.z = 0.0f;
		readReal(line, "z", loc.z);

		// The handler reads an angle only when six arguments are present, so
		// it is all or nothing; always send it and let the power ignore it.
		Real angle = 0.0f;
		readReal(line, "angle", angle);

		Int inWay = 0;
		readInt(line, "object_in_way", inWay);

		msg->appendIntegerArgument(sp->getID());
		msg->appendLocationArgument(loc);
		msg->appendRealArgument(angle);
		msg->appendObjectIDArgument((ObjectID)inWay);
		msg->appendIntegerArgument(options);
		msg->appendObjectIDArgument((ObjectID)source);

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- upgrades --------------------------------------------------------

	if (strcmp(verb, "upgrade") == 0 || strcmp(verb, "cancel_upgrade") == 0)
	{
		AsciiString upName;
		Int building = 0;
		if (!readString(line, "upgrade", upName) ||
				!readInt(line, "building", building))
		{
			reply("error", "upgrade needs upgrade name and building id");
			return;
		}

		const UpgradeTemplate *up = TheUpgradeCenter->findUpgrade(upName);
		if (up == nullptr)
		{
			reply("error", "unknown upgrade name");
			return;
		}

		// The handler applies the upgrade to the selection, while the object id
		// identifies the producer, so both are needed.
		GameMessage *sel = beginMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		if (sel == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		sel->appendBooleanArgument(TRUE);
		sel->appendObjectIDArgument((ObjectID)building);
		TheCommandList->appendMessage(sel);

		GameMessage *msg = beginMessage(strcmp(verb, "upgrade") == 0
			? GameMessage::MSG_QUEUE_UPGRADE
			: GameMessage::MSG_CANCEL_UPGRADE);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		msg->appendObjectIDArgument((ObjectID)building);
		msg->appendIntegerArgument((Int)up->getUpgradeNameKey());

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- generals' promotions --------------------------------------------

	if (strcmp(verb, "purchase_science") == 0)
	{
		AsciiString sciName;
		if (!readString(line, "science", sciName))
		{
			reply("error", "purchase_science needs a science name");
			return;
		}

		// getScienceFromInternalName only interns the string into a name key and
		// will happily return a "type" for nonsense, so the result has to be
		// checked against the sciences that actually exist.
		ScienceType st = TheScienceStore->getScienceFromInternalName(sciName);
		if (st == SCIENCE_INVALID || !TheScienceStore->isValidScience(st))
		{
			reply("error", "unknown science name");
			return;
		}

		GameMessage *msg = beginMessage(GameMessage::MSG_PURCHASE_SCIENCE);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		msg->appendIntegerArgument((Int)st);
		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- weapons ---------------------------------------------------------

	if (strcmp(verb, "fire_weapon") == 0 ||
			strcmp(verb, "fire_weapon_at") == 0 ||
			strcmp(verb, "fire_weapon_at_object") == 0 ||
			strcmp(verb, "switch_weapon") == 0)
	{
		Int slot = 0;	// PRIMARY_WEAPON
		readInt(line, "slot", slot);

		if (!selectObjects(line))
		{
			reply("error", "weapon order needs a non-empty ids array");
			return;
		}

		if (strcmp(verb, "switch_weapon") == 0)
		{
			GameMessage *msg = beginMessage(GameMessage::MSG_SWITCH_WEAPONS);
			if (msg == nullptr)
			{
				reply("error", "unsupported order");
				return;
			}
			msg->appendIntegerArgument(slot);
			TheCommandList->appendMessage(msg);
			reply("ok", verb);
			return;
		}

		// Default to firing without a shot limit, which is what the command bar
		// sends when a button does not specify one.
		Int maxShots = NO_MAX_SHOTS_LIMIT;
		readInt(line, "max_shots", maxShots);

		if (strcmp(verb, "fire_weapon") == 0)
		{
			GameMessage *msg = beginMessage(GameMessage::MSG_DO_WEAPON);
			if (msg == nullptr)
			{
				reply("error", "unsupported order");
				return;
			}
			msg->appendIntegerArgument(slot);
			msg->appendIntegerArgument(maxShots);
			TheCommandList->appendMessage(msg);
			reply("ok", verb);
			return;
		}

		if (strcmp(verb, "fire_weapon_at_object") == 0)
		{
			Int target = 0;
			if (!readInt(line, "target", target))
			{
				reply("error", "fire_weapon_at_object needs a target id");
				return;
			}
			GameMessage *msg = beginMessage(GameMessage::MSG_DO_WEAPON_AT_OBJECT);
			if (msg == nullptr)
			{
				reply("error", "unsupported order");
				return;
			}
			msg->appendIntegerArgument(slot);
			msg->appendObjectIDArgument((ObjectID)target);
			msg->appendIntegerArgument(maxShots);
			TheCommandList->appendMessage(msg);
			reply("ok", verb);
			return;
		}

		Real x = 0.0f, y = 0.0f;
		if (!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "fire_weapon_at needs x and y");
			return;
		}

		GameMessage *msg = beginMessage(GameMessage::MSG_DO_WEAPON_AT_LOCATION);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		Coord3D loc;
		loc.x = x;
		loc.y = y;
		loc.z = 0.0f;
		readReal(line, "z", loc.z);

		msg->appendIntegerArgument(slot);
		msg->appendLocationArgument(loc);
		msg->appendIntegerArgument(maxShots);

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- unload a specific passenger --------------------------------------

	if (strcmp(verb, "exit_unit") == 0)
	{
		Int transport = 0, passenger = 0;
		if (!readInt(line, "transport", transport) ||
				!readInt(line, "passenger", passenger))
		{
			reply("error", "exit_unit needs transport and passenger ids");
			return;
		}

		GameMessage *sel = beginMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		if (sel == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		sel->appendBooleanArgument(TRUE);
		sel->appendObjectIDArgument((ObjectID)transport);
		TheCommandList->appendMessage(sel);

		GameMessage *msg = beginMessage(GameMessage::MSG_EXIT);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		msg->appendObjectIDArgument((ObjectID)passenger);
		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- stance and behaviour toggles -------------------------------------

	if (strcmp(verb, "set_retaliation") == 0)
	{
		Int on = 1;
		readInt(line, "enabled", on);

		GameMessage *msg = beginMessage(GameMessage::MSG_ENABLE_RETALIATION_MODE);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		msg->appendIntegerArgument(on);
		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	if (strcmp(verb, "set_mine_clearing") == 0)
	{
		Int on = 1;
		readInt(line, "enabled", on);

		if (!selectObjects(line))
		{
			reply("error", "set_mine_clearing needs a non-empty ids array");
			return;
		}

		GameMessage *msg = beginMessage(GameMessage::MSG_SET_MINE_CLEARING_DETAIL);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		msg->appendIntegerArgument(on);
		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- build a line of structures (walls) --------------------------------

	if (strcmp(verb, "build_wall") == 0)
	{
		AsciiString what;
		Int dozer = 0;
		Real x = 0.0f, y = 0.0f, x2 = 0.0f, y2 = 0.0f;

		if (!readString(line, "structure", what) ||
				!readInt(line, "dozer", dozer) ||
				!readReal(line, "x", x) || !readReal(line, "y", y) ||
				!readReal(line, "x2", x2) || !readReal(line, "y2", y2))
		{
			reply("error", "build_wall needs structure, dozer, x, y, x2 and y2");
			return;
		}

		const ThingTemplate *tmpl = TheThingFactory->findTemplate(what, FALSE);
		if (tmpl == nullptr)
		{
			reply("error", "unknown structure name");
			return;
		}

		GameMessage *sel = beginMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		if (sel == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		sel->appendBooleanArgument(TRUE);
		sel->appendObjectIDArgument((ObjectID)dozer);
		TheCommandList->appendMessage(sel);

		GameMessage *msg = beginMessage(GameMessage::MSG_DOZER_CONSTRUCT_LINE);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}

		Coord3D a, b;
		a.x = x;  a.y = y;  a.z = 0.0f;
		b.x = x2; b.y = y2; b.z = 0.0f;

		Real angle = 0.0f;
		readReal(line, "angle", angle);

		msg->appendIntegerArgument(tmpl->getTemplateID());
		msg->appendLocationArgument(a);
		msg->appendRealArgument(angle);
		msg->appendLocationArgument(b);

		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	// ---- placement query --------------------------------------------------
	//
	// The UI shows a red or green footprint as the cursor moves, so an agent
	// asking whether a structure fits is reading the same thing a player sees,
	// not privileged state. It is a query: nothing is built and no message is
	// queued, so the simulation is untouched.

	if (strcmp(verb, "can_place") == 0)
	{
		AsciiString what;
		Real x = 0.0f, y = 0.0f;
		if (!readString(line, "structure", what) ||
				!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "can_place needs structure, x and y");
			return;
		}

		const ThingTemplate *tmpl = TheThingFactory->findTemplate(what, FALSE);
		if (tmpl == nullptr)
		{
			reply("error", "unknown structure name");
			return;
		}

		Player *player = ThePlayerList->getNthPlayer(m_playerIndex);
		if (player == nullptr || TheBuildAssistant == nullptr)
		{
			reply("error", "no such player");
			return;
		}

		Object *builder = nullptr;
		Int dozer = 0;
		if (readInt(line, "dozer", dozer))
			builder = TheGameLogic->findObjectByID((ObjectID)dozer);

		Coord3D loc;
		loc.x = x;
		loc.y = y;
		loc.z = 0.0f;
		readReal(line, "z", loc.z);

		Real angle = 0.0f;
		readReal(line, "angle", angle);

		// The same checks the dozer itself applies in DozerAIUpdate::construct,
		// shroud included. Shroud is revealed by the logic, not the renderer,
		// so a headless run clears it around every unit exactly as a rendered
		// one does; an earlier version skipped the check headless and told the
		// agent "ok" for sites the real build order then silently refused.
		UnsignedInt options = BuildAssistant::TERRAIN_RESTRICTIONS |
													BuildAssistant::CLEAR_PATH |
													BuildAssistant::NO_OBJECT_OVERLAP |
													BuildAssistant::SHROUD_REVEALED;

		// The command set gate is separate from terrain legality and is the more
		// common reason a build is refused, so report it distinctly rather than
		// letting the caller guess.
		if (builder != nullptr &&
				!TheBuildAssistant->isPossibleToMakeUnit(builder, tmpl))
		{
			reply("no", "builder cannot make this");
			return;
		}

		// The dozer drops a construct order outright when the bank is short
		// (DozerAIUpdate::construct -> BuildAssistant::canMakeUnit), and did so
		// silently behind an "ok" from here: the agent logged "starting" for a
		// building the engine had already discarded. Say so instead.
		if (builder != nullptr &&
				TheBuildAssistant->canMakeUnit(builder, tmpl) == CANMAKE_NO_MONEY)
		{
			reply("no", "no money");
			return;
		}

		LegalBuildCode code = TheBuildAssistant->isLocationLegalToBuild(
			&loc, tmpl, angle, options, builder, player);

		static const char *codeNames[] = {
			"ok", "restricted_terrain", "not_flat_enough", "objects_in_the_way",
			"no_clear_path", "shroud", "too_close_to_supplies", "generic_failure"
		};
		const Int idx = (Int)code;
		reply(code == LBC_OK ? "ok" : "no",
			(idx >= 0 && idx < (Int)(sizeof(codeNames) / sizeof(codeNames[0])))
				? codeNames[idx] : "unknown");
		return;
	}

	// ---- command set dump -------------------------------------------------
	//
	// The engine already knows exactly what a builder may produce and why an
	// entry is unavailable: isPossibleToMakeUnit() consults the builder's
	// command set and then Player::canBuild() for prerequisites. Both answers
	// were previously collapsed into a single "builder cannot make this", which
	// left an agent guessing at the tech tree. This reports them separately,
	// with the same "Requires: ..." text the UI puts on a greyed-out cameo.
	//
	// It is a pure query and reads only what the player's own UI would show.

	if (strcmp(verb, "command_set") == 0)
	{
		Int dozer = 0;
		if (!readInt(line, "object", dozer) && !readInt(line, "dozer", dozer))
		{
			reply("error", "command_set needs object (or dozer)");
			return;
		}

		Object *builder = TheGameLogic->findObjectByID((ObjectID)dozer);
		if (builder == nullptr)
		{
			reply("error", "no such object");
			return;
		}

		if (TheControlBar == nullptr)
		{
			reply("error", "no control bar");
			return;
		}

		const CommandSet *commandSet =
			TheControlBar->findCommandSet(builder->getCommandSetString());
		if (commandSet == nullptr)
		{
			reply("no", "object has no command set");
			return;
		}

		Player *player = builder->getControllingPlayer();

		std::string body;
		body.reserve(4096);
		body.append("{\"status\":\"ok\",\"detail\":\"command_set\",\"object\":");
		appendInt(body, dozer);
		body.append(",\"command_set\":\"");
		appendEscaped(body, builder->getCommandSetString().str());
		body.append("\",\"entries\":[");

		Bool first = TRUE;
		Int i;
		for (i = 0; i < MAX_COMMANDS_PER_SET; i++)
		{
			const CommandButton *button = commandSet->getCommandButton(i);
			if (button == nullptr)
				continue;

			if (!first)
				body.append(",");
			first = FALSE;

			body.append("{\"slot\":");
			appendInt(body, i);
			body.append(",\"button\":\"");
			appendEscaped(body, button->getName().str());
			body.append("\",\"command\":");
			appendInt(body, (Int)button->getCommandType());

			// The special power this button actually invokes. Without it a
			// client has to guess a power by name from the global store, and
			// picking (say) the Ranger's capture ability for a Red Guard
			// yields an order the engine accepts and silently ignores.
			const SpecialPowerTemplate *power = button->getSpecialPowerTemplate();
			if (power != nullptr)
			{
				body.append(",\"power\":\"");
				appendEscaped(body, power->getName().str());
				body.append("\"");
			}

			const ThingTemplate *tmpl = button->getThingTemplate();
			const Bool isBuild =
				(button->getCommandType() == GUI_COMMAND_UNIT_BUILD ||
				 button->getCommandType() == GUI_COMMAND_DOZER_CONSTRUCT);

			if (tmpl != nullptr)
			{
				body.append(",\"thing\":\"");
				appendEscaped(body, tmpl->getName().str());
				body.append("\"");
			}

			// An upgrade button names its upgrade, whether it is already owned,
			// and what it costs -- the cameo, its checkmark and its tooltip.
			const UpgradeTemplate *up = button->getUpgradeTemplate();
			if (up != nullptr && player != nullptr)
			{
				body.append(",\"upgrade\":\"");
				appendEscaped(body, up->getUpgradeName().str());
				body.append("\",\"owned\":");
				body.append(player->hasUpgradeComplete(up) ? "true" : "false");
				body.append(",\"cost\":");
				appendInt(body, (Int)up->calcCostToBuild(player));
			}

			// A science button lists the sciences it grants.
			const ScienceVec &sciences = button->getScienceVec();
			if (!sciences.empty() && TheScienceStore != nullptr)
			{
				body.append(",\"sciences\":[");
				for (size_t si = 0; si < sciences.size(); ++si)
				{
					if (si > 0)
						body.append(",");
					body.append("\"");
					appendEscaped(body, TheScienceStore->getInternalNameForScience(sciences[si]).str());
					body.append("\"");
				}
				body.append("]");
			}

			if (isBuild && tmpl != nullptr && player != nullptr)
			{
				const Bool can = player->canBuild(tmpl);
				body.append(",\"buildable\":");
				body.append(can ? "true" : "false");

				body.append(",\"cost\":");
				appendInt(body, tmpl->calcCostToBuild(player));

				// Only unmet prerequisites are listed, which is exactly what the
				// greyed-out cameo's tooltip shows.
				if (!can)
				{
					AsciiString requires;
					Int p;
					for (p = 0; p < tmpl->getPrereqCount(); p++)
					{
						const ProductionPrerequisite *pre = tmpl->getNthPrereq(p);
						if (pre->isSatisfied(player))
							continue;

						AsciiString one;
						one.translate(pre->getRequiresList(player));
						if (one.isEmpty())
							continue;
						if (!requires.isEmpty())
							requires.concat(", ");
						requires.concat(one);
					}

					body.append(",\"requires\":\"");
					appendEscaped(body, requires.str());
					body.append("\"");
				}
			}

			body.append("}");
		}

		body.append("],\"frame\":");
		appendInt(body, TheGameLogic ? (Int)TheGameLogic->getFrame() : -1);
		body.append("}");

		replyRaw(body);
		return;
	}

	// ---- path query -------------------------------------------------------
	//
	// The engine's own pathfinder, asked the way the UI asks it when it draws
	// a move preview: is there a route for this unit, and how long is it. It
	// is a query -- nothing moves -- and it respects cliffs, bridges, water and
	// the unit's own locomotor, which is exactly what a client's flat grid
	// approximation gets wrong at the margins.

	if (strcmp(verb, "path_query") == 0)
	{
		Int objectId = 0;
		Real x = 0.0f, y = 0.0f;
		if (!readInt(line, "object", objectId) ||
				!readReal(line, "x", x) || !readReal(line, "y", y))
		{
			reply("error", "path_query needs object, x and y");
			return;
		}

		Object *obj = TheGameLogic->findObjectByID((ObjectID)objectId);
		if (obj == nullptr)
		{
			reply("error", "no such object");
			return;
		}
		const Player *player = ThePlayerList->getNthPlayer(m_playerIndex);
		if (player != nullptr && obj->getControllingPlayer() != player)
		{
			reply("error", "not your object");
			return;
		}
		if (obj->getAIUpdateInterface() == nullptr || TheAI == nullptr ||
				TheAI->pathfinder() == nullptr)
		{
			reply("no", "cannot_path");
			return;
		}

		Coord3D from = *obj->getPosition();
		Real fx = 0.0f, fy = 0.0f;
		if (readReal(line, "from_x", fx) && readReal(line, "from_y", fy))
		{
			from.x = fx;
			from.y = fy;
			from.z = TheTerrainLogic->getGroundHeight(fx, fy);
		}
		Coord3D to;
		to.x = x;
		to.y = y;
		to.z = TheTerrainLogic->getGroundHeight(x, y);

		// findPath() is private to the pathfinder (only the AI update module
		// may call it), so this uses the public ground-path query the AI group
		// logic uses to route a formation. It is a ground route two cells wide,
		// which is what tanks and infantry actually walk; an aircraft never
		// needs the question answered.
		Path *path = TheAI->pathfinder()->findGroundPath(&from, &to, 2, FALSE);
		if (path == nullptr)
		{
			reply("no", "no_path");
			return;
		}

		// Length is summed along the nodes; the node list itself is capped so
		// a cross-map route does not turn into a multi-kilobyte reply.
		Real length = 0.0f;
		Int nodes = 0;
		const Coord3D *prev = nullptr;
		std::string points;
		for (const PathNode *n = path->getFirstNode(); n != nullptr; n = n->getNext())
		{
			const Coord3D *p = n->getPosition();
			if (prev != nullptr)
			{
				const Real dx = p->x - prev->x;
				const Real dy = p->y - prev->y;
				length += sqrtf(dx * dx + dy * dy);
			}
			prev = p;
			++nodes;
			if (nodes <= 64)
			{
				char pt[48];
				snprintf(pt, sizeof(pt), "%s[%.0f,%.0f]", nodes > 1 ? "," : "", p->x, p->y);
				points.append(pt);
			}
		}
		deleteInstance(path);

		std::string body;
		body.reserve(points.size() + 128);
		body.append("{\"status\":\"ok\",\"detail\":\"path\",\"length\":");
		appendInt(body, (Int)length);
		body.append(",\"nodes\":");
		appendInt(body, nodes);
		body.append(",\"path\":[");
		body.append(points);
		body.append("]}");
		replyRaw(body);
		return;
	}

	// ---- selection management ---------------------------------------------

	if (strcmp(verb, "deselect") == 0)
	{
		GameMessage *msg = beginMessage(GameMessage::MSG_DESTROY_SELECTED_GROUP);
		if (msg == nullptr)
		{
			reply("error", "unsupported order");
			return;
		}
		TheCommandList->appendMessage(msg);
		reply("ok", verb);
		return;
	}

	reply("error", "unknown action");
}

//-------------------------------------------------------------------------------------------------
void ActionServer::update()
{
	if (!m_enabled || m_pending == nullptr)
		return;

	// Orders only mean anything inside a running match.
	if (TheGameLogic == nullptr || !TheGameLogic->isInGame() ||
			TheGameLogic->isInShellGame() || ThePlayerList == nullptr ||
			TheCommandList == nullptr)
		return;

	if (m_clientSocket == (UnsignedInt)INVALID_SOCKET)
	{
		acceptClient();
		if (m_clientSocket == (UnsignedInt)INVALID_SOCKET)
			return;
	}

	receive();
	if (m_clientSocket == (UnsignedInt)INVALID_SOCKET)
		return;

	// Execute every complete line received so far, leaving any partial line in
	// the buffer for the next frame.
	for (;;)
	{
		const char *buf = m_pending->str();
		const char *nl = strchr(buf, '\n');
		if (nl == nullptr)
			break;

		std::string line(buf, nl - buf);
		AsciiString rest = nl + 1;
		*m_pending = rest;

		if (!line.empty())
			executeLine(line.c_str());
	}
}
