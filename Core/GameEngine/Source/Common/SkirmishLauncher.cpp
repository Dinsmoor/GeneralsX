/*
**	Headless skirmish launcher. See SkirmishLauncher.h for the rationale.
**
**	The engine can already play a replay headlessly, but it had no way to start
**	a *new* match without the shell UI, which is what an agent needs in order to
**	play or train. This builds the same GameInfo the skirmish menu would build,
**	then sends the same MSG_NEW_GAME, and pumps GameLogic exactly the way
**	ReplaySimulation does.
*/

#include "PreRTS.h"

#include "Common/SkirmishLauncher.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/MessageStream.h"
#include "Common/Recorder.h"
#include "Common/RandomValue.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"
#include "GameClient/ControlBar.h"

#include <string>

namespace
{

/**
	Build the GameInfo option string for the requested match.

	Format matches GameInfoToAsciiString: map, its CRC and size, seed, then a
	slot list. Each slot is either "H<name>,<ip>,<port>,<accept>,<color>,
	<template>,<startpos>,<team>,<natbehavior>", a "C<...>" computer slot, or
	"X" for closed.
*/
static AsciiString buildGameInfoString( const MapMetaData *md,
																				const AsciiString& mapDir,
																				const AsciiString& slotSpec,
																				Int startingCash,
																				Int seed )
{
	std::string s;
	char chunk[512];

	// The map value carries a two hex digit contents mask, then just the map's
	// directory; the parser rebuilds "<dir>\<dir>.map" from it. This mirrors
	// what a real replay header stores, e.g. "03maps/alpine assault".
	snprintf(chunk, sizeof(chunk), "US=1;M=03%s;MC=%8.8X;MS=%d;SD=%d;C=100;SR=0;SC=%d;O=N;S=",
		mapDir.str(), md->m_CRC, md->m_filesize, seed, startingCash);
	s += chunk;

	Int slotIndex = 0;
	const char *p = slotSpec.str();
	while (*p != '\0' && slotIndex < MAX_SLOTS)
	{
		char word[64];
		Int w = 0;
		while (*p != '\0' && *p != ',' && w < (Int)sizeof(word) - 1)
			word[w++] = *p++;
		word[w] = '\0';
		if (*p == ',')
			++p;

		if (slotIndex > 0)
			s += ':';

		// A slot may name a faction after a colon, e.g. "human:2", to pin the
		// player template instead of taking a random general. -1 stays random.
		Int playerTemplate = -1;
		char *colon = strchr(word, ':');
		if (colon != nullptr)
		{
			*colon = '\0';
			playerTemplate = atoi(colon + 1);
		}

		if (strcmp(word, "human") == 0)
		{
			// H<name>,<ip>,<port>,<accept+map>,<color>,<template>,<startpos>,
			// <team>,<nat>. IP 0 marks a local player, which is what a solo
			// skirmish from the menu produces. Template -1 is random faction.
			snprintf(chunk, sizeof(chunk), "Hbot,0,0,TT,%d,%d,%d,-1,1",
				slotIndex, playerTemplate, slotIndex);
		}
		else if (strcmp(word, "x") == 0 || strcmp(word, "closed") == 0)
		{
			snprintf(chunk, sizeof(chunk), "X");
		}
		else if (strcmp(word, "open") == 0)
		{
			snprintf(chunk, sizeof(chunk), "O");
		}
		else
		{
			// C<E|M|B>,<color>,<template>,<startpos>,<team>
			char diff = 'M';
			if (strcmp(word, "easy") == 0)
				diff = 'E';
			else if (strcmp(word, "brutal") == 0 || strcmp(word, "hard") == 0)
				diff = 'B';
			snprintf(chunk, sizeof(chunk), "C%c,%d,%d,%d,-1",
				diff, slotIndex, playerTemplate, slotIndex);
		}
		s += chunk;
		++slotIndex;
	}

	while (slotIndex < MAX_SLOTS)
	{
		s += ":X";
		++slotIndex;
	}
	s += ';';

	AsciiString out;
	out.set(s.c_str());
	return out;
}

} // anonymous namespace


