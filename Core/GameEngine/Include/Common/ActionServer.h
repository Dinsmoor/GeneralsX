/*
**	Action server: accepts orders from an external agent and injects them
**	into the game as if a player had issued them.
**
**	Enabled with the command line argument "-actport <port>". When disabled
**	(the default) every entry point is a no-op and no socket is created.
**
**	Orders arrive as newline terminated JSON objects on 127.0.0.1 and are
**	turned into GameMessages appended to TheCommandList, which is the same
**	place the network layer puts commands received from remote players.
**	Nothing here touches simulation state directly, so the simulation stays
**	bit identical and replay/CRC compatible.
*/

#pragma once

#ifndef _ACTION_SERVER_H_
#define _ACTION_SERVER_H_

#include "Lib/BaseType.h"
#include "Common/MessageStream.h"

#include <string>


class ActionServer
{
public:

	ActionServer();
	~ActionServer();

	/// Begin listening on the loopback interface. Safe to call more than once.
	void init( UnsignedShort port, Int playerIndex );

	/// Stop listening and drop any connected client.
	void shutdown();

	Bool isEnabled() const { return m_enabled; }

	/**
		Read and execute any pending orders. Called once per logic frame,
		before the command list is processed.
	*/
	void update();

private:

	/// Accept a pending connection, if any. Never blocks.
	void acceptClient();

	/// Drain the socket into m_pending. Never blocks.
	void receive();

	/// Execute one complete JSON order line.
	void executeLine( const char *line );

	/// Reply to the agent with a single JSON status line. Best effort.
	void reply( const char *status, const char *detail );

	/// Send an already-built JSON object body. Used by queries whose answer does
	/// not fit the fixed status/detail shape, such as a command set dump.
	void replyRaw( const std::string& body );

	/**
		Begin a message of the given type attributed to the controlled player.
		Returns nullptr if the type is not a legal network command.
	*/
	GameMessage *beginMessage( GameMessage::Type type );

	/**
		How many messages are queued on TheCommandList right now. update()
		charges the difference across executeLine() against this frame's
		AIGroup budget -- see the pacing note there.
	*/
	static Int countCommands();

	/// Select exactly the given objects, so the following command applies to them.
	Bool selectObjects( const char *json );

	Bool			m_enabled;
	Int				m_playerIndex;			///< the player whose orders these are, or -1 for "the local player"

	Int				playerIndex() const;	///< resolves m_playerIndex, or the local player when it is -1
	UnsignedInt		m_listenSocket;			///< SOCKET, kept opaque here
	UnsignedInt		m_clientSocket;			///< SOCKET, or invalid when idle
	class AsciiString*	m_pending;			///< partial line carried between frames

	/*
		The AIGroup budget, carried ACROSS calls because update() is called
		once per iteration of the main loop, NOT once per logic frame. See the
		pacing note in update() -- these two members are the whole reason the
		budget is honest.
	*/
	UnsignedInt		m_budgetFrame;			///< the logic frame m_queuedThisFrame belongs to
	Int				m_queuedThisFrame;		///< AIGroup-minting messages queued during m_budgetFrame

};

extern ActionServer *TheActionServer;	///< singleton instance

#endif // _ACTION_SERVER_H_
