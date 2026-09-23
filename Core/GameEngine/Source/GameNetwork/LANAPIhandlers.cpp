/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LANAPIHandlers.cpp
// Author: Matthew D. Campbell, October 2001
// Description: LAN callback handlers
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "Common/GlobalData.h"
#include "Common/QuotedPrintable.h"
#include "Common/UserPreferences.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/MapUtil.h"

// GeneralsX @build BenderAI 13/02/2026 WideCharWindows conversion helpers (fighter19 pattern)
// These functions handle conversion between WideChar (wchar_t) and WideCharWindows (uint16_t)
// Required because wchar_t size varies (2 bytes Windows, 4 bytes Linux) but network protocol needs fixed size

void CopyWcharToWindowsWideChar( WideCharWindows *dest, const WideChar *src, UnsignedInt len )
{
	for (UnsignedInt i = 0; i < len; ++i)
	{
		dest[i] = src[i];
	}
	dest[len] = 0;
}

wchar_t *GetWindowsWideCharAsWchar( WideCharWindows *src )
{
	static wchar_t buf[MAX_COMPUTERNAME_LENGTH];
	// Get the length of the string
	UnsignedInt len = 0;
	while (src[len] != 0)
	{
		++len;
	}

	if (len > MAX_COMPUTERNAME_LENGTH)
	{
		return NULL; // too long
	}

	// Copy the string
	for (UnsignedInt i = 0; i < len; ++i)
	{
		buf[i] = src[i];
	}
	buf[len] = 0;
	return buf;
}

void LANAPI::handleRequestLocations( LANMessage *msg, UnsignedInt senderIP )
{
	/* 	fprintf(stderr, "[LAN86] handleRequestLocations sender=%d.%d.%d.%d inLobby=%d currentGame=%d\n",
		PRINTF_IP_AS_4_INTS(senderIP), m_inLobby, (m_currentGame != nullptr)); */
	if (m_inLobby)
	{
		LANMessage reply;
		fillInLANMessage( &reply );
		reply.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;

		sendMessage(&reply);
		m_lastResendTime = timeGetTime();
	}
	else
	{
		// In game - are we a game host?
		if (m_currentGame)
		{
			// TheSuperHackers @bugfix AmIHost() compares (address, port); an inline
			// address-only test made every co-located instance answer as the host.
			if (AmIHost())
			{
				AsciiString gameOpts = GenerateGameOptionsString();
				if (!gameOpts.isEmpty())
				{
					LANMessage reply;
					fillInLANMessage( &reply );
					reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
					strlcpy(reply.GameInfo.options, gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
					// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
					CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
					reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
					reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

					sendMessage(&reply);
				}
			}
			else
			{
				// We're a joiner
			}
		}
	}
	// Add the player to the lobby player list
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
	player->setName(UnicodeString(GetWindowsWideCharAsWchar(msg->name)));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);
	/* 	fprintf(stderr, "[LAN86] handleRequestLocations player=%d.%d.%d.%d name=%ls host=%s login=%s\n",
		PRINTF_IP_AS_4_INTS(player->getIP()), player->getName().str(), player->getHost().str(), player->getLogin().str()); */

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	/* 	fprintf(stderr, "[LAN86] handleGameAnnounce sender=%d.%d.%d.%d game=%ls inProgress=%d direct=%d\n",
		PRINTF_IP_AS_4_INTS(senderIP), GetWindowsWideCharAsWchar(msg->GameInfo.gameName),
		msg->GameInfo.inProgress, msg->GameInfo.isDirectConnect); */
	/*	TheSuperHackers @bugfix was `senderIP == m_localIP`, which is upstream of
		every other co-located-instance problem: a second engine on this machine
		announces from our own address (the kernel stamps it -- see isFromSelf),
		so we threw its announce away and NEVER DISCOVERED ITS GAME AT ALL. No
		discovery, no join, however well everything downstream works.

		isFromSelf also compares the port, so our own echo is still ignored. */
	if (isFromSelf(senderIP))
	{
		return; // Don't try to update own info
	}
	else if (m_currentGame && m_currentGame->isGameInProgress())
	{
		return; // Don't care about games if we're playing
	}
	else if (senderIP == m_directConnectRemoteIP)
	{
		if (m_currentGame == nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			LANGameInfo *game = LookupGame(UnicodeString(GetWindowsWideCharAsWchar(msg->GameInfo.gameName)));
			if (!game)
			{
				game = NEW LANGameInfo;
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
				game->setName(UnicodeString(GetWindowsWideCharAsWchar(msg->GameInfo.gameName)));
				addGame(game);
			}
			Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
			game->setGameInProgress(msg->GameInfo.inProgress);
			game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
			game->setLastHeard(timeGetTime());
			if (!success)
			{
				// remove from list
				removeGame(game);
				delete game;
				return;
			}
			RequestGameJoin(game, m_directConnectRemoteIP);
		}
	}
	else
	{
		// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
		LANGameInfo *game = LookupGame(UnicodeString(GetWindowsWideCharAsWchar(msg->GameInfo.gameName)));
		if (!game)
		{
			game = NEW LANGameInfo;
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			game->setName(UnicodeString(GetWindowsWideCharAsWchar(msg->GameInfo.gameName)));
			addGame(game);
		}
		Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
		game->setGameInProgress(msg->GameInfo.inProgress);
		game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
		game->setLastHeard(timeGetTime());
		if (!success)
		{
			// remove from list
			removeGame(game);
			delete game;
			game = nullptr;
			/* 			fprintf(stderr, "[LAN86] handleGameAnnounce parse failed sender=%d.%d.%d.%d\n",
				PRINTF_IP_AS_4_INTS(senderIP)); */
		}
		else if (game != nullptr)
		{
			/* 			fprintf(stderr, "[LAN86] handleGameAnnounce accepted game host=%d.%d.%d.%d name=%ls\n",
				PRINTF_IP_AS_4_INTS(senderIP), game->getName().str()); */
		}

		OnGameList( m_games );
	//	if (game == m_currentGame && !m_inLobby)
	//		OnSlotList(RET_OK, game);
	}
}