//-------------------------------------------------------------------------------------------------
Int SkirmishLauncher::runSkirmish()
{
	if (TheMapCache == nullptr)
	{
		printf("Skirmish: no map cache\n");
		return 1;
	}

	AsciiString mapName = TheGlobalData->m_skirmishMap;
	if (mapName.isEmpty())
		mapName = TheGlobalData->m_mapName;
	if (mapName.isEmpty())
	{
		printf("Skirmish: no map given, use -skirmishmap \"Alpine Assault\"\n");
		return 1;
	}

	TheMapCache->updateCache();

	// The cache is keyed by the full lower case path, so a plain name like
	// "Alpine Assault" is expanded to maps/alpine assault/alpine assault.map.
	AsciiString lower = mapName;
	lower.toLower();
	const MapMetaData *md = TheMapCache->findMap(lower);

	// Cache keys are full paths, and differ between the user maps directory and
	// the ones inside the .big archives, so an exact key cannot be constructed
	// from a plain name. Match on the "<name>/<name>.map" tail instead, which is
	// unique and works for both.
	if (md == nullptr)
	{
		AsciiString tail;
		tail.format("%s\\%s.map", lower.str(), lower.str());
		AsciiString tailFwd;
		tailFwd.format("%s/%s.map", lower.str(), lower.str());

		for (MapCache::const_iterator it = TheMapCache->begin();
				 it != TheMapCache->end(); ++it)
		{
			if (it->first.endsWithNoCase(tail.str()) ||
					it->first.endsWithNoCase(tailFwd.str()))
			{
				lower = it->first;
				md = &it->second;
				break;
			}
		}
	}
	if (md == nullptr)
	{
		printf("Skirmish: could not find map \"%s\"\n", mapName.str());
		printf("Skirmish: %d maps in the cache, first few:\n", (Int)TheMapCache->size());
		Int shown = 0;
		for (MapCache::const_iterator it = TheMapCache->begin();
				 it != TheMapCache->end() && shown < 8; ++it, ++shown)
		{
			printf("   %s\n", it->first.str());
		}
		return 1;
	}

	// Without -seed every run would otherwise be the identical match, which is
	// useless for collecting varied training games. The seed is printed in the
	// option string below (SD=) and stored in the replay header, so any run can
	// still be repeated exactly by passing it back with -seed.
	const Int seed = TheGlobalData->m_fixedSeed >= 0
		? TheGlobalData->m_fixedSeed
		: (Int)(GetTickCount() & 0x7fffffff);

	// The GameInfo string wants "maps/<name>", not the absolute cache key, so
	// rebuild it from the plain name the caller gave.
	AsciiString shortName = mapName;
	shortName.toLower();
	AsciiString mapDir;
	mapDir.format("maps/%s", shortName.str());

	AsciiString options = buildGameInfoString(md, mapDir,
		TheGlobalData->m_skirmishSlots,
		(Int)TheGlobalData->m_defaultStartingCash.countMoney(), seed);

	printf("Skirmish: %s\n", options.str());
	fflush(stdout);

	// TheControlBar normally comes up with the in game UI, which a headless run
	// never creates. BuildAssistant::isPossibleToMakeUnit() consults it to check
	// that a builder's command set actually offers what is being built, so
	// without it EVERY dozer construction is silently refused. It is only the
	// command button and command set tables that matter here, and those come
	// from INI, so creating it headless is safe and does not touch the renderer.
	if (TheControlBar == nullptr)
	{
		TheControlBar = NEW ControlBar;
		TheControlBar->initCommandDataOnly();
	}

	if (TheSkirmishGameInfo == nullptr)
		TheSkirmishGameInfo = NEW SkirmishGameInfo;

	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->enterGame();

	if (!ParseAsciiStringToGameInfo(TheSkirmishGameInfo, options))
	{
		printf("Skirmish: could not build a valid game setup\n");
		return 1;
	}

	TheSkirmishGameInfo->setLocalIP(0);
	TheWritableGlobalData->m_pendingFile = TheSkirmishGameInfo->getMap();
	TheSkirmishGameInfo->startGame(0);

	InitRandom(seed);

	// Same message the skirmish menu sends, straight onto the command list
	// because TheMessageStream is not pumped during a headless run.
	GameMessage *msg = newInstance(GameMessage)(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_SKIRMISH);
	msg->appendIntegerArgument(DIFFICULTY_NORMAL);
	msg->appendIntegerArgument(0);
	msg->appendIntegerArgument(TheGlobalData->m_framesPerSecondLimit);
	TheCommandList->appendMessage(msg);

	// Pump the logic. The observation and action servers are driven from
	// GameLogic::update(), so an agent sees every frame and can act on it.
	const UnsignedInt limit = TheGlobalData->m_skirmishMaxFrames;
	DWORD startMillis = GetTickCount();

	while (!TheGameEngine->getQuitting())
	{
		TheGameLogic->UPDATE();

		const UnsignedInt frame = TheGameLogic->getFrame();

		if (limit != 0 && frame >= limit)
		{
			printf("Skirmish: reached the frame limit of %d\n", limit);
			break;
		}

		// A finished match leaves the game, which is the natural end.
		if (frame > 1 && !TheGameLogic->isInGame())
		{
			printf("Skirmish: game over\n");
			break;
		}

		const Int reportEvery = 5 * 60 * LOGICFRAMES_PER_SECOND;
		if (frame != 0 && frame % reportEvery == 0)
		{
			UnsignedInt gameSec = frame / LOGICFRAMES_PER_SECOND;
			UnsignedInt realSec = (GetTickCount() - startMillis) / 1000;
			printf("Elapsed Time: %02d:%02d Game Time: %02d:%02d\n",
				realSec / 60, realSec % 60, gameSec / 60, gameSec % 60);
			fflush(stdout);
		}
	}

	// Close the replay file. Recording starts automatically off MSG_NEW_GAME,
	// but a headless run breaks out of the loop on a frame limit rather than
	// leaving the game the way the UI does, so nothing would ever call this and
	// the replay would be left open and unwritten.
	if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
	{
		TheRecorder->stopRecording();
		printf("Skirmish: replay written to %s%s%s\n",
			RecorderClass::getReplayDir().str(),
			RecorderClass::getLastReplayFileName().str(),
			RecorderClass::getReplayExtention().str());
	}

	UnsignedInt gameSec = TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND;
	UnsignedInt realSec = (GetTickCount() - startMillis) / 1000;
	printf("Skirmish finished. Elapsed Time: %02d:%02d Game Time: %02d:%02d\n",
		realSec / 60, realSec % 60, gameSec / 60, gameSec % 60);
	fflush(stdout);

	return 0;
}
