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


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CRCDebug.h"
#include "Common/Debug.h"
#include "Common/PerfTimer.h"
#include "Common/LocalFileSystem.h"
#include "GameClient/InGameUI.h"
#include "GameNetwork/IPEnumeration.h"
#include <cstdarg>
#include <cstdlib>
#include <cstring>


#ifdef DEBUG_CRC

static const Int MaxStrings = 64000;
static const Int MaxStringLen = 1024;

/*	The ring, sized at RUNTIME.

	It was a fixed 64,000-line static array, which is ~7 FRAMES on a machine
	that renders: measured 2026-09-15, a playing host logs ~8,300 CRC lines per
	frame against a headless bot's ~234, 35x more. The two captures therefore
	overlapped by only three frames and the actual divergence was outside the
	window on the host's side -- the capture proved the machines were in sync
	right up to the last frame it could see, and said nothing about where they
	parted.

	-CRCRingLines N sets the ring size so a rendering client can hold a useful
	window. ~250,000 lines is about 30 host frames and ~255 MB.
*/
static char (*DebugStrings)[MaxStringLen] = nullptr;
static Int RingLines = MaxStrings;
static Int nextDebugString = 0;
static Int numDebugStrings = 0;

/*	KEEP THE LAST FEW FRAMES IN MEMORY AND WRITE ONLY ON A MISMATCH.

	The per-frame dump explains WHAT diverged, and it is the only thing that
	does. But writing every frame costs ~21 MB/s synchronously inside the game
	loop -- measured 2026-09-14, 11.6 GB in nine minutes -- which made a live
	multiplayer match unplayable and changed the very timing we were trying to
	observe.

	The buffer above is already a ring of 64,000 lines. All that was missing
	is not emptying it every frame: keep writing into it, remember where each
	frame began, and when the network reports a CRC mismatch flush the last
	RING_FRAMES frames to disk in one go. Disk cost during play is then ZERO,
	and the frames on either side of the divergence -- the ones that actually
	matter -- are the ones preserved.

	g_crcRingFrames > 0 selects this mode (-CRCRingFrames N).
*/
Int g_crcRingFrames = 0;
Int g_crcRingLines = 0;		///< -CRCRingLines N: ring size; 0 = the default MaxStrings

/**
 * Allocate the ring. Safe to call more than once; grows only.
 */
static void ensureRing()
{
	/*	Clamped, because this is a 32-BIT process.

		A single malloc of 1,000,000 * 1024 = 1.02 GB fails in the Win32
		address space and the old code then kept the previous (or null) ring
		and wrote NOTHING -- silently, which is the worst way to lose a
		capture you can only take once. 400,000 lines (409 MB) is verified to
		work; cap there and say so if the caller asks for more.
	*/
	static const Int MaxRingLines = 400000;
	Int want = (g_crcRingLines > 0) ? g_crcRingLines : (Int)MaxStrings;
	if (want > MaxRingLines)
	{
		DEBUG_LOG(("CRC ring: %d lines is too large for a 32-bit process; using %d",
			want, MaxRingLines));
		want = MaxRingLines;
	}
	if (DebugStrings != nullptr && RingLines >= want)
		return;
	char (*grown)[MaxStringLen] = (char (*)[MaxStringLen])
		malloc((size_t)want * MaxStringLen);
	if (grown == nullptr)
	{
		// Say so. A silent failure here means an empty capture later.
		DEBUG_LOG(("CRC ring: could not allocate %d lines (%d MB); "
			"keeping %d", want, (Int)((size_t)want * MaxStringLen / (1024*1024)),
			RingLines));
		return;
	}
	if (DebugStrings != nullptr)
		free(DebugStrings);
	DebugStrings = grown;
	RingLines = want;
	memset(DebugStrings, 0, (size_t)want * MaxStringLen);
}
/*	-CRCRingTestFrame N: pretend a mismatch happened at frame N.

	The ring only ever writes from Network::setSawCRCMismatch(), which cannot
	be provoked on demand -- so without this the write path can only be
	verified by playing a real multiplayer match and hoping. That is exactly
	how an empty-file bug reached a live test once. This exercises the same
	function against a real, busy buffer in a headless skirmish.
*/
Int g_crcRingTestFrame = 0;

struct FrameMark
{
	UnsignedInt	frame;
	Int			at;			///< index into the ring where this frame's lines start
};
static const Int MaxFrameMarks = 256;
static FrameMark FrameMarks[MaxFrameMarks];
static Int numFrameMarks = 0;
static Int nextFrameMark = 0;

