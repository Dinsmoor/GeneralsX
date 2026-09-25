/*
**	Observation server: streams per-frame game state to an external agent.
**	See Common/ObservationServer.h for the rationale.
*/

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ObservationServer.h"
#include <stdlib.h>

#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"   // getSide(), so chat can name a faction
#include "Common/ThingTemplate.h"
#include "Common/Geometry.h"        // getMajorRadius, for object footprints
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "Common/Energy.h"
#include "Common/Team.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/HordeUpdate.h"
#include "GameLogic/Module/AutoDepositUpdate.h"
#include "GameLogic/Module/ActiveBody.h"
#include "GameClient/MapUtil.h"
#include "Common/ThingFactory.h"
#include "Common/Science.h"
#include "Common/Upgrade.h"
#include "Common/SpecialPower.h"
// isReady()/getReadyFrame() are declared on the interface, which
// Object.h only forward-declares.
#include "GameLogic/Module/SpecialPowerModule.h"
#include "Common/NameKeyGenerator.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/WeaponSet.h"
#include "GameLogic/Module/CollideModule.h"
#include "GameLogic/Armor.h"
#include "GameLogic/ArmorSet.h"
#include "GameLogic/Damage.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include <string>

/*	GeneralsX @build Keep winsock2 on Windows; use the shim elsewhere.

	This file was written when the engine only ever ran under Wine, so it
	included <winsock2.h> unconditionally. GeneralsX ships a Winsock -> POSIX
	BSD layer that supplies every symbol used below (SOCKET, INVALID_SOCKET,
	SOCKET_ERROR, closesocket, ioctlsocket, WSAStartup, WSACleanup,
	WSAGetLastError), so the socket code itself is unchanged.
*/
#ifdef _WIN32
#include <winsock2.h>
#else
#include "socket_compat.h"
#include <netinet/tcp.h>	// TCP_NODELAY; socket_compat.h does not pull this in
#endif

ObservationServer *TheObservationServer = nullptr;

// The terrain is sampled on its own grid rather than the pathfinder's, because
// an agent reasons about terrain strategically and a finer grid costs far more
// bandwidth than it adds information.
static const Int  MAP_SAMPLE_SIZE   = 20;
static const Real MAP_SAMPLE_SIZE_F = 20.0f;

// What an object IS, as a bitmask over the KindOf flags a player reads off any
// cameo or tooltip: structure or unit, infantry or vehicle, which faction
// building. Emitted as "k" on every object and every buildable template, so a
// client classifies by what the engine says a thing is instead of pattern
// matching template names. The bit order is published once in the map message
// as "kind_bits", so nothing about it is hardcoded on the client.
static const KindOfType KIND_BITS[] = {
	KINDOF_STRUCTURE, KINDOF_INFANTRY, KINDOF_VEHICLE, KINDOF_AIRCRAFT,
	KINDOF_DOZER, KINDOF_HARVESTER, KINDOF_COMMANDCENTER, KINDOF_FS_POWER,
	KINDOF_FS_FACTORY, KINDOF_FS_BASE_DEFENSE, KINDOF_SUPPLY_SOURCE,
	KINDOF_FS_SUPPLY_CENTER, KINDOF_FS_SUPPLY_DROPZONE, KINDOF_FS_BARRACKS,
	KINDOF_FS_WARFACTORY, KINDOF_CAN_ATTACK, KINDOF_TRANSPORT,
	KINDOF_TECH_BUILDING, KINDOF_HERO, KINDOF_PROJECTILE,
	// CAPTURABLE is what actually decides whether infantry can take a
	// building. TECH_BUILDING alone was standing in for it, which conflates
	// "neutral thing worth having" with "thing we can walk in and own".
	KINDOF_CAPTURABLE, KINDOF_TECH_BASE_DEFENSE, KINDOF_REPAIR_PAD,
	// Whether infantry can be put INSIDE it. The map's own civilian
	// bunkers sit beside every expansion dock on Alpine and exported as
	// bare STRUCTURE, so an agent had no way to tell one from a house:
	// the bot never garrisoned one in any match, and Tyler named it
	// watching a replay -- "there's no gattling turret being put up or
	// any infantry being put in that bunker that came with the map".
	// Same shape of blindness as FS_AIRFIELD not existing.
	KINDOF_GARRISONABLE_UNTIL_DESTROYED,
	// A superweapon structure: nuke silo, particle uplink, scud storm.
	// The bit has always existed; it was simply never exported, so the bot
	// identified the silo by GUESSING -- "costs >= $4,000 and draws >= 5
	// power" -- and that guess is what hid two real bugs. Because the name
	// was settled speculatively, before one was ever built, the
	// identification loop stopped running and NOTHING ever read the
	// standing building's command set. The missile was never offered, so
	// it was never fired; and the silo's two tank upgrades -- Uranium
	// Shells and Nuclear Tanks, which apply to every BattleMaster and
	// Overlord -- were never even seen. Tyler, 2026-09-17, on a match
	// where the silo stood with the missile ready the whole time.
	KINDOF_FS_SUPERWEAPON,
	// GLA salvage. A vehicle only drops a junk crate when its killer is a
	// SALVAGER (Crate.ini SalvageCrateData, KilledByType = SALVAGER), and
	// only a WEAPON_SALVAGER turns a crate into a weapon tier.
	KINDOF_SALVAGER, KINDOF_WEAPON_SALVAGER,
};
static const char *const KIND_BIT_NAMES[] = {
	"STRUCTURE", "INFANTRY", "VEHICLE", "AIRCRAFT",
	"DOZER", "HARVESTER", "COMMANDCENTER", "FS_POWER",
	"FS_FACTORY", "FS_BASE_DEFENSE", "SUPPLY_SOURCE",
	"FS_SUPPLY_CENTER", "FS_SUPPLY_DROPZONE", "FS_BARRACKS",
	"FS_WARFACTORY", "CAN_ATTACK", "TRANSPORT",
	"TECH_BUILDING", "HERO", "PROJECTILE",
	"CAPTURABLE", "TECH_BASE_DEFENSE", "REPAIR_PAD",
	"GARRISONABLE", "FS_SUPERWEAPON",
	"SALVAGER", "WEAPON_SALVAGER",
};
static const Int KIND_BIT_COUNT = sizeof(KIND_BITS) / sizeof(KIND_BITS[0]);

static UnsignedInt kindMask( const ThingTemplate *t )
{
	UnsignedInt mask = 0;
	for (Int i = 0; i < KIND_BIT_COUNT; ++i)
		if (t->isKindOf(KIND_BITS[i]))
			mask |= (1u << i);
	return mask;
}

// The primary weapon's unmodified range straight off the template: the same
// number the unit's tooltip shows.
static Real templateRange( const ThingTemplate *t )
{
	const WeaponTemplateSetVector &sets = t->getWeaponTemplateSets();
	if (sets.empty())
		return 0.0f;
	const WeaponTemplate *w = sets[0].getNth(PRIMARY_WEAPON);
	return w ? w->getUnmodifiedAttackRange() : 0.0f;
}

// ------------------------------------------------------------------------------------------------
ObservationServer::ObservationServer()
	: m_enabled(FALSE),
		m_unitsOnly(FALSE),
		m_playerIndex(-1),
		m_sentMap(FALSE),
		m_frameInterval(1),
		m_lastSentFrame(0),
		m_listenSocket(INVALID_SOCKET),
		m_clientSocket(INVALID_SOCKET),
		m_delta(FALSE),
		m_keyInterval(30),
		m_sinceKeyframe(0)
{
}

// ------------------------------------------------------------------------------------------------
/**
 * Switch the stream to delta encoding.
 *
 * Off by default, so every existing tool and recording keeps working
 * untouched; a snapshot stream carries no "delta" marker and is read exactly
 * as it always was.
 */
void ObservationServer::setDelta( Bool on, UnsignedInt keyInterval )
{
	m_delta = on;
	if (keyInterval > 0)
		m_keyInterval = keyInterval;
	m_prevObjects.clear();
	m_sinceKeyframe = 0;
}

// ------------------------------------------------------------------------------------------------
ObservationServer::~ObservationServer()
{
	shutdown();
}

// ------------------------------------------------------------------------------------------------
void ObservationServer::init( UnsignedShort port, UnsignedInt frameInterval )
{
	if (m_enabled)
		return;

	WORD verReq = MAKEWORD(2, 2);
	WSADATA wsadata;
	if (WSAStartup(verReq, &wsadata) != 0)
	{
		DEBUG_LOG(("ObservationServer: WSAStartup failed"));
		return;
	}

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		DEBUG_LOG(("ObservationServer: socket() failed"));
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
		DEBUG_LOG(("ObservationServer: could not listen on port %d", port));
		closesocket(listenSock);
		WSACleanup();
		return;
	}

	// Never block the logic thread waiting for an agent.
	u_long nonBlocking = 1;
	ioctlsocket(listenSock, FIONBIO, &nonBlocking);

	m_listenSocket = (UnsignedInt)listenSock;
	m_frameInterval = frameInterval > 0 ? frameInterval : 1;
	m_unitsOnly = TheGlobalData->m_observationUnitsOnly;
	m_playerIndex = TheGlobalData->m_observationPlayer;
	m_enabled = TRUE;
	// TheSuperHackers @fix -obsplayer -1 means "no observing player": the
	// full view of everything, which is right for after-action analysis and
	// WRONG for an agent playing a live match -- it would see through the
	// shroud. In a network game the slot depends on join order, so it cannot
	// be named on the command line either. -obsplayer -2 means "whichever
	// slot this machine controls", resolved when the game is running (see
	// resolvePlayerIndex, called from update()).
	m_bindToLocalPlayer = (m_playerIndex == OBSERVE_LOCAL_PLAYER);
	if (m_bindToLocalPlayer)
		m_playerIndex = -1;

	DEBUG_LOG(("ObservationServer: listening on 127.0.0.1:%d, every %d frame(s)",
		port, m_frameInterval));
}

// ------------------------------------------------------------------------------------------------
void ObservationServer::shutdown()
{
	if (!m_enabled)
		return;

	if (m_clientSocket != INVALID_SOCKET)
		closesocket((SOCKET)m_clientSocket);
	if (m_listenSocket != INVALID_SOCKET)
		closesocket((SOCKET)m_listenSocket);

	m_clientSocket = INVALID_SOCKET;
	m_listenSocket = INVALID_SOCKET;

	WSACleanup();
	m_enabled = FALSE;
}

// ------------------------------------------------------------------------------------------------
void ObservationServer::acceptClient()
{
	SOCKET sock = accept((SOCKET)m_listenSocket, nullptr, nullptr);
	if (sock == INVALID_SOCKET)
		return;	// nothing pending; the listen socket is non blocking

	u_long nonBlocking = 1;
	ioctlsocket(sock, FIONBIO, &nonBlocking);

	// Observations are latency sensitive and small, so coalescing hurts here.
	BOOL noDelay = TRUE;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&noDelay, sizeof(noDelay));

	m_clientSocket = (UnsignedInt)sock;
	m_sentMap = FALSE;
	// A new client has never seen any object, so the next observation must be
	// a KEYFRAME -- diffing against a world it does not have would hand it a
	// permanently wrong state.
	m_prevObjects.clear();
	m_sinceKeyframe = 0;	// each new agent needs the static map first
	DEBUG_LOG(("ObservationServer: agent connected"));
}

// ------------------------------------------------------------------------------------------------
void ObservationServer::sendRaw( const std::string &payload )
{
	sendRaw(payload.c_str(), (Int)payload.size());
}

