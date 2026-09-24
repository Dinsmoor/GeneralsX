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

// RandomValue.cpp
// Pseudo-random number generators
// Author: Michael S. Booth, January 1998

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Lib/BaseType.h"
#include "Common/RandomValue.h"
#include "Common/crc.h"
#include "Common/Debug.h"
#include "GameLogic/GameLogic.h"

/*	These were hand-edited comment toggles, so turning one on meant modifying
	tracked source on every machine in the match and remembering to revert it.
	Worse, the unconditional #undef block DISCARDED anything the build defined,
	so passing -DDEBUG_RANDOM_LOGIC looked like it worked and did nothing.

	Each is now settable from the build (cmake/config-debug.cmake:
	RTS_DEBUG_RANDOM_LOGIC and friends) and is only cleared when the build did
	NOT ask for it, which keeps the default byte-identical to before.

	DEBUG_RANDOM_LOGIC is the one a desync hunt wants: every logic draw logs
	its frame, value, range, FILE and LINE, so diffing two peers' logs names
	the exact call site that ran on one and not the other. It is extremely
	chatty -- combat is thousands of draws a second -- so it is a deliberate
	build, never a default. */
#ifndef DEBUG_RANDOM_AUDIO
#undef DEBUG_RANDOM_AUDIO
#endif
#ifndef DEBUG_RANDOM_CLIENT
#undef DEBUG_RANDOM_CLIENT
#endif
#ifndef DEBUG_RANDOM_LOGIC
#undef DEBUG_RANDOM_LOGIC
#endif
#ifndef DETERMINISTIC
#undef DETERMINISTIC
#endif

/*	Logic random-draw trace, for finding a lockstep desync.

	WHY THIS EXISTS SEPARATELY FROM DEBUG_RANDOM_LOGIC

	DEBUG_RANDOM_LOGIC logs through DEBUG_LOG, which needs RTS_DEBUG_LOGGING=ON,
	and that configuration does not currently COMPILE on Linux: enabling it
	turns on a chain of debug-only code that has only ever been built by 32-bit
	MSVC (__int64 in SimpleProfiler, a missing <string.h>, two pointer-to-int
	truncations, and Windows MEMORYSTATUS fields). Fixing all of that is a
	separate job from finding a desync.

	This path is deliberately tiny and independent: one FILE*, plain fprintf,
	no DEBUG_LOG, no subsystem. It therefore works in the RELEASE build every
	peer already runs, which also means the trace runs at release speed and
	needs no second binary per machine.

	Only the LOGIC generator is traced, because only the logic generator feeds
	the CRC that decides a mismatch.

	Enabled at runtime by setting GENERALS_RANDOM_TRACE to a path; unset, the
	cost is one pointer test per draw. The file is opened on first use and
	line-buffered, so a killed engine still leaves a usable file.

	    GENERALS_RANDOM_TRACE=/tmp/rng.log ./GeneralsXZH ...

	One line per draw, which is what makes two peers' traces diffable -- the
	first differing line names the call site that ran on one peer and not the
	other:

	    <frame> i <value> <lo> <hi> <file>:<line>     integer draw
	    <frame> r <float-bits-hex> <file>:<line>      real draw

	Compare with `diff`, or find the first difference with
	`cmp <(...) <(...)`. The Unchanged variants are deliberately NOT traced:
	they roll a copy of the seed and cannot move the CRC. */
static FILE *theRandomTraceFile = nullptr;
static Bool theRandomTraceChecked = FALSE;

static FILE *randomTraceFile( void )
{
	if (!theRandomTraceChecked)
	{
		theRandomTraceChecked = TRUE;
		const char *path = getenv("GENERALS_RANDOM_TRACE");
		if (path != nullptr && path[0] != '\0')
		{
			theRandomTraceFile = fopen(path, "w");
			if (theRandomTraceFile != nullptr)
				setvbuf(theRandomTraceFile, nullptr, _IOLBF, 0);
		}
	}
	return theRandomTraceFile;
}

/*	The frame, or -1 before TheGameLogic exists.

	populateRandomSideAndColor draws before frame 0, and a divergence THERE is
	just as interesting as one mid-match, so those draws are recorded rather
	than skipped. */