void LANAPI::handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	/* 	fprintf(stderr, "[LAN86] handleLobbyAnnounce sender=%d.%d.%d.%d name=%ls host=%s login=%s\n",
		PRINTF_IP_AS_4_INTS(senderIP), GetWindowsWideCharAsWchar(msg->name), msg->hostName, msg->userName); */
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
	player->setName(UnicodeString(GetWindowsWideCharAsWchar(msg->name)));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);
	/* 	fprintf(stderr, "[LAN86] handleLobbyAnnounce updated player=%d.%d.%d.%d name=%ls\n",
		PRINTF_IP_AS_4_INTS(player->getIP()), player->getName().str()); */

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP )
{
	/* 	fprintf(stderr, "[LAN86] handleRequestGameInfo sender=%d.%d.%d.%d requesterIP=%d.%d.%d.%d requesterName=%ls\n",
		PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(msg->PlayerInfo.ip), GetWindowsWideCharAsWchar(msg->PlayerInfo.playerName)); */
	// In game - are we a game host?
	if (m_currentGame)
	{
		if (AmIHost() || (m_currentGame->isGameInProgress() && TheNetwork && TheNetwork->isPacketRouter())) // if we're in game we should reply if we're the packet router
		{
			AsciiString gameOpts = GameInfoToAsciiString(m_currentGame);
			if (gameOpts.isEmpty())
			{
				return;
			}

			LANMessage reply;
			fillInLANMessage( &reply );
			reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;

			strlcpy(reply.GameInfo.options,gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
			// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
			CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
			reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
			reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

			replyToSender(&reply, senderIP);
		}
	}
}

static Bool IsInvalidCharForPlayerName(const WideChar c)
{
	return c < L' ' // C0 control chars
		|| c == L',' || c == L':' || c == L';' // chars used for strtok in ParseAsciiStringToGameInfo
		|| (c >= L'\x007f' && c <= L'\x009f') // DEL + C1 control chars
		|| c == L'\x2028' || c == L'\x2029' // line and paragraph separators
		|| (c >= L'\xdc00' && c <= L'\xdfff') // low surrogate, for chars beyond the Unicode Basic Multilingual Plane
		|| (c >= L'\xd800' && c <= L'\xdbff'); // high surrogate, for chars beyond the BMP
}

static Bool IsSpaceCharacter(const WideChar c)
{
	return c == L' ' // space
		|| c == L'\xA0' // no-break space
		|| c == L'\x1680' // ogham space mark
		|| (c >= L'\x2000' && c <= L'\x200A') // en/em spaces, figure, punctuation, thin, hair
		|| c == L'\x202F' // narrow no-break space
		|| c == L'\x205F' // medium mathematical space
		|| c == L'\x3000'; // ideographic space
}

static Bool ContainsInvalidChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (IsInvalidCharForPlayerName(*playerName++))
			return true;
	}

	return false;
}