// ------------------------------------------------------------------------------------------------
void ObservationServer::sendRaw( const char *data, Int length )
{
	Int remaining = length;
	Int offset = 0;

	// A large message (the static map runs to megabytes) will not fit in the
	// socket buffer at once, so a full buffer is normal rather than a sign of a
	// stalled agent. Wait briefly for room and give up only if the agent makes
	// no progress at all, so the simulation is never held hostage by a client
	// that has stopped reading.
	// The static map is now a real terrain grid rather than an empty one, so it
	// runs to a few hundred kilobytes and a client in a slower language can
	// legitimately take several seconds to drain it. One second was enough when
	// the grid was empty; it is not now, and dropping the agent mid-map leaves
	// it with a truncated line and no way to recover.
	// Raised again for the late game. An observation of a 30-minute match
	// runs to ~38 KB (a big army, many known objects), and the agent's own
	// work per tick grows with that army: eighty order sends in one tick,
	// each owing an ack to drain. Thirty seconds was not enough headroom --
	// the engine hung up on a perfectly healthy agent at 17:44 and again at
	// 24:08 of thirty-minute matches, and the game played on with nothing
	// driving it, which looks exactly like a bot crash and silently
	// corrupts every measurement taken from that match.
	//
	// This only ever fires for an agent that has genuinely stopped reading,
	// and the cost of waiting longer for one that has is a few idle seconds
	// at the end of a match; the cost of giving up too early is the match.
	Int stalledAttempts = 0;
	// Three minutes was still not enough. Measured on a 3-player Flash
	// Effect: the ENGINE took 3 min 7 s of wall time to advance five game
	// minutes late in the match -- it is the simulation that crawls under
	// load, not the agent that stops reading. Give it fifteen minutes.
	//
	// The cost of being generous is bounded and dull: a genuinely dead
	// agent leaves the engine writing into a socket nobody drains until
	// this expires, at the end of a match that is already over. The cost of
	// being strict is the match itself, and a corrupted measurement that
	// looks exactly like a bot bug.
	const Int MAX_STALLED_ATTEMPTS = 90000;		// ~15 minutes at 10ms per attempt

	while (remaining > 0)
	{
		Int sent = send((SOCKET)m_clientSocket, data + offset, remaining, 0);
		if (sent > 0)
		{
			offset += sent;
			remaining -= sent;
			stalledAttempts = 0;
			continue;
		}

		if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			if (++stalledAttempts > MAX_STALLED_ATTEMPTS)
			{
				DEBUG_LOG(("ObservationServer: agent not reading, dropping connection"));
				closesocket((SOCKET)m_clientSocket);
				m_clientSocket = INVALID_SOCKET;
				return;
			}

			// Give the reader a moment to drain what we already sent.
			fd_set writable;
			FD_ZERO(&writable);
			FD_SET((SOCKET)m_clientSocket, &writable);
			struct timeval briefWait;
			briefWait.tv_sec = 0;
			briefWait.tv_usec = 10 * 1000;
			select(0, nullptr, &writable, nullptr, &briefWait);
			continue;
		}

		DEBUG_LOG(("ObservationServer: agent disconnected (error %d)", WSAGetLastError()));
		closesocket((SOCKET)m_clientSocket);
		m_clientSocket = INVALID_SOCKET;
		return;
	}
}

// ------------------------------------------------------------------------------------------------
/**
	Is this object worth reporting to an agent?

	Ownership is not a useful test: the map's scenery and its civilian
	structures both belong to non playing players, so everything on the map has
	a controlling player. Filter on what the object *is* instead. Units and
	structures always matter. Neutral buildings matter when infantry can
	garrison them or when they can be captured for income or tech; plain
	scenery (walls, trees, trains, uninhabitable houses) does not.
*/
/** OBS_MASK (environment, diagnostic only): bit 1 skips the per-player map
	sections, bit 2 the per-player player block, bit 4 the per-player object
	filtering, bit 8 the under-attack list. Used to bisect which part of a
	player-perspective observation perturbs the simulation. */
static Int obsMask()
{
	static Int mask = -1;
	if (mask < 0)
	{
		const char *e = getenv("OBS_MASK");
		mask = e ? atoi(e) : 0;
	}
	return mask;
}

static Bool isTacticallyRelevant( const ThingTemplate *tmpl )
{
	if (tmpl->isKindOf(KINDOF_INFANTRY) ||
			tmpl->isKindOf(KINDOF_VEHICLE) ||
			tmpl->isKindOf(KINDOF_AIRCRAFT) ||
			tmpl->isKindOf(KINDOF_STRUCTURE))
		return TRUE;

	return tmpl->isKindOf(KINDOF_GARRISONABLE_UNTIL_DESTROYED) ||
				 tmpl->isKindOf(KINDOF_CAPTURABLE) ||
				 tmpl->isKindOf(KINDOF_TECH_BUILDING);
}

// ------------------------------------------------------------------------------------------------
/**
	Serialize the parts of the map that never change: its bounds, a coarse
	height sample and a passability grid. This is sent once when an agent
	connects rather than every frame, because it can be tens of thousands of
	values and none of it moves.

	The grid is sampled at the pathfinder's own cell size, so a passability
	entry lines up with the cells the engine actually pathfinds over.
*/
/**
 * Is this pathfind cell walkable by a ground unit?
 *
 * The catch is bridges. The engine does not put a bridge deck on the ground
 * map at all -- each bridge gets its own PathfindLayer (GameType.h: layers 2
 * through LAYER_LAST-1 are bridges), and the ground cells UNDERNEATH a deck
 * are deliberately marked CELL_BRIDGE_IMPASSABLE so that units do not walk
 * through the pilings. Asking LAYER_GROUND alone therefore reports every
 * bridge on every map as a solid wall, which is exactly what it did: the bot
 * had no concept of a bridge and would only ever cross water where the map
 * happened to also provide land.
 *
 * So consider the bridge layers too, and take the most permissive answer --
 * a cell is walkable if the ground is walkable OR any intact bridge spans it.
 *
 * A destroyed bridge is skipped, because it genuinely is impassable. Bridges
 * are ordinary damageable objects (BridgeBehavior::onBodyDamageStateChange
 * drops them to BODY_RUBBLE), so this is live state, not a map constant.
 */
static Bool cellIsWalkable( Int cx, Int cy )
{
	Pathfinder *pf = TheAI->pathfinder();

	// Pathfinder::getCell falls back to the ground map whenever a layer has
	// no cell at these coordinates, so a layer that does not reach here
	// simply re-reports the ground answer rather than lying. That also means
	// an unused layer costs us nothing but a repeated ground lookup.
	//
	// A destroyed bridge needs no special case: PathfindLayer::setDestroyed
	// re-runs classifyCells, which re-types every deck cell to
	// CELL_BRIDGE_IMPASSABLE, and that is rejected below like any other
	// impassable cell.
	for (Int layer = LAYER_GROUND; layer <= LAYER_LAST; ++layer)
	{
		PathfindCell *cell = pf->getCell((PathfindLayerEnum)layer, cx, cy);
		// Off the edge of the pathfind grid is not walkable either.
		if (cell == nullptr)
			continue;

		const PathfindCell::CellType t = cell->getType();
		if (t != PathfindCell::CELL_WATER &&
				t != PathfindCell::CELL_CLIFF &&
				t != PathfindCell::CELL_IMPASSABLE &&
				t != PathfindCell::CELL_BRIDGE_IMPASSABLE)
			return TRUE;
	}
	return FALSE;
}

