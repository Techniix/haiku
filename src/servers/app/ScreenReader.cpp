/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ScreenReader.h"

#include <math.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <AutoLocker.h>
#include <Locker.h>
#include <ObjectList.h>
#include <OS.h>

#include "Desktop.h"
#include "DrawingEngine.h"
#include "HWInterface.h"
#include "RenderingBuffer.h"
#include "ServerBitmap.h"
#include "ServerCursor.h"
#include "ServerWindow.h"
#include "Window.h"
#include "WindowPrivate.h"


namespace {

// How long after the last capture BeginUpdate() keeps saving update
// regions. A recorder takes frames at least this often; a single
// ReadBitmap() makes the next few seconds' updates a little more
// expensive (one copy of the update region in main memory each).
const bigtime_t kCaptureActiveTime = 3000000;

int64 sLastCapture = -kCaptureActiveTime;


/*!	The last completed frame of a window's update region, saved when its
	update session began.
*/
struct UpdateSnapshot {
	Window*		window;
	BRegion		region;			// screen coordinates
	BPoint		windowPosition;	// the window's frame then
	clipping_rect bounds;		// of region, the pixels' origin
	uint32		bytesPerRow;
	uint8*		bits;			// B_RGB32

	UpdateSnapshot()
		:
		window(NULL),
		bits(NULL)
	{
	}

