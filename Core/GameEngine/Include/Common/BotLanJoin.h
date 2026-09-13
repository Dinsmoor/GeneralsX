/*
**	Headless LAN joiner: joins a locally hosted multiplayer game from the
**	command line, without the shell UI, so an external agent can play a real
**	network match against people.
**
**	Enabled with "-botjoinmp <host ip>". Disabled by default, in which case
**	none of this is reachable.
**
**	The bot appears in the host's lobby as an ordinary player. It picks its
**	faction, team, colour and start position from LOBBY CHAT, using the same
**	RequestGameOptions path the options menu uses, so the host sees nothing
**	unusual: a joiner asking for a slot setting. It then accepts, and the
**	host starts the game when everyone is ready.
**
**	Commands are addressed to the bot by name and read from ordinary chat:
**
**	    bot faction china        bot team 1       bot color 3
**	    bot start 2              bot ready        bot unready
**	    bot say <anything>       bot help
**
**	See BotLanJoin.cpp for why the LAN layer has to be pumped here.
*/

#pragma once

#ifndef _BOT_LAN_JOIN_H_
#define _BOT_LAN_JOIN_H_

#include "Lib/BaseType.h"

class AsciiString;
class UnicodeString;

namespace BotLanJoin
{
	/**
		Join the game at the address given to -botjoinmp, take part in the
		lobby, play the match to its end, and return a process exit code.
		Only valid when -botjoinmp was given.
	*/
	Int runLanJoin();

	/**
		Host a LAN game on the map given to -nethost, wait for the lobby to
		fill, start the match and play it. Only valid when -nethost was given.
	*/
	Int runLanHost();

	/**
		Handle one line of lobby chat. Called by the LAN chat callback; public
		so the callback in LANAPICallbacks can reach it without knowing the
		internals. Returns TRUE when the line was a command for us.
	*/
	Bool handleChat( const UnicodeString& speaker, const UnicodeString& text );
}

#endif // _BOT_LAN_JOIN_H_
