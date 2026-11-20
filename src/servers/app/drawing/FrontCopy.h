/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef FRONT_COPY_H
#define FRONT_COPY_H


#include <FrontCopyInfo.h>
#include <SupportDefs.h>


/*!	Renga (batch 7): the 32-bit copies from app_server's back buffer to the
	frame buffer, see <FrontCopyInfo.h>. Called with the HWInterface locked
	for reading, so several threads may copy different rectangles at once:
	the shadow is only written where the frame buffer is written, its
	per-block "valid" marks are bytes, and the counters are atomic.

	The shadow mirrors what was written to the frame buffer through here
	(the back buffer's pixels and the software cursor's), so a block whose
	new pixels equal the shadow's needs no write. Anything else that writes
	to the frame buffer (BDirectWindow, BWindowScreen, a mode change) makes
	the whole shadow invalid; while such a client is active the shadow is
	not used.
*/
class FrontCopy {
public:
								FrontCopy();
								~FrontCopy();

			void				SetMode(int32 mode);
			int32				Mode() const { return fMode; }

			//! Copies rows of \a bytes (a multiple of 4) at x, y of the
			//! frame buffer (\a front, \a frontBPR, \a width x \a height).
			void				Copy(uint8* front, uint32 frontBPR,
									int32 frontWidth, int32 frontHeight,
									const uint8* src, uint32 srcBPR,
									int32 x, int32 y, int32 right,
									int32 bottom);

			void				Invalidate();
			void				DirectAccessChanged(int32 delta);

			void				GetStats(front_copy_stats& stats, bool reset);

	static	void				StreamCopy(uint8* dst, const uint8* src,
									size_t bytes);
	static	bool				HasSimd();

private:
			bool				_PrepareShadow(uint8* front, uint32 frontBPR,
									int32 width, int32 height);

			int32				fMode;
			uint8*				fShadow;
			uint8*				fValid;
				// one byte per 64-pixel block: 1 if the shadow holds what
				// the frame buffer shows
			uint32				fShadowBPR;
			int32				fWidth;
			int32				fHeight;
			int32				fBlocksPerRow;
			uint8*				fFront;
			int32				fDirectAccess;

			int64				fRects;
			int64				fBytesRequested;
			int64				fBytesWritten;
			int64				fCopyTime;
};


#endif	// FRONT_COPY_H