static void noteFrameStart( UnsignedInt frame, Int at )
{
	FrameMarks[nextFrameMark].frame = frame;
	FrameMarks[nextFrameMark].at = at;
	nextFrameMark = (nextFrameMark + 1) % MaxFrameMarks;
	if (numFrameMarks < MaxFrameMarks)
		++numFrameMarks;
}
//static char DumpStrings[MaxStrings][MaxStringLen];
//static Int nextDumpString = 0;
//static Int numDumpStrings = 0;

#define IS_FRAME_OK_TO_LOG TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheDebugIgnoreSyncErrors && \
	TheCRCFirstFrameToLog >= 0 && TheCRCFirstFrameToLog <= TheGameLogic->getFrame() \
	&& TheGameLogic->getFrame() <= TheCRCLastFrameToLog

CRCVerification::CRCVerification()
{
#ifdef DEBUG_LOGGING
/**/
	if (g_verifyClientCRC && (IS_FRAME_OK_TO_LOG))
	{
		m_startCRC = TheGameLogic->getCRC(CRC_RECALC, (g_clientDeepCRC)?"clientPre.crc":"");
	}
	else
	{
		m_startCRC = 0;
	}
/**/
#endif
}

CRCVerification::~CRCVerification()
{
#ifdef DEBUG_LOGGING
/**/
	UnsignedInt endCRC = 0;
	if (g_verifyClientCRC && (IS_FRAME_OK_TO_LOG))
	{
		endCRC = TheGameLogic->getCRC(CRC_RECALC, (g_clientDeepCRC)?"clientPost.crc":"");
	}
	DEBUG_ASSERTCRASH(!TheGameLogic->isInGame() || m_startCRC == endCRC, ("GameLogic changed outside of GameLogic::update() on frame %d!", TheGameLogic->getFrame()));
	if (TheGameLogic->isInMultiplayerGame() && m_startCRC != endCRC)
	{
		if (TheInGameUI)
		{
			TheInGameUI->message(L"GameLogic changed outside of GameLogic::update() - call Matt (x36804)!");
		}
		CRCDEBUG_LOG(("GameLogic changed outside of GameLogic::update()!!!"));
	}
/**/
#endif
}

void outputCRCDebugLines()
{
	IPEnumeration ips;
	AsciiString fname;
	fname.format("crcDebug%s.txt", ips.getMachineName().str());
	FILE *fp = fopen(fname.str(), "wt");
	int start = 0;
	int end = nextDebugString;
	if (numDebugStrings >= RingLines)
		start = nextDebugString - RingLines;

	for (Int i=start; i<end; ++i)
	{
		const char *line = DebugStrings[ (i + RingLines) % RingLines ];
		DEBUG_LOG(("%s", line));
		if (fp) fprintf(fp, "%s\n", line);
	}

	if (fp) fclose(fp);
}

Int lastCRCDebugFrame = 0;
Int lastCRCDebugIndex = 0;
extern Bool inCRCGen;

void CRCDebugStartNewGame()
{
	if (TheGameLogic->isInShellGame())
		return;
	if (g_saveDebugCRCPerFrame)
	{
		// Create folder for frame data, if it doesn't exist yet.
		CreateDirectory(g_saveDebugCRCPerFrameDir.str(), nullptr);

		// Delete existing files
		FilenameList files;
		AsciiString dir = g_saveDebugCRCPerFrameDir;
		dir.concat("/");
		TheLocalFileSystem->getFileListInDirectory(dir.str(), "", "DebugFrame_*.txt", files, FALSE);
		FilenameList::iterator it;
		for (it = files.begin(); it != files.end(); ++it)
		{
			DeleteFile(it->str());
		}
	}
	nextDebugString = 0;
	numDebugStrings = 0;
	lastCRCDebugFrame = 0;
	lastCRCDebugIndex = 0;
}

static void outputCRCDebugLinesPerFrame()
{
	if (!g_saveDebugCRCPerFrame || numDebugStrings == 0 || DebugStrings == nullptr)
		return;
	AsciiString fname;
	fname.format("%s/DebugFrame_%06d.txt", g_saveDebugCRCPerFrameDir.str(), lastCRCDebugFrame);
	FILE *fp = fopen(fname.str(), "wt");
	int start = 0;
	int end = nextDebugString;
	if (numDebugStrings >= RingLines)
		start = nextDebugString - RingLines;
	nextDebugString = 0;
	numDebugStrings = 0;
	if (!fp)
		return;

	for (Int i=start; i<end; ++i)
	{
		const char *line = DebugStrings[ (i + RingLines) % RingLines ];
		//DEBUG_LOG(("%s", line));
		fprintf(fp, "%s\n", line);
	}

	fclose(fp);
}