static Int randomTraceFrame( void )
{
	return (TheGameLogic != nullptr) ? (Int)TheGameLogic->getFrame() : -1;
}

/*	Basename only. The peers may have different source roots -- they are
	different machines, possibly different architectures -- and an absolute path
	would make every line differ for no reason. */
static const char *randomTraceBase( const char *file )
{
	const char *base = file;
	if (file != nullptr)
	{
		for (const char *p = file; *p != '\0'; ++p)
			if (*p == '/' || *p == '\\')
				base = p + 1;
	}
	return (base != nullptr) ? base : "?";
}

static void traceLogicDraw( Int rval, int lo, int hi, const char *file, int line )
{
	FILE *fp = randomTraceFile();
	if (fp == nullptr)
		return;

	fprintf(fp, "%d i %d %d %d %s:%d\n",
		randomTraceFrame(), rval, lo, hi, randomTraceBase(file), line);
}

static void traceLogicDrawReal( Real rval, const char *file, int line )
{
	FILE *fp = randomTraceFile();
	if (fp == nullptr)
		return;

	/*	Raw bits, not %f. Two peers must agree EXACTLY, and a decimal rendering
		can hide a difference in the low mantissa bits -- which is precisely the
		kind of difference worth seeing. */
	UnsignedInt bits = 0;
	memcpy(&bits, &rval, sizeof(bits));
	fprintf(fp, "%d r %08X %s:%d\n",
		randomTraceFrame(), bits, randomTraceBase(file), line);
}

static const Real theMultFactor = 1.0f / static_cast<float>(UINT_MAX);

// Initial seed values.
static UnsignedInt theGameAudioSeed[6] =
{
	0xf22d0e56L, 0x883126e9L, 0xc624dd2fL, 0x702c49cL, 0x9e353f7dL, 0x6fdf3b64L
};

static UnsignedInt theGameClientSeed[6] =
{
	0xf22d0e56L, 0x883126e9L, 0xc624dd2fL, 0x702c49cL, 0x9e353f7dL, 0x6fdf3b64L
};

static UnsignedInt theGameLogicSeed[6] =
{
	0xf22d0e56L, 0x883126e9L, 0xc624dd2fL, 0x702c49cL, 0x9e353f7dL, 0x6fdf3b64L
};

static UnsignedInt theGameLogicBaseSeed = 0;

UnsignedInt GetGameLogicRandomSeed()
{
	return theGameLogicBaseSeed;
}

UnsignedInt GetGameLogicRandomSeedCRC()
{
	CRC c;
	c.computeCRC(theGameLogicSeed, sizeof(theGameLogicSeed));
	return c.get();
}

static void seedRandom(UnsignedInt SEED, UnsignedInt (&seed)[6])
{
	UnsignedInt ax;

	ax = SEED;                      /* mov     eax,SEED                     */
	ax += 0xf22d0e56;               /* add     eax,0f22d0e56h               */
	seed[0] = ax;                   /* mov     seed,eax                     */
	ax += 0x883126e9 - 0xf22d0e56;  /* add     eax,0883126e9h-0f22d0e56h    */
	seed[1] = ax;                   /* mov     seed+4,eax                   */
	ax += 0xc624dd2f - 0x883126e9;  /* add     eax,0c624dd2fh-0883126e9h    */
	seed[2] = ax;                   /* mov     seed+8,eax                   */
	ax += 0x0702c49c - 0xc624dd2f;  /* add     eax,00702c49ch-0c624dd2fh    */
	seed[3] = ax;                   /* mov     seed+12,eax                  */
	ax += 0x9e353f7d - 0x0702c49c;  /* add     eax,09e353f7dh-00702c49ch    */
	seed[4] = ax;                   /* mov     seed+16,eax                  */
	ax += 0x6fdf3b64 - 0x9e353f7d;  /* add     eax,06fdf3b64h-09e353f7dh    */
	seed[5] = ax;                   /* mov     seed+20,eax                  */
}