	~UpdateSnapshot()
	{
		free(bits);
	}
};

BLocker sSnapshotLock("update snapshots");
BObjectList<UpdateSnapshot, true> sSnapshots(4);


UpdateSnapshot*
find_snapshot(Window* window)
{
	for (int32 i = 0; i < sSnapshots.CountItems(); i++) {
		if (sSnapshots.ItemAt(i)->window == window)
			return sSnapshots.ItemAt(i);
	}
	return NULL;
}


/*!	Copies \a rect (screen coordinates) from \a bits (the screen at
	\a bitsOrigin, \a width x \a height pixels) into \a bitmap, \a origin
	going to the bitmap's (0, 0), clipped to the bitmap. Returns the number
	of pixels copied.
*/
int64
copy_rect(ServerBitmap* bitmap, BPoint origin, clipping_rect rect,
	const uint8* bits, uint32 bytesPerRow, color_space space,
	BPoint bitsOrigin, int32 width, int32 height)
{
	// in the bitmap's coordinates
	int32 left = rect.left - (int32)origin.x;
	int32 top = rect.top - (int32)origin.y;
	int32 right = rect.right - (int32)origin.x;
	int32 bottom = rect.bottom - (int32)origin.y;
	left = max_c(left, 0);
	top = max_c(top, 0);
	right = min_c(right, (int32)bitmap->Width() - 1);
	bottom = min_c(bottom, (int32)bitmap->Height() - 1);
	// and in the source's
	int32 sourceLeft = left + (int32)origin.x - (int32)bitsOrigin.x;
	int32 sourceTop = top + (int32)origin.y - (int32)bitsOrigin.y;
	if (sourceLeft < 0) {
		left -= sourceLeft;
		sourceLeft = 0;
	}
	if (sourceTop < 0) {
		top -= sourceTop;
		sourceTop = 0;
	}
	right = min_c(right, left + width - 1 - sourceLeft);
	bottom = min_c(bottom, top + height - 1 - sourceTop);
	if (right < left || bottom < top)
		return 0;

	if (bitmap->ImportBits(bits, bytesPerRow * height, bytesPerRow, space,
			BPoint(sourceLeft, sourceTop), BPoint(left, top),
			right - left + 1, bottom - top + 1) != B_OK)
		return 0;
	return (int64)(right - left + 1) * (bottom - top + 1);
}


int64
copy_region(ServerBitmap* bitmap, BPoint origin, const BRegion& region,
	RenderingBuffer* buffer, color_space space)
{
	int64 pixels = 0;
	for (int32 i = 0; i < region.CountRects(); i++) {
		pixels += copy_rect(bitmap, origin, region.RectAtInt(i),
			(const uint8*)buffer->Bits(), buffer->BytesPerRow(), space,
			B_ORIGIN, buffer->Width(), buffer->Height());
	}
	return pixels;
}


/*!	Composites the cursor and drag bitmap (HWInterface's combined image)
	into \a bitmap exactly as the software cursor draws it
	(HWInterface::_DrawCursor(): the image is premultiplied, the screen
	opaque). Returns its frame in screen coordinates, invalid if not drawn.
*/
BRect
draw_cursor(HWInterface* interface, ServerBitmap* bitmap, BPoint origin)
{
	if (bitmap->ColorSpace() != B_RGB32 && bitmap->ColorSpace() != B_RGBA32)
		return BRect();
	if (!interface->IsCursorVisible())
		return BRect();
	ServerCursorReference reference = interface->CursorAndDragBitmap();
	ServerCursor* cursor = reference.Get();
	if (cursor == NULL || cursor->Bits() == NULL)
		return BRect();

	BPoint position = interface->CursorPosition() - cursor->GetHotSpot();
	BRect frame(floorf(position.x), floorf(position.y),
		floorf(position.x) + cursor->Width() - 1,
		floorf(position.y) + cursor->Height() - 1);

	int32 left = max_c((int32)frame.left, (int32)origin.x);
	int32 top = max_c((int32)frame.top, (int32)origin.y);
	int32 right = min_c((int32)frame.right,
		(int32)origin.x + (int32)bitmap->Width() - 1);
	int32 bottom = min_c((int32)frame.bottom,
		(int32)origin.y + (int32)bitmap->Height() - 1);
	if (right < left || bottom < top)
		return BRect();

	uint8* bits = bitmap->Bits();
	int32 bpr = bitmap->BytesPerRow();
	const uint8* cursorBits = cursor->Bits();
	int32 cursorBPR = cursor->BytesPerRow();
	for (int32 y = top; y <= bottom; y++) {
		uint8* d = bits + (y - (int32)origin.y) * bpr
			+ (left - (int32)origin.x) * 4;
		const uint8* c = cursorBits + (y - (int32)frame.top) * cursorBPR
			+ (left - (int32)frame.left) * 4;
		for (int32 x = left; x <= right; x++, d += 4, c += 4) {
			int a = 255 - c[3];
			d[0] = ((int)(d[0] * a + 255) >> 8) + c[0];
			d[1] = ((int)(d[1] * a + 255) >> 8) + c[1];
			d[2] = ((int)(d[2] * a + 255) >> 8) + c[2];
		}
	}
	return BRect(left, top, right, bottom);
}


// Totals for tests and diagnostics (AS_TEST_SCREEN_READ)
BLocker sTotalsLock("screen read totals");
screen_read_stats sTotals;
bool sLegacy = false;
int32 sSnapshotCount = 0;

}	// namespace