/**
 * Write the last g_crcRingFrames frames to disk. Called when the network
 * reports a CRC mismatch, and only then.
 *
 * One file, not one per frame: the frames on either side of a divergence are
 * read together, and a single file is what crc-diff.py wants anyway.
 */
void outputCRCRing( const char *why )
{
	if (g_crcRingFrames <= 0 || numDebugStrings == 0 || numFrameMarks == 0
			|| DebugStrings == nullptr)
		return;

	// Walk back g_crcRingFrames marks to find where the window starts.
	const Int want = (g_crcRingFrames < numFrameMarks) ? g_crcRingFrames : numFrameMarks;
	const Int oldest = (nextFrameMark - want + MaxFrameMarks) % MaxFrameMarks;
	Int start = FrameMarks[oldest].at;
	const UnsignedInt fromFrame = FrameMarks[oldest].frame;

	/*	How many lines are we actually asking for, and are they still here?

		nextDebugString is a RING INDEX (0..MaxStrings-1, wraps), while
		numDebugStrings is a monotonic COUNT of every line ever written -- it
		reached 34 million in one match. Treating the count as a ring position
		is what made the first version of this write an empty file: it decided
		the ring had wrapped past the window and set start = nextDebugString,
		so the copy loop exited immediately.

		The honest test is the DISTANCE from the window start to the write
		head. If that is the whole ring, the window is older than anything we
		still hold and the most we can offer is the full buffer.
	*/
	Int avail = (nextDebugString - start + RingLines) % RingLines;
	if (avail == 0)
		avail = RingLines;		// start == head: a full ring, not an empty one
	if (numDebugStrings > RingLines && avail > RingLines)
		avail = RingLines;

	AsciiString fname;
	fname.format("%s/CRCRing_%06d.txt", g_saveDebugCRCPerFrameDir.str(),
		TheGameLogic ? TheGameLogic->getFrame() : 0);
	FILE *fp = fopen(fname.str(), "wt");
	if (!fp)
		return;

	fprintf(fp, "; %s\n", why ? why : "crc ring");
	fprintf(fp, "; frames %u..%u (last %d of %d marked), %d lines\n",
		fromFrame, TheGameLogic ? TheGameLogic->getFrame() : 0,
		want, numFrameMarks, avail);

	Int i = start;
	for (Int n = 0; n < avail; ++n)
	{
		fprintf(fp, "%s\n", DebugStrings[i]);
		i = (i + 1) % RingLines;
	}
	fclose(fp);

	DEBUG_LOG(("CRC ring: wrote %s (frames %u..%u)", fname.str(), fromFrame,
		TheGameLogic ? TheGameLogic->getFrame() : 0));
}

void outputCRCDumpLines()
{
	/*
	int start = 0;
	int end = nextDumpString;
	if (numDumpStrings >= MaxStrings)
		start = nextDumpString - MaxStrings;

	for (Int i=start; i<end; ++i)
	{
		const char *line = DumpStrings[ (i + MaxStrings) % MaxStrings ];
		DEBUG_LOG(("%s", line));
	}
	*/
}

static AsciiString getFname(AsciiString path)
{
	return path.reverseFind('\\') + 1;
}