void InitRandom()
{
#ifdef DETERMINISTIC
	// needs to be the same every time
	seedRandom(0, theGameAudioSeed);
	seedRandom(0, theGameClientSeed);
	seedRandom(0, theGameLogicSeed);
	theGameLogicBaseSeed = 0;
#else
	const time_t seconds = time( nullptr );

	seedRandom(seconds, theGameAudioSeed);
	seedRandom(seconds, theGameClientSeed);
	seedRandom(seconds, theGameLogicSeed);
	theGameLogicBaseSeed = seconds;
#endif
}

void InitRandom( UnsignedInt seed )
{
#ifdef DETERMINISTIC
	seed = 0;
#endif

	seedRandom(seed, theGameAudioSeed);
	seedRandom(seed, theGameClientSeed);
	seedRandom(seed, theGameLogicSeed);
	theGameLogicBaseSeed = seed;

#ifdef DEBUG_RANDOM_LOGIC
	DEBUG_LOG(("InitRandom %08lx", seed));
#endif
}

// Add with carry. SUM is replaced with A + B + C, C is replaced with 1 if there was a carry, 0 if there wasn't.
// A carry occurred if the sum is less than one of the inputs. This is addition, so carry can never be more than one.
#define ADC(SUM, A, B, C) SUM = (A) + (B) + (C); C = ((SUM < (A)) || (SUM < (B)))

static UnsignedInt randomValue(UnsignedInt (&seed)[6])
{
	UnsignedInt ax;
	UnsignedInt c = 0;

	ADC(ax, seed[5], seed[4], c);   /*  mov     ax,seed+20  */
	/*  add     ax,seed+16  */
	seed[4] = ax;                   /*  mov     seed+8,ax   */

	ADC(ax, ax, seed[3], c);        /*  adc     ax,seed+12  */
	seed[3] = ax;                   /*  mov     seed+12,ax  */

	ADC(ax, ax, seed[2], c);        /*  adc     ax,seed+8   */
	seed[2] = ax;                   /*  mov     seed+8,ax   */

	ADC(ax, ax, seed[1], c);        /*  adc     ax,seed+4   */
	seed[1] = ax;                   /*  mov     seed+4,ax   */

	ADC(ax, ax, seed[0], c);        /*  adc     ax,seed+0   */
	seed[0] = ax;                   /*  mov     seed+0,ax   */

	/* Increment seed array, bubbling up the carries. */
	if (!++seed[5])
	{
		if (!++seed[4])
		{
			if (!++seed[3])
			{
				if (!++seed[2])
				{
					if (!++seed[1])
					{
						++seed[0];
						++ax;
					}
				}
			}
		}
	}

	return ax;
}

//
// It is necessary to separate the GameClient and GameLogic usage of random
// values to ensure that the GameLogic remains deterministic, regardless
// of the effects displayed on the GameClient.
//

