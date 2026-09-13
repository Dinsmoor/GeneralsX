/*
**	Observation server: streams per-frame game state to an external agent.
**
**	Enabled with the command line argument "-obsport <port>". When disabled
**	(the default) every entry point is a no-op and no socket is created.
**
**	The server listens on 127.0.0.1 and writes one newline terminated JSON
**	object per observed frame to any connected client. It is intentionally
**	one directional; issuing orders is a separate concern.
*/

#pragma once

#ifndef _OBSERVATION_SERVER_H_
#define _OBSERVATION_SERVER_H_

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

#include <string>
#include <vector>

/**
	-obsplayer value meaning "whichever slot this machine controls".

	-1 already means "no observing player": the omniscient view, which is
	right for after-action analysis and WRONG for an agent playing a live
	match, because it sees through the shroud. In a network game the slot
	depends on join order and cannot be named on the command line, so this
	is resolved from ThePlayerList once the game is running.
*/
const Int OBSERVE_LOCAL_PLAYER = -2;

class ObservationServer
{
public:

	ObservationServer();
	~ObservationServer();

	/// Begin listening on the loopback interface. Safe to call more than once.
	void init( UnsignedShort port, UnsignedInt frameInterval );

	/// Stop listening and drop any connected client.
	void shutdown();

	Bool isEnabled() const { return m_enabled; }

	/**
		Emit the observation for the current frame, if a client is connected
		and the frame interval has elapsed. Called once per logic frame.
	*/
	void update();

	/**
		Record one line of in-game chat for the next observation.

		Called from ConnectionManager::processChat, which is the single
		funnel every chat line passes through on every machine -- global
		and team alike. The agent reads all of it: a human reads the
		opponent's taunts too, and an agent that cannot see chat cannot
		be talked to.

		Buffered rather than sent immediately because observations are
		rebuilt whole on an interval; chat is transient and would
		otherwise be gone by the time the next snapshot is built.
	*/
	void recordChat( Int senderSlot, const UnicodeString &text, Int playerMask );

private:

	/// Accept a pending connection, if any. Never blocks.
	void acceptClient();

	/// Write the buffer to the client, dropping it if the write fails.

	/// Send an arbitrary payload, bypassing the AsciiString length cap.
	void sendRaw( const std::string &payload );
	void sendRaw( const char *data, Int length );

	/// Serialize the current game state into out.
	void buildObservation( std::string &out );

	/// Serialize the static map description (terrain, passability) into out.
	void buildMapDescription();

	Bool				m_enabled;
	Bool				m_unitsOnly;			///< skip map scenery, keep units and objectives
	Int					m_playerIndex;			///< whose point of view to report, or -1 for omniscient
	Bool				m_bindToLocalPlayer;	///< -obsplayer OBSERVE_LOCAL_PLAYER: resolve to the local slot once the game exists

	void				resolvePlayerIndex();	///< binds m_playerIndex to the local player when asked to
	Bool				m_sentMap;				///< the static map is sent once per connection
	UnsignedInt			m_frameInterval;		///< emit every Nth logic frame
	UnsignedInt			m_lastSentFrame;
	UnsignedInt			m_listenSocket;			///< SOCKET, kept opaque here
	UnsignedInt			m_clientSocket;			///< SOCKET, or invalid when idle

	/**	Chat lines seen since the last observation was sent.

		Bounded: a flood of chat must not grow this without limit when no
		client is connected to drain it.
	*/
	struct ChatLine
	{
		Int				senderSlot;
		Int				playerMask;
		UnsignedInt		frame;
		std::string		text;			///< UTF-8, already JSON-escaped
	};
	std::vector<ChatLine>	m_chat;
	// An enum, not a static const size_t: VC6 rejects an in-class
	// initializer on a static data member ("pure specifier can only be
	// specified for functions"), and this toolchain is VC6.
	enum { MAX_CHAT_BUFFERED = 64 };

};

extern ObservationServer *TheObservationServer;	///< singleton instance

#endif // _OBSERVATION_SERVER_H_
