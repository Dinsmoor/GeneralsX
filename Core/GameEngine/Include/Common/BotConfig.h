/*
**	One INI file describing a headless bot, instead of a dozen flags.
**
**	A bot needs a role, an address, an identity, a faction and a pair of
**	sockets for the agent driving it. Passing each of those as its own
**	command-line flag made the relationships between them invisible -- which
**	flags were required together, which were host-only, which were ignored --
**	and the command lines were long enough to hide a typo.
**
**	    generalszh.exe -headless -noaudio -nologo -botconfig joiner.ini
**
**	The file is read with UserPreferences, the engine's existing "key = value"
**	reader (the same one behind Network.ini), so this adds a schema and not a
**	parser. Keys are case-insensitive and every one is optional; an empty file
**	is a valid do-nothing config.
**
**	    ; who we are
**	    role     = join          ; join | host
**	    host     = 192.168.1.10  ; the host's address (role=join)
**	    map      = Alpine Assault ; the map to host (role=host)
**	    players  = 2             ; total SEATS, humans and AI (role=host)
**	    slots    = medium,medium ; what sits in slots 1..n; slot 0 is us
**	                             ;   open | closed | easy | medium | hard
**	    teams    = 1,2,1,2       ; team per slot INCLUDING slot 0, 1-based,
**	                             ;   "-" for none
**	    lanLobby = no            ; host a broadcast LAN game, not direct connect
**	    name     = Claude        ; lobby name, and the word we answer to
**	    localIP  = 127.0.0.2     ; bind LAN networking to this address
**	    faction  = China         ; china | america | gla | a general's name
**
**	    ; the agent driving us
**	    obsPort     = 7777
**	    obsInterval = 30
**	    obsPlayer   = -2
**	    actPort     = 7778
**	    actPlayer   = -1
**	    frames      = 36000      ; give up after this many logic frames
**	    lobbyScript = drive.txt  ; test-only; see -lobbyscript
**
**	The faction is stored as the WORD, not as a player-template index. The
**	config is read at startup, long before INI data exists, so there is no
**	template store to resolve against yet -- and an index would be opaque
**	anyway, since the templates live inside the .big archives. The word is
**	resolved once the lobby is up, by the same factionIndexFromWord() that
**	reads "bot faction china" out of chat.
*/

#pragma once

#ifndef _BOT_CONFIG_H_
#define _BOT_CONFIG_H_

#include "Lib/BaseType.h"

class AsciiString;

namespace BotConfig
{
	/**
		Read the named file and apply it to TheWritableGlobalData, exactly as
		the equivalent flags would have. Returns FALSE if the file could not
		be opened, having changed nothing.

		Relative names are resolved against the user data directory, where
		Network.ini and the other preference files already live; a name
		containing a slash or a drive letter is used as given.

		Called from the startup parse, so it may not touch anything that INI
		data creates.
	*/
	Bool load( const AsciiString& fname );
}

#endif // _BOT_CONFIG_H_