static Bool ContainsAnyReadableChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (!IsSpaceCharacter(*playerName++))
			return true;
	}

	return false;
}

void LANAPI::handleRequestJoin( LANMessage *msg, UnsignedInt senderIP )
{
	UnsignedInt responseIP = senderIP;	// need this cause the player may or may not be
																			// in the player list at the sendMessage.
	// GeneralsX @bugfix GitHubCopilot 12/04/2026 Keep join responses unicast so the joining client does not depend on LAN broadcast delivery.
	/* 	fprintf(stderr, "[LAN86] handleRequestJoin sender=%d.%d.%d.%d targetGameIP=%d.%d.%d.%d localIP=%d.%d.%d.%d pending=%d inLobby=%d\n",
		PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(msg->GameToJoin.gameIP), PRINTF_IP_AS_4_INTS(m_localIP),
		m_pendingAction, m_inLobby); */

	if (msg->GameToJoin.gameIP != m_localIP)
	{
		/* 		fprintf(stderr, "[LAN86] handleRequestJoin ignored sender=%d.%d.%d.%d reason=wrong-game-ip\n",
			PRINTF_IP_AS_4_INTS(senderIP)); */
		return; // Not us.  Ignore it.
	}
	LANMessage reply;
	fillInLANMessage( &reply );
	if (!m_inLobby && AmIHost())
	{
		if (m_currentGame->isGameInProgress())
		{
			reply.messageType = LANMessage::MSG_JOIN_DENY;
			reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_STARTED;
			reply.GameNotJoined.gameIP = m_localIP;
			reply.GameNotJoined.playerIP = senderIP;
			/* 			fprintf(stderr, "[LAN86] handleRequestJoin deny sender=%d.%d.%d.%d reason=game-started responseIP=%d.%d.%d.%d\n",
				PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(responseIP)); */
			DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game already started."));
		}
		else
		{
			int player;
			Bool canJoin = true;

			// see if the CRCs match
#if defined(RTS_DEBUG)
			if (TheGlobalData->m_netMinPlayers > 0) {
#endif
// TheSuperHackers @todo Enable CRC checks!
#if !RTS_ZEROHOUR
			if (msg->GameToJoin.iniCRC != TheGlobalData->m_iniCRC ||
					msg->GameToJoin.exeCRC != TheGlobalData->m_exeCRC)
			{
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of CRC mismatch. CRCs are them/us INI:%X/%X exe:%X/%X",
					msg->GameToJoin.iniCRC, TheGlobalData->m_iniCRC,
					msg->GameToJoin.exeCRC, TheGlobalData->m_exeCRC));
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				reply.GameNotJoined.reason = LANAPIInterface::RET_CRC_MISMATCH;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				canJoin = false;
			}
#endif
#if defined(RTS_DEBUG)
			}
#endif

// TheSuperHackers @tweak Disables the duplicate serial check
#if 0
			// check for a duplicate serial
			AsciiString s;
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				s.clear();
				if (player == 0)
				{
					GetStringFromRegistry("\\ergc", "", s);
				}
				else if (slot->isHuman())
				{
					s = slot->getSerial();
					if (s.isEmpty())
						s = "<Munkee>";
				}

				if (s.isNotEmpty())
				{
					DEBUG_LOG(("Checking serial '%s' in slot %d", s.str(), player));

					if (!strncmp(s.str(), msg->GameToJoin.serial, g_maxSerialLength))
					{
						// serials match!  kick the punk!
						reply.messageType = LANMessage::MSG_JOIN_DENY;
						reply.GameNotJoined.reason = LANAPIInterface::RET_SERIAL_DUPE;
						reply.GameNotJoined.gameIP = m_localIP;
						reply.GameNotJoined.playerIP = senderIP;
						canJoin = false;

						DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate serial # (%s).", s.str()));
						break;
					}
				}
			}