void ObservationServer::buildMapDescription()
{

	AsciiString scratch;

	Region3D extent;
	extent.lo.zero();
	extent.hi.zero();
	if (TheTerrainLogic != nullptr)
		TheTerrainLogic->getExtent(&extent);

	// TerrainLogic::getExtent() is only implemented by W3DTerrainLogic, so a
	// headless run gets the base class stub and a zero extent -- which left the
	// height and passability grids empty and the agent effectively blind. The
	// map cache reads the same dimensions straight out of the .map file with no
	// renderer involved, so fall back to it rather than shipping an empty map.
	const MapMetaData *meta = nullptr;
	if (TheMapCache != nullptr && !TheGlobalData->m_mapName.isEmpty())
		meta = TheMapCache->findMap(TheGlobalData->m_mapName);

	if (extent.hi.x - extent.lo.x <= 0.0f && meta != nullptr)
		extent = meta->m_extent;

	// The grid is sampled out of the pathfinder, so it has to describe the same
	// area the pathfinder covers. The map cache extent above is read from the
	// .map file and is a whole sample cell larger, so using it would walk off
	// the end of the pathfind grid and report every cell impassable. Prefer the
	// pathfinder's own extent whenever it has one.
	if (TheAI != nullptr && TheAI->pathfinder() != nullptr)
	{
		const ICoord2D *pfHi = TheAI->pathfinder()->getExtent();
		if (pfHi != nullptr && pfHi->x > 0 && pfHi->y > 0)
		{
			extent.lo.x = 0.0f;
			extent.lo.y = 0.0f;
			extent.hi.x = (Real)(pfHi->x + 1) * PATHFIND_CELL_SIZE_F;
			extent.hi.y = (Real)(pfHi->y + 1) * PATHFIND_CELL_SIZE_F;
		}
	}

	const Real width = extent.hi.x - extent.lo.x;
	const Real height = extent.hi.y - extent.lo.y;
	const Int cellsX = (width > 0.0f) ? (Int)(width / MAP_SAMPLE_SIZE_F) : 0;
	const Int cellsY = (height > 0.0f) ? (Int)(height / MAP_SAMPLE_SIZE_F) : 0;

	// AsciiString reallocates on every concat, so building tens of thousands
	// of samples through it is quadratic. Assemble the bulk in a std::string
	// with the capacity reserved up front, then hand it over once.
	std::string body;
	body.reserve((size_t)(cellsX * cellsY * 8) + 4096);

	// Sized for the longest line below with room to spare, and every write into
	// it is bounded by snprintf. The header line alone is ~145 bytes with real
	// map extents, and a 128 byte buffer here was a 17 byte stack overrun that
	// clobbered the return address and took the process down as soon as the
	// map had been sent.
	char chunk[512];
	// Everything time related is reported in logic frames, never seconds: the
	// simulation always advances LOGICFRAMES_PER_SECOND frames per second of
	// game time, but how fast those frames elapse in real time depends on the
	// frame limit (and -noFPSLimit removes it entirely). An agent that reasons
	// in frames behaves identically at any speed; one that reasons in seconds
	// does not.
	snprintf(chunk, sizeof(chunk), "{\"type\":\"map\",\"lo_x\":%.1f,\"lo_y\":%.1f,"
								 "\"hi_x\":%.1f,\"hi_y\":%.1f,\"cell\":%d,"
								 "\"cells_x\":%d,\"cells_y\":%d,"
								 "\"logic_frames_per_second\":%d,\"fps_limit\":%d",
		extent.lo.x, extent.lo.y, extent.hi.x, extent.hi.y,
		(Int)MAP_SAMPLE_SIZE, cellsX, cellsY,
		(Int)LOGICFRAMES_PER_SECOND,
		TheGlobalData->m_useFpsLimit ? TheGlobalData->m_framesPerSecondLimit : 0);
	body += chunk;

	body += ",\"kind_bits\":[";
	for (Int kb = 0; kb < KIND_BIT_COUNT; ++kb)
	{
		if (kb > 0)
			body += ',';
		body += '"';
		body += KIND_BIT_NAMES[kb];
		body += '"';
	}
	body += ']';

	// Row major from (lo_x, lo_y): ground height at each sample point, and
	// whether that point is a cliff (which is what stops ground movement).
	body += ",\"height\":[";
	Int cx, cy;
	for (cy = 0; cy < cellsY; ++cy)
	{
		for (cx = 0; cx < cellsX; ++cx)
		{
			const Real wx = extent.lo.x + (cx + 0.5f) * MAP_SAMPLE_SIZE_F;
			const Real wy = extent.lo.y + (cy + 0.5f) * MAP_SAMPLE_SIZE_F;
			const Real h = (TheTerrainLogic != nullptr)
										 ? TheTerrainLogic->getGroundHeight(wx, wy) : 0.0f;
			if (cx || cy)
				body += ',';
			snprintf(chunk, sizeof(chunk), "%.0f", h);
			body += chunk;
		}
	}
	body += "]";

	body += ",\"passable\":[";
	for (cy = 0; cy < cellsY; ++cy)
	{
		for (cx = 0; cx < cellsX; ++cx)
		{
			const Real wx = extent.lo.x + (cx + 0.5f) * MAP_SAMPLE_SIZE_F;
			const Real wy = extent.lo.y + (cy + 0.5f) * MAP_SAMPLE_SIZE_F;
			// TerrainLogic::isCliffCell is another renderer-backed stub that just
			// returns false headless, which reported the whole map as walkable.
			// The pathfinder is pure logic and already holds exactly this answer
			// for every cell, so ask it instead -- and it accounts for water and
			// impassable objects, not only cliffs.
			Bool passable = TRUE;
			if (TheAI != nullptr && TheAI->pathfinder() != nullptr)
			{
				passable = cellIsWalkable(
					REAL_TO_INT(wx / PATHFIND_CELL_SIZE_F),
					REAL_TO_INT(wy / PATHFIND_CELL_SIZE_F));
			}
			else if (TheTerrainLogic != nullptr)
			{
				passable = !TheTerrainLogic->isCliffCell(wx, wy);
			}
			if (cx || cy)
				body += ',';
			body += passable ? '1' : '0';
		}
	}
	body += "]";

	// The map's fixed points of interest, read from the .map file by the map
	// cache with no renderer involved. These are what a player sees on the
	// loading screen minimap and reasons about before the match even starts:
	// where the supply docks are, where the neutral tech buildings are, and
	// where every start position is. Without them an agent cannot tell one
	// dock from another, cannot expand deliberately, and has no way to work out
	// which direction its enemy lies in.
	if (meta != nullptr)
	{
		Coord3DList::const_iterator it;

		body += ",\"supply_docks\":[";
		Bool firstDock = TRUE;
		for (it = meta->m_supplyPositions.begin();
				 it != meta->m_supplyPositions.end(); ++it)
		{
			if (!firstDock)
				body += ",";
			firstDock = FALSE;
			snprintf(chunk, sizeof(chunk), "{\"x\":%.1f,\"y\":%.1f}", it->x, it->y);
			body += chunk;
		}
		body += "]";

		body += ",\"tech_buildings\":[";
		Bool firstTech = TRUE;
		for (it = meta->m_techPositions.begin();
				 it != meta->m_techPositions.end(); ++it)
		{
			if (!firstTech)
				body += ",";
			firstTech = FALSE;
			snprintf(chunk, sizeof(chunk), "{\"x\":%.1f,\"y\":%.1f}", it->x, it->y);
			body += chunk;
		}
		body += "]";

		body += ",\"start_positions\":[";
		Bool firstWay = TRUE;
		WaypointMap::const_iterator w;
		for (w = meta->m_waypoints.begin(); w != meta->m_waypoints.end(); ++w)
		{
			// The multiplayer start spots are named Player_N_Start; every other
			// waypoint is script scaffolding and means nothing to an agent.
			if (strstr(w->first.str(), "Player_") == nullptr)
				continue;
			if (!firstWay)
				body += ",";
			firstWay = FALSE;
			snprintf(chunk, sizeof(chunk), "{\"name\":\"%.48s\",\"x\":%.1f,\"y\":%.1f}",
				w->first.str(), w->second.x, w->second.y);
			body += chunk;
		}
		body += "]";
	}

	// The map's authored waypoint paths. The skirmish AI attacks along paths
	// labelled Center<N>, Flank<N> and Backdoor<N> into player N's base
	// (ScriptActions::doTeamFollowSkirmishApproachPath); a human sees the
	// same terrain and picks the same routes, so an agent may read them. Only
	// waypoints on a labelled path are sent, with their outgoing links, so the
	// agent can walk a path from either end.
	body += ",\"paths\":[";
	if (TheTerrainLogic != nullptr)
	{
		Bool firstPath = TRUE;
		for (Waypoint *wp = TheTerrainLogic->getFirstWaypoint(); wp != nullptr; wp = wp->getNext())
		{
			const AsciiString &l1 = wp->getPathLabel1();
			const AsciiString &l2 = wp->getPathLabel2();
			const AsciiString &l3 = wp->getPathLabel3();
			if (l1.isEmpty() && l2.isEmpty() && l3.isEmpty())
				continue;
			if (!firstPath)
				body += ",";
			firstPath = FALSE;
			snprintf(chunk, sizeof(chunk), "{\"name\":\"%.48s\",\"x\":%.1f,\"y\":%.1f,\"labels\":[",
				wp->getName().str(), wp->getLocation()->x, wp->getLocation()->y);
			body += chunk;
			Bool firstLabel = TRUE;
			const AsciiString *labels[3] = { &l1, &l2, &l3 };
			for (Int li = 0; li < 3; ++li)
			{
				if (labels[li]->isEmpty())
					continue;
				if (!firstLabel)
					body += ",";
				firstLabel = FALSE;
				snprintf(chunk, sizeof(chunk), "\"%.48s\"", labels[li]->str());
				body += chunk;
			}
			body += "],\"links\":[";
			for (Int ln = 0; ln < wp->getNumLinks(); ++ln)
			{
				Waypoint *to = wp->getLink(ln);
				if (to == nullptr)
					continue;
				snprintf(chunk, sizeof(chunk), "%s\"%.48s\"", ln ? "," : "", to->getName().str());
				body += chunk;
			}
			body += "]}";
		}
	}
	body += "]";

	// Everything the observing player may build, with the numbers shown on the
	// build palette. Static for the match, so it travels with the map. Costs
	// come from the templates, so a mod's own units and prices come through.
	const Player *observing = (m_playerIndex >= 0 && ThePlayerList != nullptr && !(obsMask() & 1))
														? ThePlayerList->getNthPlayer(m_playerIndex) : nullptr;
	if (observing != nullptr && TheThingFactory != nullptr)
	{
		// The counter table, straight from Armor.ini: which damage types each
		// armor class shrugs off or suffers. This is the same information the
		// manual's "strong against / weak against" text paraphrases, and it is
		// what the engine actually computes with. Damage type names are listed
		// once; each buildable template then names its armor row ("armor") and
		// its primary weapon's damage type ("dmg") by index, and the rows
		// themselves follow the catalogue as "armors".
		body += ",\"damage_types\":[";
		for (Int dt = 0; dt < (Int)DAMAGE_NUM_TYPES; ++dt)
		{
			if (dt > 0)
				body += ',';
			body += '"';
			body += DamageTypeFlags::s_bitNameList[dt];
			body += '"';
		}
		body += ']';
		std::vector<const ArmorTemplate *> armorRows;

		body += ",\"buildable\":[";

		Bool firstTmpl = TRUE;
		for (const ThingTemplate *t = TheThingFactory->firstTemplate();
				 t; t = t->friend_getNextTemplate())
		{
			if (!observing->allowedToBuild(t))
				continue;
			const Int cost = t->calcCostToBuild(observing);
			if (cost <= 0)
				continue;

			// A template with BuildVariations (the GLA Technical) is a hollow
			// shell: ordering it produces one of its variations, and the shell
			// itself carries no body and no weapons. Describe it by its first
			// variation -- they are functionally identical -- and name them
			// all below, so the bot can count what comes out as what it ordered.
			const ThingTemplate *st = t;
			if (!t->getBuildVariations().empty())
			{
				const ThingTemplate *v = TheThingFactory->findTemplate(t->getBuildVariations()[0]);
				if (v != nullptr)
					st = v;
			}

			Int armorIdx = -1;
			const ArmorTemplateSet *aset = st->findArmorTemplateSet(ArmorSetFlags());
			const ArmorTemplate *armor = aset ? aset->getArmorTemplate() : nullptr;
			if (armor != nullptr)
			{
				for (size_t ai = 0; ai < armorRows.size(); ++ai)
					if (armorRows[ai] == armor)
						armorIdx = (Int)ai;
				if (armorIdx < 0)
				{
					armorRows.push_back(armor);
					armorIdx = (Int)armorRows.size() - 1;
				}
			}
			Int dmgIdx = -1;
			Real damage = 0.0f;
			Int shotDelay = 0;	// milliseconds between shots
			Int anti = 0;			// WeaponAntiMaskType bits: what the weapon may target
			Int clipSize = 0;		// rounds before reloading (0 = unlimited)
			Int clipReload = 0;	// milliseconds to reload a spent clip
			Real splash = 0.0f;	// primary damage radius
			Real minRange = 0.0f;	// cannot fire closer than this
			Real scatter = 0.0f;	// shot deviation radius
			// Every weapon the template carries, not just the primary. An
			// Overlord's main gun is anti-tank and reads as 6 damage a second
			// against infantry; exporting only that made the combat estimator
			// predict twenty Red Guards beat three Overlords, when the engine
			// says they are wiped out without landing a scratch. The fields
			// below stay as they were, for anything already reading them.
			AsciiString weapons;
			weapons.concat(",\"weapons\":[");
			{
				const WeaponTemplateSetVector &allSets = st->getWeaponTemplateSets();
				Bool firstW = TRUE;
				if (!allSets.empty())
				{
					for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
					{
						const WeaponTemplate *wt =
							allSets[0].getNth((WeaponSlotType)slot);
						if (wt == nullptr)
							continue;

						// Frames, not milliseconds, in the template -- see the
						// conversion on shotDelay below.
						const Int wDelay = ((wt->getMinDelayBetweenShots()
						                     + wt->getMaxDelayBetweenShots()) / 2)
						                   * 1000 / LOGICFRAMES_PER_SECOND;
						const Int wReload = wt->getClipReloadTime(WeaponBonus())
						                    * 1000 / LOGICFRAMES_PER_SECOND;

						if (!firstW)
							weapons.concat(",");
						firstW = FALSE;

						char wchunk[256];
						snprintf(wchunk, sizeof(wchunk),
							"{\"slot\":%d,\"dmg\":%d,\"damage\":%.0f,\"shot_ms\":%d,"
							"\"clip\":%d,\"reload_ms\":%d,\"anti\":%d,\"range\":%.0f,"
							"\"splash\":%.0f}",
							slot, (Int)wt->getDamageType(),
							wt->getPrimaryDamage(WeaponBonus()), wDelay,
							wt->getClipSize(), wReload, wt->getAntiMask(),
							wt->getAttackRange(WeaponBonus()),
							wt->getPrimaryDamageRadius(WeaponBonus()));
						weapons.concat(wchunk);
					}
				}
			}
			weapons.concat("]");

			const WeaponTemplateSetVector &wsets = st->getWeaponTemplateSets();
			if (!wsets.empty())
			{
				const WeaponTemplate *w = wsets[0].getNth(PRIMARY_WEAPON);
				if (w != nullptr)
				{
					dmgIdx = (Int)w->getDamageType();
					damage = w->getPrimaryDamage(WeaponBonus());
					// NOT getDelayBetweenShots(): that one rolls the logic RNG for a
					// value in [min,max], and describing the weapon in the map message
					// advanced the RNG -- every player-perspective replay desynced at
					// its first CRC check (frame 110). Report the midpoint instead.
					// Both of these are FRAMES in the weapon template, and the
					// fields are named in milliseconds, so they must be
					// converted. Exporting the raw frame counts made every
					// clipped weapon look ~33x faster than it fires: an
					// Overlord read as 2051 sustained dps and beat eight
					// Battlemasters in 0.7 seconds.
					shotDelay = ((w->getMinDelayBetweenShots()
					              + w->getMaxDelayBetweenShots()) / 2)
					            * 1000 / LOGICFRAMES_PER_SECOND;
					anti = w->getAntiMask();
					// Sustained fire needs the clip too: delayBetweenShots is
					// only the animation gap between rounds in a magazine, so
					// using it alone makes a siege gun look like a machine gun.
					clipSize = w->getClipSize();
					clipReload = w->getClipReloadTime(WeaponBonus())
					             * 1000 / LOGICFRAMES_PER_SECOND;
					// Splash: what actually makes a weapon good against
					// massed infantry.
					splash = w->getPrimaryDamageRadius(WeaponBonus());
					// A siege gun cannot shoot what has closed inside its
					// minimum range, and scatter is why artillery misses
					// moving infantry. Both are needed to tell "high damage"
					// apart from "actually kills that target".
					minRange = w->getMinimumAttackRange();
					scatter = w->getScatterRadius();
				}
			}

			if (!firstTmpl)
				body += ',';
			firstTmpl = FALSE;

			// Footprint and weapon range are what a player sees in the placement
			// cursor and the tooltip; they let a client reason about spacing and
			// engagement distance without a hand-kept table.
			// Max health. A fight is damage AND durability -- without this a
			// client comparing two units is scoring a race in which only one
			// side shoots, which is how a 800hp flame tank read as beating a
			// tank that outranges it and has far more armour.
			Real maxHealth = 0.0f;
			// Infantry squishing is gated on a SquishCollide module, NOT on
			// the crushable level (Object::canCrushOrSquish). Report it so a
			// client can tell "drive over this" from "shoot this".
			Int squishable = 0;
			{
				const ModuleInfo &mi = st->getBehaviorModuleInfo();
				for (Int sIdx = 0; sIdx < mi.getCount(); ++sIdx)
				{
					if (strcmp(mi.getNthName(sIdx).str(), "SquishCollide") == 0)
					{
						squishable = 1;
						break;
					}
				}
			}
			{
				const ModuleInfo &mi = st->getBehaviorModuleInfo();
				for (Int mIdx = 0; mIdx < mi.getCount(); ++mIdx)
				{
					const AsciiString mName = mi.getNthName(mIdx);
					if (strstr(mName.str(), "Body") == nullptr)
						continue;
					const BodyModuleData *bmd = (const BodyModuleData *)mi.getNthData(mIdx);
					if (bmd != nullptr)
					{
						maxHealth = ((const ActiveBodyModuleData *)bmd)->m_maxHealth;
						break;
					}
				}
			}

			const GeometryInfo &geom = st->getTemplateGeometryInfo();
			snprintf(chunk, sizeof(chunk), "{\"type\":\"%.48s\",\"tid\":%d,\"cost\":%d,\"build_frames\":%d,\"prereqs_met\":%d,"
				"\"k\":%u,\"footprint\":[%.0f,%.0f],\"range\":%.0f,"
				"\"armor\":%d,\"dmg\":%d,\"damage\":%.0f,\"shot_ms\":%d,\"anti\":%d,"
				"\"clip\":%d,\"reload_ms\":%d,\"splash\":%.0f,"
				"\"min_range\":%.0f,\"scatter\":%.0f,\"hp\":%.0f,"
				"\"crusher\":%d,\"crushable\":%d,\"squishable\":%d,"
				// Power. Positive produces, negative consumes -- straight from
				// the INI's EnergyProduction (the header comment in
				// ThingTemplate.h claims the opposite and is stale; the field
				// is parsed verbatim). A superweapon is the first building
				// whose draw the bot cannot absorb by accident: China's
				// Nuclear Missile Launcher is -10 against a Power Plant's +5.
				"\"energy\":%d}",
				t->getName().str(), (Int)t->getTemplateID(), cost, t->calcTimeToBuild(observing),
				observing->canBuild(t) ? 1 : 0,
				kindMask(t), geom.getMajorRadius(), geom.getMinorRadius(),
				templateRange(st), armorIdx, dmgIdx, damage, shotDelay, anti,
				clipSize, clipReload, splash, minRange, scatter, maxHealth,
				(Int)st->getCrusherLevel(), (Int)st->getCrushableLevel(), squishable,
				(Int)t->getEnergyProduction());
			// Splice the weapons array in before the closing brace.
			body.append(chunk, strlen(chunk) - 1);
			body += weapons.str();
			if (!t->getBuildVariations().empty())
			{
				body += ",\"variations\":[";
				const std::vector<AsciiString> &vars = t->getBuildVariations();
				for (size_t vi = 0; vi < vars.size(); ++vi)
				{
					snprintf(chunk, sizeof(chunk), "%s\"%.48s\"", vi ? "," : "", vars[vi].str());
					body += chunk;
				}
				body += ']';
			}
			body += '}';
		}
		body += ']';

		body += ",\"armors\":[";
		for (size_t ai = 0; ai < armorRows.size(); ++ai)
		{
			if (ai > 0)
				body += ',';
			body += '[';
			for (Int dt = 0; dt < (Int)DAMAGE_NUM_TYPES; ++dt)
			{
				snprintf(chunk, sizeof(chunk), "%s%.2f", dt > 0 ? "," : "",
					armorRows[ai]->adjustDamage((DamageType)dt, 1.0f));
				body += chunk;
			}
			body += ']';
		}
		body += ']';
	}

	// The rest of the action space, so an agent can discover what it may ask
	// for rather than carry a hardcoded table that a mod would invalidate.
	// These are names the action server accepts verbatim.
	if (TheScienceStore != nullptr)
	{
		body += ",\"sciences\":[";
		std::vector<AsciiString> sciNames = TheScienceStore->friend_getScienceNames();
		Bool firstSci = TRUE;
		for (size_t i = 0; i < sciNames.size(); ++i)
		{
			const ScienceType st =
				TheScienceStore->getScienceFromInternalName(sciNames[i]);
			if (st == SCIENCE_INVALID || !TheScienceStore->isValidScience(st))
				continue;

			if (!firstSci)
				body += ',';
			firstSci = FALSE;

			// Faction and rank placeholders sit in the same store; only what this
			// player could actually buy right now is flagged purchasable.
			// "id" is the ScienceType, which is the name's interned KEY, not
			// an index: it is what MSG_PURCHASE_SCIENCE carries in a replay,
			// so the corpus tools resolve a purchase by it.
			snprintf(chunk, sizeof(chunk), "{\"name\":\"%.48s\",\"id\":%d,\"cost\":%d,\"purchasable\":%d}",
				sciNames[i].str(), (Int)st, TheScienceStore->getSciencePurchaseCost(st),
				(observing != nullptr && observing->isCapableOfPurchasingScience(st)) ? 1 : 0);
			body += chunk;
		}
		body += ']';
	}

	if (TheUpgradeCenter != nullptr)
	{
		// UpgradeTemplate's cost accessors take a non-const Player even though
		// they only read it, so fetch the player again rather than cast.
		Player *observingRW = (m_playerIndex >= 0 && ThePlayerList != nullptr && !(obsMask() & 1))
													? ThePlayerList->getNthPlayer(m_playerIndex) : nullptr;

		body += ",\"upgrades\":[";
		Bool firstUp = TRUE;
		for (const UpgradeTemplate *u = TheUpgradeCenter->firstUpgradeTemplate();
				 u; u = u->friend_getNext())
		{
			if (!firstUp)
				body += ',';
			firstUp = FALSE;

			// "id" is the interned name key, which is what MSG_QUEUE_UPGRADE
			// carries in argument 1 -- so a replay's upgrade orders can be
			// resolved to a name. It is a plain member read, no RNG: an
			// observer that rolls the logic RNG desyncs the simulation.
			snprintf(chunk, sizeof(chunk), "{\"name\":\"%.48s\",\"id\":%d,\"cost\":%d,\"build_frames\":%d}",
				u->getUpgradeName().str(), (Int)u->getUpgradeNameKey(),
				(Int)u->calcCostToBuild(observingRW),
				(Int)u->calcTimeToBuild(observingRW));
			body += chunk;
		}
		body += ']';
	}

	if (TheSpecialPowerStore != nullptr)
	{
		body += ",\"special_powers\":[";
		const Int numPowers = TheSpecialPowerStore->getNumSpecialPowers();
		Bool firstSp = TRUE;
		for (Int i = 0; i < numPowers; ++i)
		{
			const SpecialPowerTemplate *sp =
				TheSpecialPowerStore->getSpecialPowerTemplateByIndex((UnsignedInt)i);
			if (sp == nullptr)
				continue;

			if (!firstSp)
				body += ',';
			firstSp = FALSE;

			snprintf(chunk, sizeof(chunk), "{\"name\":\"%.48s\",\"reload_frames\":%d}",
				sp->getName().str(), (Int)sp->getReloadTime());
			body += chunk;
		}
		body += ']';
	}

	body += "}\n";

	// Sent straight from the std::string rather than staged through an AsciiString:
	// AsciiString caps at MAX_LEN (32767) characters and a real map grid runs
	// to hundreds of kilobytes, so staging it there silently truncated the
	// message and the agent saw a broken line.
	sendRaw(body);
}

