/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

#pragma once

#include <stdio.h>

#if defined(_MSC_VER) && _MSC_VER < 1300

#include <stdarg.h>

// TheSuperHackers @build STLport's <cstdio> defines an identical global
// inline vsnprintf (stlport/cstdio:49, under _STLP_MSVC) with no guard of its
// own, so a translation unit that includes both fails with
// "error C2084: function vsnprintf already has a body". That pairing only
// happens in builds where DEBUG_LOGGING pulls <cstdio> into the WWVegas
// libraries -- which is exactly the configuration needed for DEBUG_CRC and
// the multiplayer desync dump, so those 25 files could not be built at all.
// _STLP_CSTDIO is <cstdio>'s own include guard: if it is set, STLport has
// already supplied the function and ours must stand down. STLport's version
// forwards straight to _vsnprintf and so does NOT null-terminate when the
// output exactly fills the buffer; that difference is the reason this adapter
// exists. Under STLport that fix is therefore not in effect -- callers here
// must not rely on a null terminator when the output exactly fills the buffer.
//
// Include STLport's <cstdio> up front so the ordering is decided here rather
// than by whichever translation unit got there first. Without this the guard
// below only worked one way round: a file that reached <cstdio> first
// compiled, and one that reached this header first (e.g. FTP.cpp via
// WWDownload/ftp.h) made STLport's copy at cstdio:50 the redefinition
// instead. Under USING_STLPORT the build's include path puts STLport ahead of
// the VC98 headers, so this resolves to STLport's.
#if defined(USING_STLPORT)
#include <cstdio>
#endif

#if !defined(_STLP_CSTDIO)

inline int vsnprintf(char* _Buffer, size_t _BufferCount, const char* _Format, va_list _ArgList)
{
	if (_BufferCount == 0)
		return -1;
	// Microsoft's _vsnprintf does not null terminate when writing the entire length.
	int result = _vsnprintf(_Buffer, _BufferCount, _Format, _ArgList);
	// Deal with errors and edge cases.
	if (result == -1 || (size_t)result == _BufferCount)
	{
		_Buffer[_BufferCount - 1] = '\0';
		return -1;
	}
	return result;
}

#endif // !_STLP_CSTDIO

// Yes, this is called vswprintf instead of vsnwprintf
inline int vswprintf(wchar_t* _Buffer, size_t _BufferCount, const wchar_t* _Format, va_list _ArgList)
{
	if (_BufferCount == 0)
		return -1;
	// Microsoft's _vsnwprintf does not null terminate when writing the entire length.
	int result = _vsnwprintf(_Buffer, _BufferCount, _Format, _ArgList);
	// Deal with errors and edge cases.
	if (result == -1 || (size_t)result == _BufferCount)
	{
		_Buffer[_BufferCount - 1] = L'\0';
		return -1;
	}
	return result;
}

inline int snprintf(char* _Buffer, size_t _BufferCount, const char* _Format, ...)
{
	va_list _ArgList;
	va_start(_ArgList, _Format);
	int result = vsnprintf(_Buffer, _BufferCount, _Format, _ArgList);
	va_end(_ArgList);
	return result;
}

// Yes, this is called swprintf instead of snwprintf
inline int swprintf(wchar_t* _Buffer, size_t _BufferCount, const wchar_t* _Format, ...)
{
	va_list _ArgList;
	va_start(_ArgList, _Format);
	int result = vswprintf(_Buffer, _BufferCount, _Format, _ArgList);
	va_end(_ArgList);
	return result;
}

#endif