#endif

			// TheSuperHackers @bugfix slurmlord 18/09/2025 need to validate the name of the connecting player before
			// allowing them to join to prevent messing up the format of game state string. Commas, colons, semicolons etc.
			// should not be in a player name. It should also not consist of only space characters.
			if (canJoin)
			{
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
				if (ContainsInvalidChars(GetWindowsWideCharAsWchar(msg->name)) || !ContainsAnyReadableChars(GetWindowsWideCharAsWchar(msg->name)))
				{
					// Just deny with a duplicate name reason, for backwards compatibility with retail
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;
					/* 					fprintf(stderr, "[LAN86] handleRequestJoin deny sender=%d.%d.%d.%d reason=invalid-name responseIP=%d.%d.%d.%d\n",
						PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(responseIP)); */

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of illegal characters in the player name."));
				}
			}	

			// Then see if the player has a duplicate name
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
				if (slot->isHuman() && slot->getName().compare(GetWindowsWideCharAsWchar(msg->name)) == 0)
				{
					// just deny duplicates
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;
					/* 					fprintf(stderr, "[LAN86] handleRequestJoin deny sender=%d.%d.%d.%d reason=duplicate-name responseIP=%d.%d.%d.%d\n",
						PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(responseIP)); */

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate names."));
					break;
				}
			}

			// TheSuperHackers @bugfix Stubbjax 26/09/2025 Players can now join open slots regardless of starting spots on the map.
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				if (m_currentGame->getLANSlot(player)->isOpen())
				{
					// OK, add him in.
					reply.messageType = LANMessage::MSG_JOIN_ACCEPT;
					// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
					CopyWcharToWindowsWideChar(reply.GameJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameJoined.gameName) - 1);
					reply.GameJoined.slotPosition = player;
					reply.GameJoined.gameIP = m_localIP;
					reply.GameJoined.playerIP = senderIP;

					LANGameSlot newSlot;
					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
					newSlot.setState(SLOT_PLAYER, UnicodeString(GetWindowsWideCharAsWchar(msg->name)));
					newSlot.setIP(senderIP);
					/*	TheSuperHackers @feature this is the load-bearing half of
						per-instance gameplay ports, and the reason they are derived
						rather than configured.

						We are the host, filling in a slot for SOMEBODY ELSE, and we
						then publish that slot to every peer. The joiner has no way to
						tell us its gameplay port: the wire format is frozen for retail
						compatibility and GameToJoin carries only gameIP, exeCRC,
						iniCRC and serial. What we DO have is the source port of the
						join request we are handling right now, which LANAPI::update
						recorded via notePeerPort before dispatching here. Deriving the
						gameplay port from that lobby port is what lets two engines on
						one machine be told apart by everyone in the game.

						A stock client joins from lobby 8086, so this yields 8088 and
						the slot is byte-identical to what the original code wrote. */
					newSlot.setPort(GetPeerGamePort(senderIP));
					newSlot.setLastHeard(timeGetTime());
					newSlot.setSerial(msg->GameToJoin.serial);
					m_currentGame->setSlot(player,newSlot);
					DEBUG_LOG(("LANAPI::handleRequestJoin - added player %ls at ip 0x%08x to the game", msg->name, senderIP));
					/* 					fprintf(stderr, "[LAN86] handleRequestJoin accept sender=%d.%d.%d.%d slot=%d responseIP=%d.%d.%d.%d game=%ls\n",
						PRINTF_IP_AS_4_INTS(senderIP), player, PRINTF_IP_AS_4_INTS(responseIP), m_currentGame->getName().str()); */

					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
					OnPlayerJoin(player, UnicodeString(GetWindowsWideCharAsWchar(msg->name)));

					break;
				}
			}

			if (canJoin && player == MAX_SLOTS)
			{
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
				CopyWcharToWindowsWideChar(reply.GameNotJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameNotJoined.gameName) - 1);
				reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_FULL;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				/* 				fprintf(stderr, "[LAN86] handleRequestJoin deny sender=%d.%d.%d.%d reason=game-full responseIP=%d.%d.%d.%d\n",
					PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(responseIP)); */
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game is full."));
			}
		}
	}
	else
	{
		reply.messageType = LANMessage::MSG_JOIN_DENY;
		reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_GONE;
		reply.GameNotJoined.gameIP = m_localIP;
		reply.GameNotJoined.playerIP = senderIP;
		/* 		fprintf(stderr, "[LAN86] handleRequestJoin deny sender=%d.%d.%d.%d reason=game-gone responseIP=%d.%d.%d.%d\n",
			PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(responseIP)); */
	}
	/*	Answer the joiner directly.

		TheSuperHackers @info corrected 2026-09-22. An earlier version of this
		comment claimed "on the accept path responseIP is deliberately 0, which
		makes sendMessage broadcast". That was simply wrong about this code:
		responseIP is initialised to senderIP at the top of this function and is
		never reassigned on any path, accept or deny. So this call has always
		been a unicast straight back to whoever asked, and the second send below
		it -- guarded on `responseIP == 0` -- was unreachable.

		That guard also carried `senderIP != m_localIP`, which would have
		suppressed the direct reply for a co-located joiner: exactly the peer a
		broadcast cannot reach, and the one the extra send was meant to rescue.
		Removed rather than fixed, because the unicast above already does the job.

		This unicast is what makes co-located joining work at all, and it is why
		six bots on one machine do not steal each other's accepts: each accept
		reaches only the bot that asked, and that bot only acts on it while
		m_pendingAction == ACT_JOIN (see handleJoinAccept), which it then clears.
		Verified, not assumed -- it is the reason no new wire field is needed.
	*/
	replyToSender(&reply, responseIP);

	RequestGameOptions(GenerateGameOptionsString(), true);
}

