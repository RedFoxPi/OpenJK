/*
===========================================================================
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#pragma once

#include "../qcommon/q_shared.h"

class IHeapAllocator
{
public:
	virtual ~IHeapAllocator() {}

	virtual void ResetHeap() = 0;
	virtual char *MiniHeapAlloc ( int size ) = 0;
};

class CMiniHeap : public IHeapAllocator
{
private:
	char	*mHeap;
	char	*mCurrentHeap;
	int		mSize;
public:

	// reset the heap back to the start
	void ResetHeap()
	{
		mCurrentHeap = mHeap;
	}

	// initialise the heap
	CMiniHeap (int size)
	{
		mHeap = (char *)malloc(size);
		mSize = size;
		if (mHeap)
		{
			ResetHeap();
		}
	}

	// free up the heap
	~CMiniHeap()
	{
		if (mHeap)
		{
			free(mHeap);
		}
	}

	// give me some space from the heap please
	char *MiniHeapAlloc(int size)
	{
		// keep every allocation address aligned to at least pointer size - this
		// is a plain bump allocator with no alignment tracking, so a sequence
		// of odd-sized allocations (e.g. surface->numVerts * 5 * 4 in
		// G2_misc.cpp) could otherwise hand out a misaligned address, and
		// callers do store pointer-sized (intptr_t) values into what they get
		// back.
		const size_t alignment = sizeof(void *);
		char *aligned = (char *)(((size_t)mCurrentHeap + alignment - 1) & ~(alignment - 1));
		const size_t padding = (size_t)(aligned - mCurrentHeap);

		if ((size_t)size + padding < (mSize - ((size_t)mCurrentHeap - (size_t)mHeap)))
		{
			mCurrentHeap = aligned + size;
			return aligned;
		}
		return NULL;
	}

};

// this is in the parent executable, so access ri->GetG2VertSpaceServer() from the rd backends!
extern IHeapAllocator *G2VertSpaceServer;
extern IHeapAllocator *G2VertSpaceClient;
