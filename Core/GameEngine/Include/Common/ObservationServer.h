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

#include <string>

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
	Bool				m_sentMap;				///< the static map is sent once per connection
	UnsignedInt			m_frameInterval;		///< emit every Nth logic frame
	UnsignedInt			m_lastSentFrame;
	UnsignedInt			m_listenSocket;			///< SOCKET, kept opaque here
	UnsignedInt			m_clientSocket;			///< SOCKET, or invalid when idle

};

extern ObservationServer *TheObservationServer;	///< singleton instance

#endif // _OBSERVATION_SERVER_H_