// ------------------------------------------------------------------------------------------------
/**
	Serialize the frame as a single line of JSON, from the point of view of one
	player.

	The observing player is m_playerIndex. Everything reported is something that
	player could actually determine by looking at their screen: their own economy
	and production in full, and enemy objects only where their shroud says they
	can see them. Fogged enemies are reported at their last known position, the
	way a player remembers what they scouted, and shrouded enemies are omitted
	entirely. With no observing player set (-obsplayer omitted) the full
	omniscient state is emitted, which is useful for analysis and for labelling
	replays but must not be fed to an agent that will later play a real match.
*/
/**
	Shroud status for one object from the partition cells alone, without
	touching any cached state. Mirrors PartitionData::getShroudedStatus's
	classification at the object's centre cell: shrouded cell -> shrouded;
	fogged cell -> fogged for things that do not move (a player's minimap
	remembers buildings), shrouded for anything mobile (units in fog are not
	on the screen at all); otherwise clear.
*/
static ObjectShroudStatus pureShroudStatus( const Object *obj, Int playerIndex )
{
	if (obj->getTemplate()->isKindOf(KINDOF_ALWAYS_VISIBLE))
		return OBJECTSHROUD_CLEAR;
	if (ThePartitionManager == nullptr || playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT)
		return OBJECTSHROUD_CLEAR;
	const Coord3D *pos = obj->getPosition();
	Int cx = 0, cy = 0;
	ThePartitionManager->worldToCell(pos->x, pos->y, &cx, &cy);
	const PartitionCell *cell = ThePartitionManager->getCellAt(cx, cy);
	if (cell == nullptr)
		return OBJECTSHROUD_SHROUDED;
	const CellShroudStatus cs = cell->getShroudStatusForPlayer(playerIndex);
	if (cs == CELLSHROUD_SHROUDED)
		return OBJECTSHROUD_SHROUDED;
	if (cs == CELLSHROUD_FOGGED)
		return obj->isKindOf(KINDOF_IMMOBILE) ? OBJECTSHROUD_FOGGED : OBJECTSHROUD_SHROUDED;
	return OBJECTSHROUD_CLEAR;
}

// ------------------------------------------------------------------------------------------------
/**
 * Split a flat object JSON blob into its top-level "key":value fields.
 *
 * The objects emitted here are flat by construction -- scalars, plus a few
 * small arrays like "footprint":[3,2] and "queue":[...] -- so this tracks
 * bracket depth and string state rather than parsing properly. Anything it
 * cannot split cleanly falls back to "the whole object changed", which is
 * always safe.
 */
static void splitFields( const std::string &blob,
	std::vector< std::pair<std::string, std::string> > &out )
{
	out.clear();
	size_t i = 0;
	const size_t n = blob.size();
	if (n > 0 && blob[0] == '{')
		++i;

	while (i < n)
	{
		while (i < n && (blob[i] == ',' || blob[i] == ' '))
			++i;
		if (i >= n || blob[i] == '}')
			break;
		if (blob[i] != '"')
			return;			// not the shape we expect; caller sends it whole

		const size_t keyStart = ++i;
		while (i < n && blob[i] != '"')
			++i;
		if (i >= n)
			return;
		const std::string key = blob.substr(keyStart, i - keyStart);
		++i;
		if (i >= n || blob[i] != ':')
			return;
		++i;

		const size_t valStart = i;
		Int depth = 0;
		Bool inStr = FALSE;
		while (i < n)
		{
			const char c = blob[i];
			if (inStr)
			{
				if (c == '\\')
					++i;
				else if (c == '"')
					inStr = FALSE;
			}
			else if (c == '"')
				inStr = TRUE;
			else if (c == '[' || c == '{')
				++depth;
			else if (c == ']' || c == '}')
			{
				if (depth == 0)
					break;		// the object's own closing brace
				--depth;
			}
			else if (c == ',' && depth == 0)
				break;
			++i;
		}
		out.push_back(std::make_pair(key, blob.substr(valStart, i - valStart)));
	}
}