//
// Integer random value
//
Int GetGameAudioRandomValue( int lo, int hi, const char *file, int line )
{
	if (lo >= hi)
		return hi;

	const UnsignedInt delta = hi - lo + 1;
	const Int rval = ((Int)(randomValue(theGameAudioSeed) % delta)) + lo;

#ifdef DEBUG_RANDOM_AUDIO
	DEBUG_LOG(( "%d: GetGameAudioRandomValue = %d (%d - %d), %s line %d",
		TheGameLogic->getFrame(), rval, lo, hi, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// Real valued random value
//
Real GetGameAudioRandomValueReal( Real lo, Real hi, const char *file, int line )
{
	if (lo >= hi)
		return hi;

	const Real delta = hi - lo;
	const Real rval = ((Real)(randomValue(theGameAudioSeed)) * theMultFactor) * delta + lo;

#ifdef DEBUG_RANDOM_AUDIO
	DEBUG_LOG(( "%d: GetGameAudioRandomValueReal = %f, %s line %d",
		TheGameLogic->getFrame(), rval, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// Integer random value
//
Int GetGameClientRandomValue( int lo, int hi, const char *file, int line )
{
	if (lo >= hi)
		return hi;

	const UnsignedInt delta = hi - lo + 1;
	const Int rval = ((Int)(randomValue(theGameClientSeed) % delta)) + lo;

#ifdef DEBUG_RANDOM_CLIENT
	DEBUG_LOG(( "%d: GetGameClientRandomValue = %d (%d - %d), %s line %d",
		TheGameLogic ? TheGameLogic->getFrame() : -1, rval, lo, hi, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// Real valued random value
//
Real GetGameClientRandomValueReal( Real lo, Real hi, const char *file, int line )
{
	if (lo >= hi)
		return hi;

	const Real delta = hi - lo;
	const Real rval = ((Real)(randomValue(theGameClientSeed)) * theMultFactor) * delta + lo;

#ifdef DEBUG_RANDOM_CLIENT
	DEBUG_LOG(( "%d: GetGameClientRandomValueReal = %f, %s line %d",
		TheGameLogic->getFrame(), rval, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// Integer random value
//
Int GetGameLogicRandomValue( int lo, int hi, const char *file, int line )
{
#if RETAIL_COMPATIBLE_CRC
	const UnsignedInt delta = hi - lo + 1;
	if (delta == 0)
		return hi;
#else
	if (lo >= hi)
		return hi;

	const UnsignedInt delta = hi - lo + 1;
#endif

	const Int rval = ((Int)(randomValue(theGameLogicSeed) % delta)) + lo;

	traceLogicDraw(rval, lo, hi, file, line);

#ifdef DEBUG_RANDOM_LOGIC
	DEBUG_LOG(( "%d: GetGameLogicRandomValue = %d (%d - %d), %s line %d",
		TheGameLogic->getFrame(), rval, lo, hi, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// Real valued random value
//
Real GetGameLogicRandomValueReal( Real lo, Real hi, const char *file, int line )
{
#if RETAIL_COMPATIBLE_CRC
	const Real delta = hi - lo;
	if (delta <= 0.0f)
		return hi;
#else
	if (lo >= hi)
		return hi;

	const Real delta = hi - lo;
#endif

	const Real rval = ((Real)(randomValue(theGameLogicSeed)) * theMultFactor) * delta + lo;

	traceLogicDrawReal(rval, file, line);

#ifdef DEBUG_RANDOM_LOGIC
	DEBUG_LOG(( "%d: GetGameLogicRandomValueReal = %f, %s line %d",
		TheGameLogic->getFrame(), rval, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// TheSuperHackers @info This function does not change the seed values with retail compatibility disabled.
// Consecutive calls always return the same value for the same combination of min / max values, assuming the seed values haven't changed in between.
// The intended use case for this function are randomized values that are desirable to be synchronized across clients,
// but should not result in a mismatch if they aren't synchronized; e.g. for scripted audio events.
//
Int GetGameLogicRandomValueUnchanged( int lo, int hi, const char *file, int line )
{
#if RETAIL_COMPATIBLE_CRC
	return GetGameLogicRandomValue(lo, hi, file, line);
#endif

	if (lo >= hi)
		return hi;

	UnsignedInt seed[ARRAY_SIZE(theGameLogicSeed)];
	memcpy(&seed[0], &theGameLogicSeed[0], sizeof(seed));

	const UnsignedInt delta = hi - lo + 1;
	const Int rval = ((Int)(randomValue(seed) % delta)) + lo;

#ifdef DEBUG_RANDOM_LOGIC
	DEBUG_LOG(( "%d: GetGameLogicRandomValueUnchanged = %d (%d - %d), %s line %d",
		TheGameLogic->getFrame(), rval, lo, hi, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//
// TheSuperHackers @info This function does not change the seed values with retail compatibility disabled.
// Consecutive calls always return the same value for the same combination of min / max values, assuming the seed values haven't changed in between.
// The intended use case for this function are randomized values that are desirable to be synchronized across clients,
// but should not result in a mismatch if they aren't synchronized; e.g. for scripted audio events.
//
Real GetGameLogicRandomValueRealUnchanged( Real lo, Real hi, const char *file, int line )
{
#if RETAIL_COMPATIBLE_CRC
	return GetGameLogicRandomValueReal(lo, hi, file, line);
#endif

	if (lo >= hi)
		return hi;

	UnsignedInt seed[ARRAY_SIZE(theGameLogicSeed)];
	memcpy(&seed[0], &theGameLogicSeed[0], sizeof(seed));

	const Real delta = hi - lo;
	const Real rval = ((Real)(randomValue(seed)) * theMultFactor) * delta + lo;

#ifdef DEBUG_RANDOM_LOGIC
	DEBUG_LOG(( "%d: GetGameLogicRandomValueRealUnchanged = %f, %s line %d",
		TheGameLogic->getFrame(), rval, file, line ));
#endif

	DEBUG_ASSERTCRASH(rval >= lo && rval <= hi, ("Bad random val"));
	return rval;
}

//--------------------------------------------------------------------------------------------------------------
// GameClientRandomVariable
//

const char *const GameClientRandomVariable::DistributionTypeNames[] =
{
	"CONSTANT", "UNIFORM", "GAUSSIAN", "TRIANGULAR", "LOW_BIAS", "HIGH_BIAS", nullptr
};
static_assert(ARRAY_SIZE(GameClientRandomVariable::DistributionTypeNames) == GameClientRandomVariable::DISTRIBUTION_COUNT + 1, "Incorrect array size");

/**
	define the range of random values, and the distribution of values
*/
void GameClientRandomVariable::setRange( Real low, Real high, DistributionType type )
{
	DEBUG_ASSERTCRASH(!(m_type == CONSTANT && m_low != m_high), ("CONSTANT GameClientRandomVariables should have low == high"));
	m_low = low;
	m_high = high;
	m_type = type;
}

/**
 * Return a value from the random distribution
 */
Real GameClientRandomVariable::getValue() const
{
	switch( m_type )
	{
		case CONSTANT:
			DEBUG_ASSERTLOG(m_low == m_high, ("m_low != m_high for a CONSTANT GameClientRandomVariable"));
			if (m_low == m_high) {
				return m_low;
			}
			FALLTHROUGH;

		case UNIFORM:
			return GameClientRandomValueReal( m_low, m_high );

		default:
			/// @todo fill in support for nonuniform GameClientRandomVariables.
			DEBUG_CRASH(("unsupported DistributionType in GameClientRandomVariable::getValue"));
			return 0.0f;
	}
}

//--------------------------------------------------------------------------------------------------------------
// GameLogicRandomVariable
//

const char *const GameLogicRandomVariable::DistributionTypeNames[] =
{
	"CONSTANT", "UNIFORM", "GAUSSIAN", "TRIANGULAR", "LOW_BIAS", "HIGH_BIAS", nullptr
};
static_assert(ARRAY_SIZE(GameLogicRandomVariable::DistributionTypeNames) == GameLogicRandomVariable::DISTRIBUTION_COUNT + 1, "Incorrect array size");

/**
	define the range of random values, and the distribution of values
*/
void GameLogicRandomVariable::setRange( Real low, Real high, DistributionType type )
{
	DEBUG_ASSERTCRASH(!(m_type == CONSTANT && m_low != m_high), ("CONSTANT GameLogicRandomVariables should have low == high"));
	m_low = low;
	m_high = high;
	m_type = type;
}

/**
 * Return a value from the random distribution
 */
Real GameLogicRandomVariable::getValue() const
{
	switch( m_type )
	{
		case CONSTANT:
			DEBUG_ASSERTLOG(m_low == m_high, ("m_low != m_high for a CONSTANT GameLogicRandomVariable"));
			if (m_low == m_high) {
				return m_low;
			}
			FALLTHROUGH;

		case UNIFORM:
			return GameLogicRandomValueReal( m_low, m_high );

		default:
			/// @todo fill in support for nonuniform GameLogicRandomVariables.
			DEBUG_CRASH(("unsupported DistributionType in GameLogicRandomVariable::getValue"));
			return 0.0f;
	}
}


//--------------------------------------------------------------------------------------------------------------
Int LogicRandomValueClass::GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const
{
	return GetGameLogicRandomValue(lo, hi, file, line);
}

Real LogicRandomValueClass::GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const
{
	return GetGameLogicRandomValueReal(lo, hi, file, line);
}

Int ClientRandomValueClass::GetRandomValueInt( Int lo, Int hi, const char *file, Int line ) const
{
	return GetGameClientRandomValue(lo, hi, file, line);
}

Real ClientRandomValueClass::GetRandomValueReal( Real lo, Real hi, const char *file, Int line ) const
{
	return GetGameClientRandomValueReal(lo, hi, file, line);
}