static void addCRCDebugLineInternal(bool count, const char *fmt, va_list args)
{
	if (TheGameLogic == nullptr || !(IS_FRAME_OK_TO_LOG))
		return;

	if (lastCRCDebugFrame != TheGameLogic->getFrame())
	{
		// In ring mode the buffer is NOT flushed or emptied here -- that is
		// the whole point. Just remember where this frame's lines begin so
		// the mismatch handler can find the last few frames.
		if (g_crcRingFrames > 0)
		{
			noteFrameStart(TheGameLogic->getFrame(), nextDebugString);
			if (g_crcRingTestFrame > 0 &&
					(Int)TheGameLogic->getFrame() == g_crcRingTestFrame)
				outputCRCRing("TEST TRIGGER");
		}
		else
			outputCRCDebugLinesPerFrame();
		lastCRCDebugFrame = TheGameLogic->getFrame();
		lastCRCDebugIndex = 0;
	}

	ensureRing();
	if (DebugStrings == nullptr)
		return;

	if (count)
		sprintf(DebugStrings[nextDebugString], "%d:%05d ", TheGameLogic->getFrame(), lastCRCDebugIndex++);
	else
		DebugStrings[nextDebugString][0] = 0;
	Int len = strlen(DebugStrings[nextDebugString]);

	vsnprintf(DebugStrings[nextDebugString]+len, MaxStringLen-len, fmt, args);

	char *tmp = DebugStrings[nextDebugString];
	while (tmp && *tmp)
	{
		if (*tmp == '\r' || *tmp == '\n')
		{
			*tmp = ' ';
		}
		++tmp;
	}

	//DEBUG_LOG(("%s", DebugStrings[nextDebugString]));

	++nextDebugString;
	++numDebugStrings;
	if (nextDebugString >= RingLines)
		nextDebugString = 0;
}

void addCRCDebugLine(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    addCRCDebugLineInternal(true, fmt, args);
    va_end(args);
}

void addCRCDebugLineNoCounter(const char *fmt, ...)
{
	// TheSuperHackers @feature helmutbuhler 04/09/2025
	// This version doesn't increase the lastCRCDebugIndex counter
	// and can be used for logging lines that don't necessarily match up on all peers.
	// (Otherwise the numbers would no longer match up and the diff would be very difficult to read)
    va_list args;
    va_start(args, fmt);
    addCRCDebugLineInternal(false, fmt, args);
    va_end(args);
}

void addCRCGenLine(const char *fmt, ...)
{
	if (!(IS_FRAME_OK_TO_LOG))
		return;

	static char buf[MaxStringLen];
	va_list va;
	va_start( va, fmt );
	vsnprintf(buf, MaxStringLen, fmt, va );
	va_end( va );
	addCRCDebugLine("%s", buf);

	//DEBUG_LOG(("%s", buf));
}

void addCRCDumpLine(const char *fmt, ...)
{
	/*
	va_list va;
	va_start( va, fmt );
	vsnprintf(DumpStrings[nextDumpString], MaxStringLen, fmt, va );
	va_end( va );

	++nextDumpString;
	++numDumpStrings;
	if (nextDumpString == MaxStrings)
		nextDumpString = 0;
		*/
}

void dumpVector3(const Vector3 *v, AsciiString name, AsciiString fname, Int line)
{
	if (!(IS_FRAME_OK_TO_LOG)) return;
	fname.toLower();
	fname = getFname(fname);
	addCRCDebugLine("dumpVector3() %s:%d %s %8.8X %8.8X %8.8X",
		fname.str(), line, name.str(),
		AS_INT(v->X), AS_INT(v->Y), AS_INT(v->Z));
}

void dumpCoord3D(const Coord3D *c, AsciiString name, AsciiString fname, Int line)
{
	if (!(IS_FRAME_OK_TO_LOG)) return;
	fname.toLower();
	fname = getFname(fname);
	addCRCDebugLine("dumpCoord3D() %s:%d %s %8.8X %8.8X %8.8X",
		fname.str(), line, name.str(),
		AS_INT(c->x), AS_INT(c->y), AS_INT(c->z));
}

void dumpMatrix3D(const Matrix3D *m, AsciiString name, AsciiString fname, Int line)
{
	if (!(IS_FRAME_OK_TO_LOG)) return;
	fname.toLower();
	fname = getFname(fname);
	const Real *matrix = (const Real *)m;
	addCRCDebugLine("dumpMatrix3D() %s:%d %s",
		fname.str(), line, name.str());
	for (Int i=0; i<3; ++i)
		addCRCDebugLine("      0x%08X 0x%08X 0x%08X 0x%08X",
			AS_INT(matrix[(i<<2)+0]), AS_INT(matrix[(i<<2)+1]), AS_INT(matrix[(i<<2)+2]), AS_INT(matrix[(i<<2)+3]));
}

void dumpReal(Real r, AsciiString name, AsciiString fname, Int line)
{
	if (!(IS_FRAME_OK_TO_LOG)) return;
	fname.toLower();
	fname = getFname(fname);
	addCRCDebugLine("dumpReal() %s:%d %s %8.8X (%f)",
		fname.str(), line, name.str(), AS_INT(r), r);
}

#endif // DEBUG_CRC