/**
 * Reduce one object to the fields that changed since it was last sent.
 *
 * Returns FALSE when nothing changed, so the object can be dropped from the
 * message entirely.
 *
 * TWO KINDS OF CHANGE, and missing the second one is the trap.
 *
 *   1. a field whose VALUE differs -- emitted normally;
 *   2. a field that was present and is now GONE. "hp":870 -> "hp":860 is
 *      obvious; "goal_x":1420.7 -> (absent, because the unit arrived) is not.
 *      Under deltas "absent" means UNCHANGED, so an omitted field would pin a
 *      stale goal on a unit that stopped moving minutes ago. Verified against
 *      a real recording: without this, 1575 of 2160 observations reconstruct
 *      wrongly. Such fields are named in a "clear" list and the reader drops
 *      them.
 *
 * "id" always rides along so the reader knows which object this is about.
 */
Bool ObservationServer::deltaObject( const std::string &full, std::string &out,
	const std::string &prev )
{
	std::vector< std::pair<std::string, std::string> > now, was;
	splitFields(full, now);
	splitFields(prev, was);
	if (now.empty() || was.empty())
	{
		out = full;		// could not split; send it whole rather than guess
		return TRUE;
	}

	std::map<std::string, std::string> wasMap;
	size_t i;
	for (i = 0; i < was.size(); ++i)
		wasMap[was[i].first] = was[i].second;

	std::string body;
	std::string idField;
	for (i = 0; i < now.size(); ++i)
	{
		const std::string &k = now[i].first;
		const std::string &v = now[i].second;
		if (k == "id")
		{
			idField = "\"id\":" + v;
			wasMap.erase(k);
			continue;
		}
		std::map<std::string, std::string>::iterator f = wasMap.find(k);
		if (f == wasMap.end() || f->second != v)
		{
			if (!body.empty())
				body += ',';
			body += "\"" + k + "\":" + v;
		}
		if (f != wasMap.end())
			wasMap.erase(f);
	}

	// Whatever is left in wasMap was present last time and is absent now.
	std::string cleared;
	for (std::map<std::string, std::string>::const_iterator c = wasMap.begin();
			c != wasMap.end(); ++c)
	{
		if (!cleared.empty())
			cleared += ',';
		cleared += "\"" + c->first + "\"";
	}

	if (body.empty() && cleared.empty())
		return FALSE;		// byte-identical: drop it from the message

	out = "{";
	out += idField.empty() ? std::string() : idField;
	if (!body.empty())
	{
		if (!out.empty() && out != "{")
			out += ',';
		out += body;
	}
	if (!cleared.empty())
	{
		if (out != "{")
			out += ',';
		out += "\"clear\":[" + cleared + "]";
	}
	out += "}";
	return TRUE;
}

