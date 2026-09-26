/*
	FrameTrace: where the wall time between two logic frames went.

	Enabled by setting GENERALS_FRAME_TRACE to a path; unset, every call is a
	branch on a null pointer. One line per logic frame, network games only
	(the question it answers is a lockstep one):

	    GENERALS_FRAME_TRACE=/tmp/frames.tsv ./GeneralsXZH ...

	Columns (milliseconds unless named):
	    frame     logic frame just executed
	    gap       wall time since the previous logic frame -- 33 at 30 fps; a
	              stutter is a run of these far above that
	    loops     engine loop iterations inside the gap
	    client    radar + audio + GameClient update (rendering, on a GUI)
	    act       the bot's action server
	    net       Network::update (packets in and out)
	    logic     GameLogic::update, obs included
	    obs       the observation server alone (building + sending to the agent)
	    wait      time inside the gap the frame's commands were NOT all here
	    slot      the first player slot whose commands were missing, last seen
	              while waiting (-1: never waited)
	    runahead  frames of run-ahead in force
	    fps       the network's agreed frame rate
	    cushion   ConnectionManager's minimum cushion
	The rest of `gap` is the engine idling (the frame pacer's sleep, or
	Network::timeForNewFrame holding the frame back).

	Every peer writing one of these over the same match lines up by frame
	number, so the peer that made everyone wait is the one whose own trace
	shows the time spent somewhere other than `wait`.
*/
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace FrameTrace
{
	enum Bucket { CLIENT, ACT, NET, LOGIC, OBS, BUCKETS };

	inline FILE *s_out = nullptr;
	inline bool s_checked = false;
	inline int64_t s_ns[BUCKETS] = {};
	inline int64_t s_waitNs = 0;
	inline int64_t s_lastFrameNs = 0;
	inline int64_t s_waitSince = 0;
	inline int s_loops = 0;
	inline int s_slot = -1;
	/// Set by Network::timeForNewFrame, which computes it anyway; asking the
	/// network for it would reset the value the game's own display reads.
	inline unsigned s_cushion = 0;

	inline int64_t now()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	inline bool on()
	{
		if (!s_checked)
		{
			s_checked = true;
			const char *path = getenv("GENERALS_FRAME_TRACE");
			if (path != nullptr && *path != '\0')
			{
				s_out = fopen(path, "w");
				if (s_out != nullptr)
					fprintf(s_out, "frame\tgap\tloops\tclient\tact\tnet\tlogic\tobs\twait\tslot\trunahead\tfps\tcushion\n");
			}
		}
		return s_out != nullptr;
	}

	/// Times one bucket for as long as it is in scope.
	struct Scope
	{
		Bucket b;
		int64_t t0;
		explicit Scope(Bucket bucket) : b(bucket), t0(on() ? now() : 0) {}
		~Scope() { if (s_out != nullptr) s_ns[b] += now() - t0; }
	};

	inline void loop() { if (s_out != nullptr) ++s_loops; }

	/// Called once per Network::update with whether the frame's commands are
	/// all here, and if not, which slot was the first one missing.
	inline void ready(bool allReady, int missingSlot)
	{
		if (s_out == nullptr)
			return;
		const int64_t t = now();
		if (!allReady)
		{
			if (s_waitSince == 0)
				s_waitSince = t;
			s_slot = missingSlot;
		}
		else if (s_waitSince != 0)
		{
			s_waitNs += t - s_waitSince;
			s_waitSince = 0;
		}
	}

	inline void frame(unsigned frameNo, int runAhead, int fps)
	{
		if (s_out == nullptr)
			return;
		const int64_t t = now();
		if (s_waitSince != 0)
		{
			s_waitNs += t - s_waitSince;
			s_waitSince = 0;
		}
		const double ms = 1e-6;
		fprintf(s_out, "%u\t%.1f\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t%d\t%d\t%u\n",
			frameNo, s_lastFrameNs ? (t - s_lastFrameNs) * ms : 0.0, s_loops,
			s_ns[CLIENT] * ms, s_ns[ACT] * ms, s_ns[NET] * ms, s_ns[LOGIC] * ms, s_ns[OBS] * ms,
			s_waitNs * ms, s_slot, runAhead, fps, s_cushion);
		// A crash or a kill must not lose the minute that mattered.
		if ((frameNo % 30) == 0)
			fflush(s_out);
		s_lastFrameNs = t;
		for (int i = 0; i < BUCKETS; ++i)
			s_ns[i] = 0;
		s_waitNs = 0;
		s_loops = 0;
		s_slot = -1;
	}
}