void LANAPI::handleJoinAccept( LANMessage *msg, UnsignedInt senderIP )
{
	// GeneralsX @build GitHubCopilot 12/04/2026 Trace directed join-accept processing and pending action transitions for LAN/direct-connect debugging.
	/* 	fprintf(stderr, "[LAN86] handleJoinAccept sender=%d.%d.%d.%d playerIP=%d.%d.%d.%d localIP=%d.%d.%d.%d pending=%d slot=%d game=%ls\n",
		PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(msg->GameJoined.playerIP), PRINTF_IP_AS_4_INTS(m_localIP),
		m_pendingAction, msg->GameJoined.slotPosition, GetWindowsWideCharAsWchar(msg->GameJoined.gameName)); */
	// isAddressedToSelf, not == m_localIP: the host echoes the address IT saw,
	// which over loopback is not the one we bound. See LANAPI.h.
	if (isAddressedToSelf(msg->GameJoined.playerIP)) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			m_currentGame = LookupGame(UnicodeString(GetWindowsWideCharAsWchar(msg->GameJoined.gameName)));

			if (!m_currentGame)
			{
				DEBUG_CRASH(("Could not find game to join!"));
				OnGameJoin(RET_UNKNOWN, nullptr);
			}
			else
			{
				m_inLobby = false;
				AsciiString options = GameInfoToAsciiString(m_currentGame);
				m_currentGame->enterGame();
				ParseAsciiStringToGameInfo(m_currentGame, options);

				Int pos = msg->GameJoined.slotPosition;

				/*	Adopt the address the HOST gave us, which is the one it
					observed and therefore the one it has put in the slot list
					that every other peer will use to reach us. Over loopback it
					is not the address we bound (we asked for 127.0.0.2, the
					kernel sourced from 127.0.0.1), and if we keep ours instead
					then the host's next slot-list update overwrites our slot
					with its value and isLocalPlayer() can no longer find us --
					we would be in the game and unable to see ourselves in it.

					On a real NIC this is the same value as m_localIP, so
					nothing changes for a retail peer.

					Assign the field, do NOT call SetLocalIP(): that resets and
					re-inits the transport, which would tear down the very
					socket this accept arrived on, mid-join. Rebinding buys
					nothing anyway -- on POSIX the lobby socket binds
					INADDR_ANY, so which local address it "has" does not affect
					what it receives. What we are correcting is our IDENTITY,
					not our binding. */
				if (isAddressedToSelf(msg->GameJoined.playerIP) &&
						(msg->GameJoined.playerIP != m_localIP))
				{
					m_localIP = msg->GameJoined.playerIP;
				}

				LANGameSlot slot;
				slot.setState(SLOT_PLAYER, m_name);
				slot.setIP(m_localIP);
				/*	TheSuperHackers @feature our own slot on being accepted, so this
					is the port we bind. It must match what the host independently
					derived for us in handleRequestJoin, and it does: both sides run
					LANGamePortFromLobbyPort over the same lobby port -- ours directly,
					the host's from the source port of our join request. This value is
					local anyway; the host's slot list is authoritative and overwrites
					it moments later. Setting it correctly keeps the two consistent in
					the window before that arrives. */
				slot.setPort(GetGamePort());
				slot.setLastHeard(0);
				slot.setLogin(m_userName);
				slot.setHost(m_hostName);
				m_currentGame->setSlot(pos, slot);

				m_currentGame->getLANSlot(0)->setHost(msg->hostName);
				m_currentGame->getLANSlot(0)->setLogin(msg->userName);

				LANPreferences prefs;
				AsciiString entry;
				entry.format("%d.%d.%d.%d:%s", PRINTF_IP_AS_4_INTS(senderIP), UnicodeStringToQuotedPrintable(m_currentGame->getSlot(0)->getName()).str());
				prefs["RemoteIP0"] = entry;
				prefs.write();

				OnGameJoin(RET_OK, m_currentGame);
				//DEBUG_CRASH(("setting host to %ls@%ls", m_currentGame->getLANSlot(0)->getUser()->getLogin().str(),
				//	m_currentGame->getLANSlot(0)->getUser()->getHost().str()));
			}
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
		else
		{
			/* 			fprintf(stderr, "[LAN86] handleJoinAccept ignored sender=%d.%d.%d.%d reason=unexpected-pending-action pending=%d\n",
				PRINTF_IP_AS_4_INTS(senderIP), m_pendingAction); */
		}
	}
}