void ObservationServer::buildObservation( std::string &out )
{
	// Accumulated in a std::string rather than an AsciiString.
	// AsciiStringData::m_numCharsAllocated is an unsigned short and MAX_LEN is
	// 32767, so a busy frame -- hundreds of objects, each with a name, position
	// and production queue -- overruns what that field can express. The
	// "existing buffer is already large enough" test in ensureUniqueBufferOfSize
	// then passes when it should not, and the strcat that follows runs off the
	// end of the allocation, writing JSON over whatever memory comes next.
	AsciiString scratch;

	out.clear();
	out.reserve(64 * 1024);

	const Int observer = m_playerIndex;
	const Player *observingAll = (observer >= 0) ? ThePlayerList->getNthPlayer(observer) : nullptr;
	const Player *observing = (obsMask() & 2) ? nullptr : observingAll;

	/*	Delta bookkeeping for this observation.

		A KEYFRAME every m_keyInterval observations, and always the first one
		after a client connects. Without it a reader that joins mid-match --
		or one that hits a gap -- could never resync, and an error would
		persist silently for the rest of the game. It is also what lets
		stream.py seek: state_at(frame) replays forward from the newest
		keyframe instead of from frame 0.
	*/
	Bool keyframe = FALSE;
	if (m_delta)
	{
		keyframe = (m_sinceKeyframe == 0) ||
			(m_keyInterval > 0 && m_sinceKeyframe >= m_keyInterval);
		if (keyframe)
		{
			m_prevObjects.clear();
			m_sinceKeyframe = 0;
		}
		++m_sinceKeyframe;
		m_seenThisFrame.clear();
	}

	scratch.format("{\"frame\":%d,\"observer\":%d%s%s,\"players\":[",
		TheGameLogic->getFrame(), observer,
		m_delta ? ",\"delta\":1" : "",
		keyframe ? ",\"keyframe\":1" : "");
	out += scratch.str();

	const Int numPlayers = ThePlayerList->getPlayerCount();
	Bool firstPlayer = TRUE;
	for (Int i = 0; i < numPlayers; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr)
			continue;

		if (!firstPlayer)
			out += ',';
		firstPlayer = FALSE;

		// Relationship is public knowledge; a player always knows who their
		// enemies are.
		Int relation = NEUTRAL;
		if (observing != nullptr && player != observing)
			relation = (Int)observing->getRelationship(player->getDefaultTeam());
		else if (player == observing)
			relation = (Int)ALLIES;

		const Bool selfOrNoObserver = (observing == nullptr || player == observing);
		if (selfOrNoObserver)
		{
			// Full economic detail, but only for the observer's own player.
			const Energy *energy = player->getEnergy();
			scratch.format("{\"index\":%d,\"self\":%d,\"relation\":%d,\"defeated\":%d,"
										 "\"money\":%d,\"power_produced\":%d,\"power_consumed\":%d,"
										 "\"has_radar\":%d,\"can_build_units\":%d,\"can_build_base\":%d",
				player->getPlayerIndex(),
				(player == observing) ? 1 : 0,
				relation,
				player->isPlayerActive() ? 0 : 1,
				player->getMoney()->countMoney(),
				energy ? energy->getProduction() : 0,
				energy ? energy->getConsumption() : 0,
				player->hasRadar() ? 1 : 0,
				player->getCanBuildUnits() ? 1 : 0,
				player->getCanBuildBase() ? 1 : 0);

			// The engine's own scorekeeper, which is the reward signal a learning
			// agent needs. Win or lose is a single bit after twenty minutes and
			// far too sparse to learn from; these accumulate continuously and say
			// WHY a game is going well or badly. Same tally the end-of-game score
			// screen shows the player, so it is not privileged information.
			ScoreKeeper *score = const_cast<Player *>(player)->getScoreKeeper();
			if (score != nullptr)
			{
				AsciiString more;
				more.format(",\"score\":%d,\"earned\":%d,\"spent\":%d,"
										"\"units_built\":%d,\"units_lost\":%d,"
										"\"units_killed\":%d,\"buildings_built\":%d,"
										"\"buildings_lost\":%d,\"buildings_killed\":%d",
					score->calculateScore(),
					score->getTotalMoneyEarned(),
					score->getTotalMoneySpent(),
					score->getTotalUnitsBuilt(),
					score->getTotalUnitsLost(),
					score->getTotalUnitsDestroyed(),
					score->getTotalBuildingsBuilt(),
					score->getTotalBuildingsLost(),
					score->getTotalBuildingsDestroyed());
				scratch.concat(more);
			}

			// Generals rank and the points left to spend on it, and which
			// sciences and upgrades this player already owns. All shown on the
			// player's own side bar, so nothing here is hidden information.
			AsciiString owned;
			owned.format(",\"rank\":%d,\"rank_points\":%d,\"sciences_owned\":[",
				player->getRankLevel(), player->getSciencePurchasePoints());
			scratch.concat(owned);
			if (TheScienceStore != nullptr)
			{
				std::vector<AsciiString> sciNames = TheScienceStore->friend_getScienceNames();
				Bool firstOwned = TRUE;
				for (size_t i = 0; i < sciNames.size(); ++i)
				{
					ScienceType st = TheScienceStore->getScienceFromInternalName(sciNames[i]);
					if (!player->hasScience(st))
						continue;
					if (!firstOwned)
						scratch.concat(",");
					firstOwned = FALSE;
					scratch.concat("\"");
					scratch.concat(sciNames[i]);
					scratch.concat("\"");
				}
			}
			scratch.concat("],\"upgrades_owned\":[");
			if (TheUpgradeCenter != nullptr)
			{
				Bool firstOwned = TRUE;
				for (const UpgradeTemplate *u = TheUpgradeCenter->firstUpgradeTemplate();
						 u; u = u->friend_getNext())
				{
					if (!player->hasUpgradeComplete(u))
						continue;
					if (!firstOwned)
						scratch.concat(",");
					firstOwned = FALSE;
					scratch.concat("\"");
					scratch.concat(u->getUpgradeName());
					scratch.concat("\"");
				}
			}
			scratch.concat("]");

			// Flush before the powers block: `scratch` is an AsciiString and
			// its size field is an unsigned short, so the per-player section
			// must not be allowed to grow without bound. See the note at the
			// top of this function.
			out += scratch.str();
			scratch.clear();

			// Special powers this player can actually use, and the frame each
			// comes off cooldown. Both are on the player's own side bar --
			// the shortcut buttons with their recharge clocks -- so nothing
			// here is hidden information.
			//
			// The store has no per-player list, so this walks every power that
			// exists and asks our own objects whether they provide it. All of
			// findSpecialPowerModuleInterface, isReady and getReadyFrame are
			// const reads of module state: nothing here may touch
			// GameLogicRandomValue, which is what desynced every replay at
			// frame 110 when a weapon getter rolled it.
			out += ",\"powers\":[";
			if (TheSpecialPowerStore != nullptr)
			{
				// Fixed cap rather than a heap allocation on the logic thread;
				// the game ships ~80 powers and this is checked below.
				enum { MAX_TRACKED_POWERS = 256 };
				Bool seenPower[MAX_TRACKED_POWERS];
				char powerChunk[256];
				// One pass over our objects, not one per power: a naive
				// powers-by-objects loop is 80 x every object in the world,
				// every observation, and this runs on the logic thread.
				Int numPowers = TheSpecialPowerStore->getNumSpecialPowers();
				if (numPowers > MAX_TRACKED_POWERS)
					numPowers = MAX_TRACKED_POWERS;
				Bool firstPower = TRUE;
				for (Int p = 0; p < numPowers; ++p)
					seenPower[p] = FALSE;

				for (Object *o = TheGameLogic->getFirstObject();
						 o != nullptr; o = o->getNextObject())
				{
					if (o->getControllingPlayer() != player)
						continue;

					for (Int p = 0; p < numPowers; ++p)
					{
						if (seenPower[p])
							continue;
						const SpecialPowerTemplate *spt =
							TheSpecialPowerStore->getSpecialPowerTemplateByIndex((UnsignedInt)p);
						if (spt == nullptr)
							continue;
						SpecialPowerModuleInterface *mod =
							o->findSpecialPowerModuleInterface(spt->getSpecialPowerType());
						if (mod == nullptr)
							continue;

						// The player's own path (Player::findMostReadyShortcutSpecialPowerOfType,
						// via doFindSpecialPowerSourceObject) refuses an object that is
						// under construction, sold or dead, and refuses a module that is
						// script-only. A source that fails any of these is not a source a
						// human could ever have fired from, so it must not be offered.
						if (o->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)
								|| o->testStatus(OBJECT_STATUS_SOLD)
								|| o->isEffectivelyDead()
								|| mod->isScriptOnly())
							continue;

						seenPower[p] = TRUE;

						// Readiness, WITHOUT starting any clock.
						//
						// SpecialPowerModule::getReadyFrame() forwards a SharedSyncedTimer
						// power to Player::getOrStartSpecialPowerReadyFrame(), which CREATES
						// the timer when it is absent. Calling it from here started every
						// superweapon's shared timer at frame 0 -- so the power reported
						// ready:1 ready_frame:0 forever, and the firing path later found a
						// timer the logic never meant to exist. The observer must not write.
						UnsignedInt readyFrame;
						Bool haveFrame;
						if (spt->isSharedNSync())
							haveFrame = player->peekSharedSpecialPowerReadyFrame(spt, readyFrame);
						else
						{
							readyFrame = mod->getReadyFrame();
							haveFrame = TRUE;
						}

						// A shared power with no timer yet has not been unlocked: it is
						// not ready, and it has no meaningful ready frame. Say so rather
						// than inventing one.
						const Bool ready = haveFrame && readyFrame < TheGameLogic->getFrame();

						if (!firstPower)
							out += ',';
						firstPower = FALSE;

						snprintf(powerChunk, sizeof(powerChunk),
							"{\"name\":\"%.48s\",\"source\":%d,\"ready\":%d,\"ready_frame\":%d}",
							spt->getName().str(), (Int)o->getID(),
							ready ? 1 : 0, haveFrame ? (Int)readyFrame : -1);
						out += powerChunk;
					}
				}
			}
			out += "]}";
		}
		else
		{
			/*	An opponent's bank balance and power grid are not
				observable. Whether they have been defeated is announced
				to everyone, and so is their SIDE: every player picks a
				faction in the lobby in full view, so naming it here
				leaks nothing -- it is what lets a human say "attack the
				GLA player" and have the bot know who that is.
			*/
			const PlayerTemplate *ptmpl = player->getPlayerTemplate();
			scratch.format("{\"index\":%d,\"self\":0,\"relation\":%d,\"defeated\":%d,\"side\":\"%.24s\"}",
				player->getPlayerIndex(),
				relation,
				player->isPlayerActive() ? 0 : 1,
				ptmpl ? ptmpl->getSide().str() : "");
		}
		out += scratch.str();
	}

	out += "]";

	// Diagnostics only (-obsdebug): every player's economy, so a harness can
	// compare the agent's income curve against its opponents'. This is
	// hidden information and an agent must never be trained on it; it is
	// for the person reading the log.
	if (TheGlobalData->m_observationDebug)
	{
		out += ",\"debug_players\":[";
		Bool firstDbg = TRUE;
		for (Int i = 0; i < numPlayers; ++i)
		{
			Player *player = ThePlayerList->getNthPlayer(i);
			if (player == nullptr)
				continue;
			ScoreKeeper *score = player->getScoreKeeper();
			if (!firstDbg)
				out += ',';
			firstDbg = FALSE;
			scratch.format("{\"index\":%d,\"money\":%d,\"earned\":%d,\"score\":%d,\"units_built\":%d}",
				player->getPlayerIndex(), player->getMoney()->countMoney(),
				score ? score->getTotalMoneyEarned() : 0,
				score ? score->calculateScore() : 0,
				score ? score->getTotalUnitsBuilt() : 0);
			out += scratch.str();
		}
		out += ']';
	}

	/*	Chat seen since the last observation.

		Emitted before the objects array purely so the big array stays
		last and this cannot disturb its assembly. "scope" is derived
		here rather than in the agent because the mask's meaning is an
		engine detail: a mask naming exactly one slot -- ours -- is a
		private line, one naming a subset is team chat, and the full set
		is global. The raw mask is kept too so the agent can be precise
		if it ever needs to be.
	*/
	out += ",\"chat\":[";
	{
		Bool firstChat = TRUE;
		for (size_t ci = 0; ci < m_chat.size(); ++ci)
		{
			const ChatLine &c = m_chat[ci];
			if (!firstChat)
				out += ',';
			firstChat = FALSE;

			// Count the addressed slots to tell private from team from all.
			// Counted over the whole mask rather than MAX_SLOTS so this
			// file needs no GameNetwork header for one loop bound.
			Int addressed = 0;
			for (UnsignedInt bit = (UnsignedInt)c.playerMask; bit; bit &= bit - 1)
				++addressed;

			const char *scope = "team";
			if (addressed <= 1)
				scope = "private";
			else if (addressed >= numPlayers && numPlayers > 0)
				scope = "global";

			scratch.format("{\"from\":%d,\"mask\":%d,\"frame\":%u,\"scope\":\"%s\",\"text\":\"",
				c.senderSlot, c.playerMask, c.frame, scope);
			out += scratch.str();
			out += c.text;			// already escaped by recordChat
			out += "\"}";
		}
	}
	out += ']';
	// Drained: each line is reported exactly once.
	m_chat.clear();

	out += ",\"objects\":[";

	// "Our base is under attack": every own structure hit in the last few
	// seconds, with where and by whom, gathered while the objects are walked
	// and emitted as one list at the end. A player gets this as an EVA line
	// and a minimap ping without seeing the attacker; so does the agent.
	std::string alerts;
	const UnsignedInt nowFrame = TheGameLogic->getFrame();

	Bool first = TRUE;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		// A GLA salvage crate: neutral, lives 30-35 seconds, and only a GLA
		// vehicle can pick it up. Identified by its collide module, not its
		// name, so a mod's crate reads the same.
		Bool salvageCrate = FALSE;
		for (BehaviorModule **m = obj->getBehaviorModules(); m != nullptr && *m != nullptr; ++m)
		{
			CollideModuleInterface *ci = (*m)->getCollide();
			if (ci != nullptr && ci->isSalvageCrateCollide())
			{
				salvageCrate = TRUE;
				break;
			}
		}

		// Dead objects linger for a frame or two while their death modules run.
		// EXCEPT crates: they have no body, so they get an InactiveBody, which
		// marks them effectively dead from birth (so nothing can shoot them).
		// This filter therefore hid every crate the bot could ever have seen.
		if (obj->isEffectivelyDead() && !salvageCrate)
			continue;

		const ThingTemplate *tmpl = obj->getTemplate();
		if (tmpl == nullptr)
			continue;

		if (m_unitsOnly && !isTacticallyRelevant(tmpl))
			continue;

		const Player *owner = obj->getControllingPlayer();
		if (obsMask() & 2)
			observing = (obsMask() & 4) ? nullptr : observingAll;	// players block masked, object loop not
		else if (obsMask() & 4)
			observing = nullptr;
		const Bool isOwn = (observing != nullptr && owner == observing);

		// Visibility. Own objects are always visible; everything else is
		// subject to this player's shroud.
		ObjectShroudStatus shroud = OBJECTSHROUD_CLEAR;
		if (observing != nullptr && !isOwn)
		{
			// A PURE read of the shroud. Object::getShroudedStatus() is not
			// one: it rewrites the object's cached per-player status, its
			// ever-seen flag, and takes or frees ghost-object snapshots --
			// work the engine otherwise does only for the local player. Done
			// here for an arbitrary player it changed the simulation: every
			// replay observed with -obsplayer went out of sync at the first
			// CRC check (frame 110), while the same replay observed
			// omnisciently ran for minutes. Read the partition cells instead.
			shroud = pureShroudStatus(obj, observer);

			// Never seen, currently unseeable: the player has no idea it exists.
			if (shroud == OBJECTSHROUD_SHROUDED || shroud == OBJECTSHROUD_INVALID)
				continue;

			// A stealthed enemy is invisible unless something detects it
			// (DETECTED is one status for everybody: an ally's detector
			// reveals it to us too, exactly as on a player's screen).
			// Except a unit that DISGUISES (the Bomb Truck): it is never
			// hidden, it is drawn as something else -- StealthUpdate::
			// calcStealthedStatusForPlayer returns NONE for it undisguised
			// and DISGUISED_ENEMY when disguised. Dropping it here made
			// every enemy Bomb Truck invisible, disguised or not.
			const StealthUpdate *st = obj->getStealth();
			const Bool disguiser = (st != nullptr && st->canDisguise());
			if (obj->testStatus(OBJECT_STATUS_STEALTHED) &&
					!obj->testStatus(OBJECT_STATUS_DETECTED) && !disguiser)
				continue;

			// Nor are the men inside a building that hides its garrison:
			// one held only by stealthy garrisoners (KINDOF_STEALTH_
			// GARRISON: Pathfinder, Jarmen Kell, Hijacker) still shows its
			// original owner to non-allies until one of them is detected
			// (GarrisonContain::getApparentControllingPlayer).
			const Object *holder = obj->getContainedBy();
			const ContainModuleInterface *hc = holder ? holder->getContain() : nullptr;
			if (hc != nullptr)
			{
				const Player *app = hc->getApparentControllingPlayer(observing);
				if (app != nullptr && app != holder->getControllingPlayer())
					continue;
			}
		}

		/*	What the observer is SHOWN, which for two stealth cases is not
			the truth, and a player sees only the shown version:
			- a disguised Bomb Truck is drawn as the vehicle it copied, in
			  that vehicle's owner's colour, until detected or it reveals
			  itself (StealthUpdate::changeVisualDisguise);
			- a building garrisoned only by stealthy garrisoners keeps its
			  original owner and shows no garrison.
			Allies see the truth in both cases, as the engine draws it. */
		const ThingTemplate *shownTmpl = tmpl;
		const Player *shownOwner = owner;
		Bool shownDisguised = FALSE;
		Bool hideContents = FALSE;
		const Bool allied = (observing != nullptr && owner != nullptr &&
			owner->getRelationship(observing->getDefaultTeam()) == ALLIES);
		if (observing != nullptr && !isOwn && !allied)
		{
			StealthUpdate *st = obj->getStealth();
			if (st != nullptr && st->canDisguise() && st->isDisguised() &&
					!obj->testStatus(OBJECT_STATUS_DETECTED) && st->getDisguisedTemplate() != nullptr)
			{
				shownTmpl = st->getDisguisedTemplate();
				const Player *as = ThePlayerList->getNthPlayer(st->getDisguisedPlayerIndex());
				if (as != nullptr)
					shownOwner = as;
				shownDisguised = TRUE;
			}
			const ContainModuleInterface *c = obj->getContain();
			if (c != nullptr)
			{
				const Player *app = c->getApparentControllingPlayer(observing);
				if (app != nullptr && app != owner)
				{
					shownOwner = app;
					hideContents = TRUE;
				}
			}
		}

		const Coord3D *pos = obj->getPosition();
		const BodyModuleInterface *body = obj->getBodyModule();

		/*	DELTA ENCODING SEAM -- see docs/OBS_PROTOCOL.md.

			The ~250 lines below append this object's JSON to `out` by name,
			and rewriting them to use a buffer would be a large diff for no
			gain. Instead: remember where this object starts, let the field
			code run exactly as before, and at the bottom of the loop cut
			the text back out of `out` to compare it against what was sent
			for the same object last time.
		*/
		const size_t objStart = out.size();
		if (!first)
			out += ',';
		first = FALSE;

		// "visible" distinguishes something being watched right now from
		// something only remembered: a fogged object reports its last known
		// state, which is what the player's screen still shows.
		const Bool fogged = (shroud == OBJECTSHROUD_FOGGED);

		scratch.format("{\"id\":%d,\"player\":%d,\"type\":\"%s\","
									 "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"angle\":%.3f,"
									 "\"hp\":%.1f,\"maxhp\":%.1f,\"own\":%d,\"visible\":%d",
			(Int)obj->getID(),
			shownOwner ? shownOwner->getPlayerIndex() : -1,
			shownTmpl->getName().str(),
			pos->x, pos->y, pos->z,
			obj->getOrientation(),
			body ? body->getHealth() : 0.0f,
			body ? body->getMaxHealth() : 0.0f,
			isOwn ? 1 : 0,
			fogged ? 0 : 1);
		out += scratch.str();

		/*	How big it is on the ground -- for IMMOBILE things only.

		An agent siting a building has to know what it must clear, and the
		buildable-template list cannot tell it: that list holds what this
		player can BUILD, and the thing most often in the way is scenery --
		a supply dock, a civilian building, a tech structure -- which is in
		no one's build menu.

		The cost of not exporting it, measured: the bot sited its supply
		centre on a ring 70 units from the dock, which is inside the dock's
		own footprint, so every candidate came back objects_in_the_way. It
		never built a supply centre, and with no supply centre it could not
		meet the War Factory's prerequisite either -- 24 minutes, no
		economy, no vehicles, $25,700 banked and unspendable, from one
		unknown radius.

		Immobile only, because a moving unit's footprint is not an obstacle
		worth planning around (friendly units step aside when you build)
		and every byte here is multiplied by every object every snapshot.
	*/
		if (tmpl->isKindOf(KINDOF_IMMOBILE) || tmpl->isKindOf(KINDOF_STRUCTURE))
		{
			const GeometryInfo &g = tmpl->getTemplateGeometryInfo();
			scratch.format(",\"footprint\":[%.0f,%.0f]",
				g.getMajorRadius(), g.getMinorRadius());
			out += scratch.str();
		}

		// What it is, how far it sees and shoots (tooltip knowledge), and its
		// veterancy (the chevrons drawn over every unit). Ranges are live
		// rather than template values, so upgrades and bonuses are included.
		const Weapon *weapon = obj->getCurrentWeapon();
		// Speed is a tactical fact, not a cosmetic one: it decides who can
		// reach behind-the-lines artillery before it fires again, who can
		// disengage, and how fast a group can actually travel (the engine
		// itself matches a group to its slowest member).
		const AIUpdateInterface *aiUpd = obj->getAIUpdateInterface();
		const Real speed = (aiUpd != nullptr) ? aiUpd->getCurLocomotorSpeed() : 0.0f;
		scratch.format(",\"k\":%u,\"vision\":%.0f,\"range\":%.0f,\"speed\":%.1f",
			kindMask(shownTmpl), obj->getVisionRange(),
			(weapon && !shownDisguised) ? weapon->getAttackRange(obj) : 0.0f, speed);
		out += scratch.str();

		// WHAT A NEUTRAL BUILDING IS WORTH, from the engine rather than
		// from its name.
		//
		// KINDOF_TECH_BUILDING is one bit for the lot -- KindOf.h calls it
		// "Neutral tech building - Oil derrick, Hospital, Radio Station,
		// Refinery" -- so an agent told only "this is a tech building" has
		// no way to tell income from healing, and the only alternative was
		// matching on template names, which is a guess about what a map
		// calls things. The engine knows: a building that pays runs an
		// AutoDepositUpdate carrying the amount and the interval.
		{
			static const NameKeyType key_deposit = NAMEKEY("AutoDepositUpdate");
			const UpdateModule *depositMod = obj->findUpdateModule(key_deposit);
			if (depositMod != nullptr)
			{
				const AutoDepositUpdate *dep = (const AutoDepositUpdate *)depositMod;
				if (dep->friend_getDepositAmount() != 0)
				{
					scratch.format(",\"income\":%d,\"income_every\":%u,\"capture_bonus\":%d",
						dep->friend_getDepositAmount(),
						dep->friend_getDepositFrames(),
						dep->friend_getCaptureBonus());
					out += scratch.str();
				}
			}
		}

		// China's horde bonus is a real damage multiplier for massing units
		// together, so whether a unit currently HAS it is tactical state, not
		// trivia: it tells an agent that its infantry is worth materially
		// more standing in a block than strung out in a line.
		{
			static const NameKeyType key_horde = NAMEKEY("HordeUpdate");
			const UpdateModule *hordeMod = obj->findUpdateModule(key_horde);
			if (hordeMod != nullptr)
			{
				const HordeUpdateInterface *horde =
					((UpdateModule *)hordeMod)->getHordeUpdateInterface();
				if (horde != nullptr)
				{
					// Also publish what the bonus REQUIRES, so an agent can
					// form the block deliberately instead of discovering the
					// bonus by accident. ExactMatch means only identical
					// templates count -- five Red Guards, not five assorted
					// infantry.
					scratch.format(",\"horde\":%d,\"horde_need\":%d,\"horde_dist\":%.0f,\"horde_exact\":%d",
						horde->isInHorde() ? 1 : 0,
						horde->getHordeMinCount(),
						horde->getHordeMinDist(),
						horde->getHordeExactMatch() ? 1 : 0);
					out += scratch.str();
				}
			}
		}

		// ALWAYS EMITTED, even at regular. This used to be omitted for
		// LEVEL_REGULAR to save bytes, which made "veteran 0" and "the
		// exporter does not send this" indistinguishable to a consumer --
		// so an agent reading the stream could not tell a fresh unit from
		// an old engine, and defaulting the missing field to 0 is only
		// right by luck. Veterancy is not cosmetic: it scales damage and
		// health, and for a China hacker it scales INCOME directly
		// (ChinaInfantry.ini pays 5/6/8/10 per CashUpdateDelay by level, so
		// a heroic hacker earns exactly twice a regular one).
		scratch.format(",\"vet\":%d", (Int)obj->getVeterancyLevel());
		out += scratch.str();

		// Passengers in a transport or garrison are counted on its UI.
		// Which structure or transport this object is riding in, if any: the
		// bot had to infer "inside a bunker" from position, and got it wrong
		// for a whole day.
		if (obj->getContainedBy() != nullptr)
		{
			scratch.format(",\"in\":%d", (Int)obj->getContainedBy()->getID());
			out += scratch.str();
		}
		const ContainModuleInterface *contain = obj->getContain();
		// A GLA tunnel (TunnelContain, so the Sneak Attack tunnel too): every
		// tunnel of a player is one network of MaxTunnelCapacity (10) shared
		// slots, and a unit that goes in at one can leave from any other.
		// "contained" on a tunnel is therefore the whole network's count.
		if (contain != nullptr && contain->isTunnelContain())
			out += ",\"tunnel\":1";
		if (contain != nullptr && contain->getContainCount() > 0 && !hideContents)
		{
			scratch.format(",\"contained\":%u", contain->getContainCount());
			out += scratch.str();
		}

		// A supply pile visibly shrinks as it is gathered; the box count is
		// what the pile model is drawn from. An agent uses it to know when a
		// dock is running dry and it is time to expand.
		if (tmpl->isKindOf(KINDOF_SUPPLY_SOURCE))
		{
			static const NameKeyType key_warehouseUpdate = NAMEKEY("SupplyWarehouseDockUpdate");
			const SupplyWarehouseDockUpdate *warehouse =
				(const SupplyWarehouseDockUpdate *)obj->findUpdateModule(key_warehouseUpdate);
			if (warehouse != nullptr)
			{
				scratch.format(",\"boxes\":%d", warehouse->getBoxesStored());
				out += scratch.str();
			}
		}

		// Behaviour an observer can read off the screen: what the unit is doing
		// and, for our own units, where it has been told to go.
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if (ai != nullptr)
		{
			scratch.format(",\"idle\":%d,\"moving\":%d,\"attacking\":%d",
				ai->isIdle() ? 1 : 0,
				ai->isMoving() ? 1 : 0,
				ai->isAttacking() ? 1 : 0);
			out += scratch.str();

			if (isOwn || observing == nullptr)
			{
				const Coord3D *goal = ai->getGoalPosition();
				if (goal != nullptr && ai->isMoving())
				{
					scratch.format(",\"goal_x\":%.1f,\"goal_y\":%.1f", goal->x, goal->y);
					out += scratch.str();
				}
			}
		}

		// Recent damage on my own things: the health bar dropping, and the
		// source if the engine knows it. Only enemy damage counts; healing
		// and self-inflicted effects are not an attack.
		if (isOwn && body != nullptr)
		{
			const UnsignedInt hitAt = body->getLastDamageTimestamp();
			const DamageInfo *dinfo = body->getLastDamageInfo();
			if (hitAt != 0 && nowFrame >= hitAt && nowFrame - hitAt <= 150 &&
					dinfo != nullptr && dinfo->in.m_damageType != DAMAGE_HEALING &&
					dinfo->in.m_sourceID != obj->getID())
			{
				scratch.format(",\"hit_frame\":%u,\"hit_by\":%u,\"hit_dmg\":%.0f",
					hitAt, (UnsignedInt)dinfo->in.m_sourceID, dinfo->out.m_actualDamageDealt);
				out += scratch.str();
				if (tmpl->isKindOf(KINDOF_STRUCTURE))
				{
					char alert[160];
					snprintf(alert, sizeof(alert),
						"%s{\"id\":%d,\"type\":\"%.48s\",\"x\":%.0f,\"y\":%.0f,\"by\":%u,\"frame\":%u}",
						alerts.empty() ? "" : ",", (Int)obj->getID(), tmpl->getName().str(),
						pos->x, pos->y, (UnsignedInt)dinfo->in.m_sourceID, hitAt);
					alerts += alert;
				}
			}
		}

		// A stealthed unit we can see is worth flagging: it is only visible
		// because something of ours is detecting it, and that can lapse.
		if (obj->testStatus(OBJECT_STATUS_STEALTHED) && !shownDisguised)
			out += ",\"stealthed\":1";
		// Our own (or an ally's) stealthed unit that an enemy has found: the
		// owner sees it drawn STEALTHLOOK_VISIBLE_FRIENDLY_DETECTED, the
		// detection overlay "as a warning", and gets a "stealth neutralized"
		// radar event -- whatever did the detecting, a Spy Satellite scan
		// included (StealthDetectorUpdate.cpp). For an enemy the flag is
		// implied: an undetected one is not reported at all.
		if ((isOwn || allied || observing == nullptr) &&
				obj->testStatus(OBJECT_STATUS_STEALTHED) && obj->testStatus(OBJECT_STATUS_DETECTED))
			out += ",\"detected\":1";
		// A disguised enemy is an ordinary vehicle to us; our own disguised
		// truck says what it is pretending to be.
		if ((isOwn || allied) && obj->testStatus(OBJECT_STATUS_DISGUISED))
		{
			StealthUpdate *st = obj->getStealth();
			if (st != nullptr && st->getDisguisedTemplate() != nullptr)
			{
				scratch.format(",\"disguise\":\"%s\"", st->getDisguisedTemplate()->getName().str());
				out += scratch.str();
			}
		}

		// A structure still going up is visibly scaffolded, so this is not
		// privileged information. It matters because hit points alone cannot
		// distinguish a half-built structure from a finished one taking fire,
		// and an agent that confuses the two will stall its build order every
		// time its base is attacked.
		if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			out += ",\"constructing\":1";

		// What our own builders are busy with. A dozer's job is not deducible
		// from position or the idle flag -- it reads as idle while walking to a
		// site it has already been given -- so an agent cannot otherwise tell a
		// free builder from a committed one, and will keep retasking the same
		// dozer and cancelling its own construction.
		if (isOwn || observing == nullptr)
		{
			const AIUpdateInterface *ai = obj->getAIUpdateInterface();
			const DozerAIInterface *dozer = ai ? ai->getDozerAIInterface() : nullptr;
			if (dozer != nullptr)
			{
				out += ",\"builder\":1";
				scratch.format(",\"dozer_task\":%d",
					(Int)(const_cast<DozerAIInterface *>(dozer)->isAnyTaskPending()
						? dozer->getCurrentTask() : (Int)DOZER_TASK_INVALID));
				out += scratch.str();
			}
		}

		// Production queues are readable off the building's own UI, so they are
		// reported for our own structures only.
		if (isOwn || observing == nullptr)
		{
			ProductionUpdateInterface *pu = obj->getProductionUpdateInterface();
			if (pu != nullptr && pu->getProductionCount() > 0)
			{
				out += ",\"queue\":[";
				Bool firstEntry = TRUE;
				for (const ProductionEntry *e = pu->firstProduction(); e; e = pu->nextProduction(e))
				{
					const ThingTemplate *what = e->getProductionObject();
					if (what == nullptr)
						continue;
					if (!firstEntry)
						out += ',';
					firstEntry = FALSE;
					scratch.format("{\"type\":\"%s\",\"percent\":%.3f,\"remaining\":%d}",
						what->getName().str(),
						e->getPercentComplete(),
						e->getProductionQuantityRemaining());
					out += scratch.str();
				}
				out += ']';
			}
		}

		// Our own units' state that their own buttons show: which per-object
		// upgrades they carry, and when each of their abilities recharges.
		//
		// "ups": OBJECT-type upgrades complete on THIS object -- an Overlord's
		// Gattling Cannon, a Humvee's drone. The player-level upgrade list
		// cannot say which of our twelve Overlords already has its add-on.
		//
		// "sp": this object's own special powers and the frame each is ready.
		// The player "powers" list reports one source per power TYPE, so ten
		// Tank Hunters' TNT or a Black Lotus's hacks were one shared clock.
		//
		// Both are const reads (hasUpgrade tests a bitmask; getReadyFrame
		// returns a stored frame) -- nothing here may touch
		// GameLogicRandomValue. Both only change when the state does, so they
		// cost nothing in a delta frame.
		if (salvageCrate)
			out += ",\"crate\":\"salvage\"";

		if (isOwn)
		{
			Bool firstUp = TRUE;
			for (const UpgradeTemplate *ut = TheUpgradeCenter ? TheUpgradeCenter->firstUpgradeTemplate() : nullptr;
					 ut != nullptr; ut = ut->friend_getNext())
			{
				if (ut->getUpgradeType() != UPGRADE_TYPE_OBJECT || !obj->hasUpgrade(ut))
					continue;
				out += firstUp ? ",\"ups\":[\"" : ",\"";
				firstUp = FALSE;
				out += ut->getUpgradeName().str();
				out += '"';
			}
			if (!firstUp)
				out += ']';

			// "junk": how many salvage weapon tiers this vehicle has picked
			// up (0-2). A crate gives the next tier until both are taken,
			// then only a chance of veterancy or $25-75 -- so the crate is
			// worth most to the vehicle with the fewest.
			if (obj->isKindOf(KINDOF_WEAPON_SALVAGER))
			{
				const Int junk = obj->testWeaponSetFlag(WEAPONSET_CRATEUPGRADE_TWO) ? 2
					: obj->testWeaponSetFlag(WEAPONSET_CRATEUPGRADE_ONE) ? 1 : 0;
				scratch.format(",\"junk\":%d", junk);
				out += scratch.str();
			}

			// "slave": bound to a master (SlavedUpdate with a slaver): a Stinger
			// Site's soldiers, a tunnel's two RPG troopers, an Angry Mob's
			// members. They stay with their master -- the soldiers ignore
			// orders outright, the tunnel's troopers are leashed to it, the mob
			// moves with its Nexus -- so the bot must not count them as an
			// army it can send anywhere.
			{
				for (BehaviorModule **m = obj->getBehaviorModules(); m != nullptr && *m != nullptr; ++m)
				{
					SlavedUpdateInterface *sl = (*m)->getSlavedUpdateInterface();
					if (sl != nullptr && sl->getSlaverID() != INVALID_ID)
					{
						out += ",\"slave\":1";
						break;
					}
				}
			}

			// "cs": this object's command set, ONLY when an upgrade has swapped
			// it from its template's (a GLA Worker toggled to fake buildings).
			// A bot that caches buttons per TYPE otherwise believes every Worker
			// offers what one does, and hands a real build to a Worker that can
			// only place decoys.
			if (obj->getCommandSetString() != obj->getTemplate()->friend_getCommandSetString())
			{
				scratch.format(",\"cs\":\"%s\"", obj->getCommandSetString().str());
				out += scratch.str();
			}

			Bool firstSp = TRUE;
			for (BehaviorModule **m = obj->getBehaviorModules(); m != nullptr && *m != nullptr; ++m)
			{
				SpecialPowerModuleInterface *sp = (*m)->getSpecialPower();
				if (sp == nullptr)
					continue;
				const SpecialPowerTemplate *spt = sp->getSpecialPowerTemplate();
				if (spt == nullptr)
					continue;
				scratch.format("%s{\"n\":\"%s\",\"r\":%u}", firstSp ? ",\"sp\":[" : ",",
					spt->getName().str(), (unsigned)sp->getReadyFrame());
				firstSp = FALSE;
				out += scratch.str();
			}
			if (!firstSp)
				out += ']';
		}

		out += '}';

		// Emit the object: whole on a keyframe or when new, otherwise only
		// the fields that changed. deltaObject() returns FALSE when nothing
		// changed at all, and the object is then left out of the message
		// entirely -- which is where nearly all of the 7.2x saving comes
		// from, since most objects are byte-identical frame to frame.
		{
			const ObjectID oid = obj->getID();
			if (m_delta)
				m_seenThisFrame.insert(oid);

			// Cut this object's JSON back out, minus any leading comma.
			size_t bodyAt = objStart;
			if (bodyAt < out.size() && out[bodyAt] == ',')
				++bodyAt;
			const std::string objBuf = out.substr(bodyAt);

			if (m_delta && m_sinceKeyframe != 0)
			{
				std::map<ObjectID, std::string>::iterator prev =
					m_prevObjects.find(oid);
				if (prev != m_prevObjects.end())
				{
					std::string emit;
					const Bool changed = deltaObject(objBuf, emit, prev->second);
					prev->second = objBuf;
					// Replace the full text with the diff, or drop the
					// object entirely when nothing changed. This is where
					// nearly all of the saving comes from: most objects are
					// byte-identical frame to frame.
					// Rewind to before this object (comma included) and
					// re-emit only if it changed. `first` is restored to
					// what it was on entry, so the comma bookkeeping stays
					// correct whether or not the object is dropped.
					const Bool wasFirst = (objStart == 0) ||
						(out[objStart] != ',');
					out.erase(objStart);
					if (changed)
					{
						if (!wasFirst)
							out += ',';
						out += emit;
						first = FALSE;
					}
					else
					{
						first = wasFirst;
					}
				}
				else
				{
					m_prevObjects[oid] = objBuf;	// first sight: sent whole
				}
			}
			else if (m_delta)
			{
				m_prevObjects[oid] = objBuf;		// keyframe
			}
		}

	}

	out += "]";

	/*	DELETION MUST BE EXPLICIT.

		Under deltas "absent" means UNCHANGED, so an object that is simply
		left out of a message is taken to be alive and unchanged. A unit
		that died would therefore live for ever in the reader's world. Name
		them.
	*/
	if (m_delta && !keyframe)
	{
		std::string gone;
		std::map<ObjectID, std::string>::iterator it = m_prevObjects.begin();
		while (it != m_prevObjects.end())
		{
			if (m_seenThisFrame.find(it->first) == m_seenThisFrame.end())
			{
				AsciiString idTxt;
				idTxt.format("%s%u", gone.empty() ? "" : ",", (UnsignedInt)it->first);
				gone += idTxt.str();
				std::map<ObjectID, std::string>::iterator dead = it++;
				m_prevObjects.erase(dead);
			}
			else
			{
				++it;
			}
		}
		if (!gone.empty())
		{
			out += ",\"gone\":[";
			out += gone;
			out += "]";
		}
	}

	out += ",\"under_attack\":[";
	if (!(obsMask() & 8))
		out += alerts;
	out += "]}\n";
}

