/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SCREEN_READER_H
#define SCREEN_READER_H


#include <Rect.h>
#include <Region.h>

#include <ScreenReadInfo.h>


class Desktop;
class ServerBitmap;
class Window;


/*!	Reading what is on screen without the front buffer (Renga, roadmap
	F-59), for BScreen::ReadBitmap() (AS_READ_BITMAP) and BScreenCapture
	(ScreenCaptureManager).

	The front buffer is video memory, mapped write-combined or uncached:
	reading it is an order of magnitude slower than reading main memory
	(about 25-30 ms for a 2560x1080 frame on an RTX 5070), and
	DrawingEngine::ReadBitmap() did so under the drawing engine's exclusive
	lock, which also stops the pointer. AccelerantHWInterface always keeps
	a back buffer in main memory with the same content, except for:

	- the cursor, which is only ever drawn into the front buffer: it is
	  composited here when asked, with the drag bitmap, whether the cursor
	  is in hardware or software;
	- direct windows (BDirectWindow while connected) and BWindowScreen
	  (windows with kWindowScreenFlag, and the whole screen while a team
	  holds Desktop::LockDirectScreen()), whose content exists only in the
	  front buffer: their visible area is read from the front buffer, and
	  nothing else is;
	- windows in an update session: Window::BeginUpdate() clears the
	  update region in the back buffer and the client redraws it over
	  several messages; the front buffer is only updated at EndUpdate().
	  While captures are active (one was made in the last few seconds),
	  BeginUpdate() saves the region's last completed frame from the back
	  buffer before clearing it (SaveUpdateSnapshot()), and reads use that
	  copy until EndUpdate(), so a capture never shows a half-drawn or
	  erased window.

	Read() needs no lock held. It takes the Desktop's window read lock and
	the drawing engine's lock, exclusive or parallel as asked: the copy is
	from main memory, so either is held only briefly. Readers under the
	parallel lock may see a drawing operation in progress; the damage that
	operation reports when it is done is what BScreenCapture copies again
	for the next frame.
*/
namespace ScreenReader {


struct Stats {
	bigtime_t	lockWait;		// waiting for the locks
	bigtime_t	lockHeld;		// copying with the engine lock held
	int64		backPixels;		// from the back buffer
	int64		frontPixels;	// from the front buffer
	int64		snapshotPixels;	// from saved update regions
	bool		frontBufferOnly;	// single buffered, or the direct
									// screen lock is held
};


/*!	Copies \a area (screen coordinates) of the screen into \a bitmap, the
	screen point \a origin going to the bitmap's (0, 0). Nothing is written
	outside the bitmap. With \a drawCursor the cursor (and the drag bitmap,
	if any) is composited over it; \a _cursorFrame, if given, is set to
	its frame in screen coordinates (invalid if it was not drawn).
*/
status_t		Read(Desktop* desktop, ServerBitmap* bitmap,
					const BRegion& area, BPoint origin, bool drawCursor,
					bool exclusive, BRect* _cursorFrame = NULL,
					Stats* stats = NULL);

//!	A capture was made now: BeginUpdate() saves update regions for a while.
void			NoteCapture();
bool			CaptureActive();

/*!	Called by Window::BeginUpdate() with the engine's parallel lock held,
	before the update region is cleared, while CaptureActive().
	\a region is in screen coordinates.
*/
void			SaveUpdateSnapshot(Desktop* desktop, Window* window,
					const BRegion& region);
//!	Called by Window::EndUpdate() and ~Window().
void			DropUpdateSnapshot(Window* window);

/*!	For tests and diagnostics (AS_TEST_SCREEN_READ, harness images): the
	totals of the reads since the last reset, and the old front-buffer read
	(DrawingEngine::ReadBitmap() under the exclusive lock) for
	BScreen::ReadBitmap(), to measure against.
*/
void			AddToTotals(bigtime_t time, bigtime_t lockHeld,
					int64 backPixels, int64 frontPixels,
					int64 snapshotPixels);
void			GetTotals(screen_read_stats& totals, bool reset);
bool			LegacyReads();
void			SetLegacyReads(bool legacy);


}	// namespace ScreenReader


#endif	// SCREEN_READER_H