void LANAPI::handleJoinDeny( LANMessage *msg, UnsignedInt senderIP )
{
	/* 	fprintf(stderr, "[LAN86] handleJoinDeny sender=%d.%d.%d.%d playerIP=%d.%d.%d.%d localIP=%d.%d.%d.%d pending=%d reason=%d game=%ls\n",
		PRINTF_IP_AS_4_INTS(senderIP), PRINTF_IP_AS_4_INTS(msg->GameJoined.playerIP), PRINTF_IP_AS_4_INTS(m_localIP),
		m_pendingAction, msg->GameNotJoined.reason, GetWindowsWideCharAsWchar(msg->GameNotJoined.gameName)); */
	/*	Same widening as handleJoinAccept, and it matters just as much: a DENY
		dropped because the host named an address we do not recognise leaves us
		retrying for ever instead of reporting why we were refused. The reason
		that will actually come up is RET_DUPLICATE_NAME, since several bots on
		one machine must each have a distinct name. */
	if (isAddressedToSelf(msg->GameJoined.playerIP)) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			OnGameJoin(msg->GameNotJoined.reason, LookupGame(UnicodeString(GetWindowsWideCharAsWchar(msg->GameNotJoined.gameName))));
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

void LANAPI::handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		/*	Resolve by (address, port). An address-only scan returns slot 0 for
			every co-located player, and slot 0 means THE HOST LEFT -- so one
			bot quitting would have torn down the game for everybody. */
		const Int player = slotForSender(senderIP);
		if (player >= 0)
		{
			if (player == 0)
			{
				OnHostLeave();
				removeGame(m_currentGame);
				delete m_currentGame;
				m_currentGame = nullptr;

				/// @todo re-add myself to lobby?  Or just keep me there all the time?  If we send a LOBBY_ANNOUNCE things'll work out...
				LANPlayer *lanPlayer = LookupPlayer(m_localIP);
				if (!lanPlayer)
				{
					lanPlayer = NEW LANPlayer;
					lanPlayer->setIP(m_localIP);
				}
				else
				{
					removePlayer(lanPlayer);
				}
				lanPlayer->setName(UnicodeString(m_name));
				lanPlayer->setHost(m_hostName);
				lanPlayer->setLogin(m_userName);
				lanPlayer->setLastHeard(timeGetTime());
				addPlayer(lanPlayer);

			}
			else
			{
				if (AmIHost())
				{
					// remove the deadbeat
					LANGameSlot slot;
					slot.setState(SLOT_OPEN);
					m_currentGame->setSlot( player, slot );
				}
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
				OnPlayerLeave(UnicodeString(GetWindowsWideCharAsWchar(msg->name)));
				m_currentGame->getLANSlot(player)->setState(SLOT_OPEN);
				m_currentGame->resetAccepted();
				RequestGameOptions(GenerateGameOptionsString(), false, senderIP);
				//m_currentGame->endGame();
			}
		}
		/*	No DEBUG_ASSERTCRASH on player < 0 here. The original one sat INSIDE
			the match, where `player < MAX_SLOTS` held by construction, so it
			could never fire. Moving it out would make it live -- and it would
			fire on an ordinary event: a LEAVE from someone who is not in our
			game, which co-located instances see routinely, because a loopback
			broadcast reaches every listener on the machine. */
	}
	else if (m_inLobby)
	{
		// Look for dissappearing games
		LANGameInfo *game = m_games;
		while (game)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			if (game->getName().compare(GetWindowsWideCharAsWchar(msg->GameToLeave.gameName)) == 0)
			{
				removeGame(game);
				delete game;
				OnGameList(m_games);
				break;
			}
			game = game->getNext();
		}
	}
}