// ------------------------------------------------------------------------------------------------
/**
 * Bind to the local player once the game has one.
 *
 * Called every frame while unresolved: ThePlayerList does not exist yet when
 * init() runs, and in a network game the local slot is not known until the
 * game starts.
 */
void ObservationServer::resolvePlayerIndex()
{
	if (!m_bindToLocalPlayer || ThePlayerList == nullptr)
		return;

	const Player *local = ThePlayerList->getLocalPlayer();
	if (local == nullptr)
		return;

	/*	Do not latch onto player 0.

		This runs every frame from update(), and at frame 0 of a NETWORK
		game the local player is not assigned yet -- getLocalPlayer()
		returns the neutral player at index 0. Latching there bound both
		engines of a LAN match to the same empty slot: identical
		observations, $0, no units, and both bots convinced they shared a
		base. Index 0 is never a playable slot (it is the neutral/civilian
		player), so treat it as "not resolved yet" and keep looking.
	*/
	const Int idx = local->getPlayerIndex();
	if (idx <= 0)
		return;

	m_playerIndex = idx;
	m_bindToLocalPlayer = FALSE;
	DEBUG_LOG(("ObservationServer: observing the local player, slot %d",
		m_playerIndex));
}

//-------------------------------------------------------------------------------------------------
/**
	Escape a string for embedding in JSON.

	Every other string this server emits is an engine-controlled
	identifier -- a template name, a waypoint, a science -- so none of
	them has ever needed escaping. Chat is the FIRST player-authored text
	to cross this boundary, and a single double-quote or backslash in a
	chat line would otherwise produce malformed JSON and break the
	agent's parser mid-match. Control characters are escaped for the same
	reason.

	Bytes >= 0x80 are passed through untouched: the source is UTF-8 and
	JSON accepts it directly.
*/
static std::string jsonEscape( const char *in )
{
	std::string out;
	if (in == nullptr)
		return out;
	for (const unsigned char *p = (const unsigned char *)in; *p; ++p)
	{
		switch (*p)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (*p < 0x20)
				{
					char esc[8];
					snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
					out += esc;
				}
				else
				{
					out += (char)*p;
				}
				break;
		}
	}
	return out;
}