namespace ScreenReader {


void
NoteCapture()
{
	atomic_set64(&sLastCapture, system_time());
}


bool
CaptureActive()
{
	return system_time() - atomic_get64(&sLastCapture) < kCaptureActiveTime;
}


void
SaveUpdateSnapshot(Desktop* desktop, Window* window, const BRegion& region)
{
	HWInterface* interface = desktop->HWInterface();
	if (interface == NULL || !interface->IsDoubleBuffered()
		|| region.CountRects() == 0)
		return;
	RenderingBuffer* buffer = interface->BackBuffer();
	if (buffer == NULL)
		return;

	UpdateSnapshot* snapshot = new(std::nothrow) UpdateSnapshot;
	if (snapshot == NULL)
		return;
	snapshot->window = window;
	snapshot->region = region;
	snapshot->windowPosition = window->Frame().LeftTop();
	clipping_rect bounds = region.FrameInt();
	bounds.left = max_c(bounds.left, 0);
	bounds.top = max_c(bounds.top, 0);
	bounds.right = min_c(bounds.right, (int32)buffer->Width() - 1);
	bounds.bottom = min_c(bounds.bottom, (int32)buffer->Height() - 1);
	if (bounds.right < bounds.left || bounds.bottom < bounds.top) {
		delete snapshot;
		return;
	}
	snapshot->bounds = bounds;
	int32 width = bounds.right - bounds.left + 1;
	int32 height = bounds.bottom - bounds.top + 1;
	snapshot->bytesPerRow = width * 4;
	snapshot->bits = (uint8*)malloc((size_t)snapshot->bytesPerRow * height);
	if (snapshot->bits == NULL) {
		delete snapshot;
		return;
	}

	// Only the region's rectangles; the rest of the bounds is never read.
	const uint8* source = (const uint8*)buffer->Bits();
	uint32 sourceBPR = buffer->BytesPerRow();
	for (int32 i = 0; i < region.CountRects(); i++) {
		clipping_rect rect = region.RectAtInt(i);
		rect.left = max_c(rect.left, bounds.left);
		rect.top = max_c(rect.top, bounds.top);
		rect.right = min_c(rect.right, bounds.right);
		rect.bottom = min_c(rect.bottom, bounds.bottom);
		if (rect.right < rect.left || rect.bottom < rect.top)
			continue;
		for (int32 y = rect.top; y <= rect.bottom; y++) {
			memcpy(snapshot->bits + (y - bounds.top) * snapshot->bytesPerRow
					+ (rect.left - bounds.left) * 4,
				source + y * sourceBPR + rect.left * 4,
				(rect.right - rect.left + 1) * 4);
		}
	}

	AutoLocker<BLocker> locker(sSnapshotLock);
	UpdateSnapshot* old = find_snapshot(window);
	if (old != NULL)
		sSnapshots.RemoveItem(old, true);
	sSnapshots.AddItem(snapshot);
	atomic_set(&sSnapshotCount, sSnapshots.CountItems());
}


void
DropUpdateSnapshot(Window* window)
{
	if (atomic_get(&sSnapshotCount) == 0)
		return;
	AutoLocker<BLocker> locker(sSnapshotLock);
	UpdateSnapshot* snapshot = find_snapshot(window);
	if (snapshot != NULL)
		sSnapshots.RemoveItem(snapshot, true);
	atomic_set(&sSnapshotCount, sSnapshots.CountItems());
}


status_t
Read(Desktop* desktop, ServerBitmap* bitmap, const BRegion& area,
	BPoint origin, bool drawCursor, bool exclusive, BRect* _cursorFrame,
	Stats* stats)
{
	Stats localStats;
	if (stats == NULL)
		stats = &localStats;
	memset(stats, 0, sizeof(Stats));
	if (_cursorFrame != NULL)
		*_cursorFrame = BRect();

	NoteCapture();

	HWInterface* interface = desktop->HWInterface();
	DrawingEngine* engine = desktop->GetDrawingEngine();
	bigtime_t start = system_time();

	if (!desktop->LockSingleWindow())
		return B_ERROR;
	bool locked = exclusive ? engine->LockExclusiveAccess()
		: engine->LockParallelAccess();
	if (!locked) {
		desktop->UnlockSingleWindow();
		return B_ERROR;
	}
	bigtime_t lockedTime = system_time();
	stats->lockWait = lockedTime - start;

	RenderingBuffer* front = interface->FrontBuffer();
	RenderingBuffer* back = interface->IsDoubleBuffered()
		? interface->BackBuffer() : NULL;
	stats->frontBufferOnly = back == NULL || desktop->IsDirectScreenLocked();

	status_t status = B_OK;
	if (front == NULL) {
		status = B_ERROR;
	} else if (stats->frontBufferOnly) {
		// Single buffered, or a BWindowScreen owns the screen: the front
		// buffer is all there is. As DrawingEngine::ReadBitmap() did, with
		// a software cursor hidden for the copy.
		BRegion region(area);
		bool hidden = interface->HideFloatingOverlays(region.Frame());
		stats->frontPixels = copy_region(bitmap, origin, region, front,
			front->ColorSpace());
		if (hidden)
			interface->ShowFloatingOverlays();
	} else {
		// The back buffer for everything. Its alpha bytes are not
		// meaningful (as for _CopyToFront()): read it as B_RGB32.
		stats->backPixels = copy_region(bitmap, origin, area, back, B_RGB32);

		int32 workspace = desktop->CurrentWorkspace();
		for (Window* window = desktop->CurrentWindows().FirstWindow();
				window != NULL; window = window->NextWindow(workspace)) {
			if (!window->IsVisible())
				continue;
			// Content that only the front buffer has: direct windows
			// while connected, and BWindowScreen's window.
			bool direct = window->ServerWindow() != NULL
				&& window->ServerWindow()->IsDirectlyAccessing();
			if (!direct && (window->Flags() & kWindowScreenFlag) == 0)
				continue;
			BRegion region(window->VisibleRegion());
			BRegion content(window->Frame());
			region.IntersectWith(&content);
			region.IntersectWith(&area);
			stats->frontPixels += copy_region(bitmap, origin, region, front,
				front->ColorSpace());
		}

		// Windows in an update session: their last completed frame.
		AutoLocker<BLocker> locker(sSnapshotLock);
		for (int32 i = 0; i < sSnapshots.CountItems(); i++) {
			UpdateSnapshot* snapshot = sSnapshots.ItemAt(i);
			Window* window = snapshot->window;
			if (!window->IsVisible()
				|| window->Frame().LeftTop() != snapshot->windowPosition
				|| !desktop->CurrentWindows().HasWindow(window))
				continue;
			BRegion region(snapshot->region);
			region.IntersectWith(&window->VisibleRegion());
			region.IntersectWith(&area);
			for (int32 j = 0; j < region.CountRects(); j++) {
				stats->snapshotPixels += copy_rect(bitmap, origin,
					region.RectAtInt(j), snapshot->bits,
					snapshot->bytesPerRow, B_RGB32,
					BPoint(snapshot->bounds.left, snapshot->bounds.top),
					snapshot->bounds.right - snapshot->bounds.left + 1,
					snapshot->bounds.bottom - snapshot->bounds.top + 1);
			}
		}
	}

	if (exclusive)
		engine->UnlockExclusiveAccess();
	else
		engine->UnlockParallelAccess();
	desktop->UnlockSingleWindow();
	stats->lockHeld = system_time() - lockedTime;

	// The cursor last, without the locks: it is in neither buffer we read
	// (or was hidden for the copy).
	if (status == B_OK && drawCursor) {
		BRect frame = draw_cursor(interface, bitmap, origin);
		if (_cursorFrame != NULL)
			*_cursorFrame = frame;
	}

	AddToTotals(system_time() - start, stats->lockHeld, stats->backPixels,
		stats->frontPixels, stats->snapshotPixels);
	return status;
}


void
AddToTotals(bigtime_t time, bigtime_t lockHeld, int64 backPixels,
	int64 frontPixels, int64 snapshotPixels)
{
	AutoLocker<BLocker> locker(sTotalsLock);
	sTotals.reads++;
	sTotals.read_time_total += time;
	sTotals.read_time_max = max_c(sTotals.read_time_max, time);
	sTotals.lock_held_total += lockHeld;
	sTotals.lock_held_max = max_c(sTotals.lock_held_max, lockHeld);
	sTotals.back_pixels += backPixels;
	sTotals.front_pixels += frontPixels;
	sTotals.snapshot_pixels += snapshotPixels;
}


void
GetTotals(screen_read_stats& totals, bool reset)
{
	AutoLocker<BLocker> locker(sTotalsLock);
	totals = sTotals;
	totals.legacy = sLegacy;
	totals.update_snapshots = atomic_get(&sSnapshotCount);
	if (reset) {
		bool legacy = sLegacy;
		memset(&sTotals, 0, sizeof(sTotals));
		sLegacy = legacy;
	}
}


bool
LegacyReads()
{
	return sLegacy;
}


void
SetLegacyReads(bool legacy)
{
	sLegacy = legacy;
}


}	// namespace ScreenReader