void LANAPI::handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			if (player->getIP() == senderIP)
			{
				removePlayer(player);
				OnPlayerList(m_lobbyPlayers);
				break;
			}
			player = player->getNext();
		}
	}
}

void LANAPI::handleSetAccept( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		// slotForSender: address alone matches the wrong player when two
		// instances share one. See LANAPI.h.
		if (slotForSender(senderIP) >= 0)
		{
			OnAccept(senderIP, msg->Accept.isAccepted);
		}
	}
}

void LANAPI::handleHasMap( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame)
	{
		CRC mapNameCRC;
//	mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
		AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
		mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
		if (mapNameCRC.get() != msg->MapStatus.mapCRC)
		{
			return;
		}

		if (slotForSender(senderIP) >= 0)
		{
			OnHasMap(senderIP, msg->MapStatus.hasMap);
		}
	}
}

void LANAPI::handleChat( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player;
		if((player=LookupPlayer(senderIP)) != nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			OnChat(UnicodeString(player->getName()), player->getIP(), UnicodeString(GetWindowsWideCharAsWchar(msg->Chat.message)), msg->Chat.chatType);
			player->setLastHeard(timeGetTime());
		}
	}
	else
	{
		// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
		if (LookupGame(UnicodeString(GetWindowsWideCharAsWchar(msg->Chat.gameName))) != m_currentGame)
		{
			DEBUG_LOG(("Game '%ls' is not my game", msg->Chat.gameName));
			if (m_currentGame)
			{
				DEBUG_LOG(("Current game is '%ls'", m_currentGame->getName().str()));
			}
			return;
		}

		const Int player = slotForSender(senderIP);
		if (player >= 0)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
			OnChat(UnicodeString(GetWindowsWideCharAsWchar(msg->name)), m_currentGame->getIP(player), UnicodeString(GetWindowsWideCharAsWchar(msg->Chat.message)), msg->Chat.chatType);
		}
	}
}

void LANAPI::handleGameStart( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == senderIP && !m_currentGame->isGameInProgress())
	{
		OnGameStart();
	}
}

void LANAPI::handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == senderIP && !m_currentGame->isGameInProgress())
	{
		OnGameStartTimer(msg->StartTimer.seconds);
	}
}

void LANAPI::handleGameOptions( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		// The slot number is what OnGameOptions acts on, so resolving it by
		// address alone would apply one player's options to another.
		const Int player = slotForSender(senderIP);
		if (player >= 0)
		{
			OnGameOptions(senderIP, player, AsciiString(msg->GameOptions.options));
		}
	}
}

void LANAPI::handleInActive(LANMessage *msg, UnsignedInt senderIP) {
	if (m_inLobby || (m_currentGame == nullptr) || (m_currentGame->isGameInProgress())) {
		return;
	}

	// check to see if we are the host of this game.
	if (m_currentGame->amIHost() == FALSE) {
		return;
	}

	UnicodeString playerName;
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharAsWchar (fighter19 pattern)
	playerName = UnicodeString(GetWindowsWideCharAsWchar(msg->name));

	Int slotNum = m_currentGame->getSlotNum(playerName);
	if (slotNum < 0)
		return;
	GameSlot *slot = m_currentGame->getSlot(slotNum);
	if (slot == nullptr) {
		return;
	}

	if (senderIP != slot->getIP()) {
		return;
	}

	// don't want to unaccept the host, that's silly.  They can't hit start alt-tabbed anyways.
	if (senderIP == GetLocalIP()) {
		return;
	}

	// only unaccept if the timer hasn't started yet.
	if (m_gameStartTime != 0) {
		return;
	}

	slot->unAccept();
	AsciiString options = GenerateGameOptionsString();
	RequestGameOptions(options, FALSE);
	lanUpdateSlotList();
}