//-------------------------------------------------------------------------------------------------
void ObservationServer::recordChat( Int senderSlot, const UnicodeString &text, Int playerMask )
{
	if (!m_enabled)
		return;

	/*	Bound the buffer.

		Nothing drains this until an observation is built, and an
		observation is only built while a client is connected and in a
		running match. Chat arriving outside that window -- or faster
		than the frame interval -- would otherwise accumulate forever.
		Dropping the OLDEST keeps the most recent orders, which is what
		matters for a command channel.
	*/
	if (m_chat.size() >= (size_t)MAX_CHAT_BUFFERED)
		m_chat.erase(m_chat.begin());

	ChatLine line;
	line.senderSlot = senderSlot;
	line.playerMask = playerMask;
	line.frame = (TheGameLogic != nullptr) ? TheGameLogic->getFrame() : 0;

	// UnicodeString is UTF-16; translate to the UTF-8 the stream carries.
	AsciiString utf8;
	utf8.translate(text);
	line.text = jsonEscape(utf8.str());

	m_chat.push_back(line);
}

//-------------------------------------------------------------------------------------------------
void ObservationServer::update()
{
	if (!m_enabled)
		return;

	// Only a running match has meaningful state. The shell menu and the
	// loading phase are skipped so nothing is serialized before the player
	// list and object list exist.
	if (TheGameLogic == nullptr || !TheGameLogic->isInGame() ||
			TheGameLogic->isInShellGame() || ThePlayerList == nullptr)
		return;

	resolvePlayerIndex();

	if (m_clientSocket == INVALID_SOCKET)
	{
		acceptClient();
		if (m_clientSocket == INVALID_SOCKET)
			return;	// no agent listening, so nothing to serialize
	}

	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame != 0 && (frame - m_lastSentFrame) < m_frameInterval)
		return;
	m_lastSentFrame = frame;

	// An agent needs the static map before the first frame makes sense.
	//
	// TheSuperHackers note: headless runs still use W3DGameLogic and
	// W3DTerrainLogic (Win32GameEngine is the only engine factory), so the
	// terrain queries work without a renderer as long as the logical height
	// map has been loaded; see W3DTerrainVisual::load.
	if (!m_sentMap)
	{
		// isInGame() turns true a frame or two before the map and its objects
		// are loaded, and an agent that connects in that window would get a
		// map built from a zero pathfinder extent: every cell impassable, no
		// objects. The pathfinder grid is sized when the map loads, so wait
		// for it rather than describe a world that does not exist yet.
		if (TheAI == nullptr || TheAI->pathfinder() == nullptr)
			return;
		const ICoord2D *pfHi = TheAI->pathfinder()->getExtent();
		if (pfHi == nullptr || pfHi->x <= 0 || pfHi->y <= 0)
			return;
		if (TheGameLogic->getFirstObject() == nullptr)
			return;

		buildMapDescription();
		m_sentMap = TRUE;
		if (m_clientSocket == INVALID_SOCKET)
			return;	// the send dropped the client
	}

	std::string observation;
	buildObservation(observation);
	sendRaw(observation);
}
