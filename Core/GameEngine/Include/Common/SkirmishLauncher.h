/*
**	Headless skirmish launcher: starts a skirmish match from the command line,
**	without the shell UI, so an external agent can play or train against the
**	built-in AI.
**
**	Enabled with "-skirmish <slots>". Disabled by default, in which case none
**	of this is reachable.
**
**	The match is described with the engine's own GameInfo string -- the same
**	format the skirmish menu builds and a replay header stores -- so the setup
**	path is identical to a human starting a game from the menu.
*/

#pragma once

#ifndef _SKIRMISH_LAUNCHER_H_
#define _SKIRMISH_LAUNCHER_H_

#include "Lib/BaseType.h"

class AsciiString;

namespace SkirmishLauncher
{
	/**
		Run a single headless skirmish to completion (or to the frame limit)
		and return a process exit code. Only valid when -skirmish was given.
	*/
	Int runSkirmish();
}

#endif // _SKIRMISH_LAUNCHER_H_
