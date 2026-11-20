/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FrontCopy.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <Autolock.h>
#include <Locker.h>
#include <OS.h>

#if defined(__SSE2__)
#	include <emmintrin.h>
#	define FRONT_COPY_SIMD 1
#else
#	define FRONT_COPY_SIMD 0
#endif


static BLocker sShadowLock("front copy shadow");
static const int32 kBlockPixels = 16;
	// one 64-byte cache line of 32-bit pixels
static const int32 kBlockBytes = kBlockPixels * 4;


FrontCopy::FrontCopy()
	:
	fMode(B_FRONT_COPY_CHANGED),
	fShadow(NULL),
	fValid(NULL),
	fShadowBPR(0),
	fWidth(0),
	fHeight(0),
	fBlocksPerRow(0),
	fFront(NULL),
	fDirectAccess(0),
	fRects(0),
	fBytesRequested(0),
	fBytesWritten(0),
	fCopyTime(0)
{
}


FrontCopy::~FrontCopy()
{
	free(fShadow);
	free(fValid);
}


void
FrontCopy::SetMode(int32 mode)
{
	if (mode < B_FRONT_COPY_PLAIN || mode > B_FRONT_COPY_CHANGED)
		mode = B_FRONT_COPY_CHANGED;
	fMode = mode;
	Invalidate();
	if (mode != B_FRONT_COPY_CHANGED) {
		// no use for the memory while the shadow is off
		free(fShadow);
		free(fValid);
		fShadow = NULL;
		fValid = NULL;
		fWidth = fHeight = 0;
	}
}


void
FrontCopy::Invalidate()
{
	if (fValid != NULL)
		memset(fValid, 0, (size_t)fBlocksPerRow * fHeight);
}


void
FrontCopy::DirectAccessChanged(int32 delta)
{
	atomic_add(&fDirectAccess, delta);
	Invalidate();
}


bool
FrontCopy::HasSimd()
{
	return FRONT_COPY_SIMD != 0;
}


/*!	Copies \a bytes; with SSE2, the destination is written with
	non-temporal 16-byte stores (whole write-combining lines, no cache
	pollution), with the unaligned head and tail copied as usual. The
	caller ends a batch with a store fence (done at the end of Copy()).
*/
void
FrontCopy::StreamCopy(uint8* dst, const uint8* src, size_t bytes)
{
#if FRONT_COPY_SIMD
	if (bytes >= 64) {
		size_t head = (16 - ((addr_t)dst & 15)) & 15;
		if (head != 0) {
			memcpy(dst, src, head);
			dst += head;
			src += head;
			bytes -= head;
		}
		while (bytes >= 64) {
			__m128i a = _mm_loadu_si128((const __m128i*)src);
			__m128i b = _mm_loadu_si128((const __m128i*)(src + 16));
			__m128i c = _mm_loadu_si128((const __m128i*)(src + 32));
			__m128i d = _mm_loadu_si128((const __m128i*)(src + 48));
			_mm_stream_si128((__m128i*)dst, a);
			_mm_stream_si128((__m128i*)(dst + 16), b);
			_mm_stream_si128((__m128i*)(dst + 32), c);
			_mm_stream_si128((__m128i*)(dst + 48), d);
			dst += 64;
			src += 64;
			bytes -= 64;
		}
		while (bytes >= 16) {
			_mm_stream_si128((__m128i*)dst,
				_mm_loadu_si128((const __m128i*)src));
			dst += 16;
			src += 16;
			bytes -= 16;
		}
	}
#endif
	if (bytes > 0)
		memcpy(dst, src, bytes);
}


static inline void
store_fence()
{
#if FRONT_COPY_SIMD
	_mm_sfence();
#endif
}


//!	Whether \a bytes at \a a and \a b differ (SSE2: 16 bytes at a time).
static inline bool
differs(const uint8* a, const uint8* b, size_t bytes)
{
#if FRONT_COPY_SIMD
	while (bytes >= 16) {
		__m128i equal = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)a),
			_mm_loadu_si128((const __m128i*)b));
		if (_mm_movemask_epi8(equal) != 0xffff)
			return true;
		a += 16;
		b += 16;
		bytes -= 16;
	}
#endif
	return bytes > 0 && memcmp(a, b, bytes) != 0;
}


//!	An ordinary copy into main memory (the shadow), SSE2 where available.
static inline void
copy_to_memory(uint8* dst, const uint8* src, size_t bytes)
{
#if FRONT_COPY_SIMD
	while (bytes >= 16) {
		_mm_storeu_si128((__m128i*)dst, _mm_loadu_si128((const __m128i*)src));
		dst += 16;
		src += 16;
		bytes -= 16;
	}
#endif
	if (bytes > 0)
		memcpy(dst, src, bytes);
}


bool
FrontCopy::_PrepareShadow(uint8* front, uint32 frontBPR, int32 width,
	int32 height)
{
	if (fShadow != NULL && fWidth == width && fHeight == height
		&& fFront == front)
		return true;

	BAutolock _(sShadowLock);
	if (fShadow != NULL && fWidth == width && fHeight == height
		&& fFront == front)
		return true;
	free(fShadow);
	free(fValid);
	fShadowBPR = width * 4;
	fBlocksPerRow = (width + kBlockPixels - 1) / kBlockPixels;
	fShadow = (uint8*)malloc((size_t)fShadowBPR * height);
	fValid = (uint8*)calloc((size_t)fBlocksPerRow * height, 1);
	if (fShadow == NULL || fValid == NULL) {
		free(fShadow);
		free(fValid);
		fShadow = NULL;
		fValid = NULL;
		fWidth = fHeight = 0;
		return false;
	}
	fWidth = width;
	fHeight = height;
	fFront = front;
	return true;
}


void
FrontCopy::Copy(uint8* front, uint32 frontBPR, int32 frontWidth,
	int32 frontHeight, const uint8* src, uint32 srcBPR, int32 x, int32 y,
	int32 right, int32 bottom)
{
	int32 bytes = (right - x + 1) * 4;
	if (bytes <= 0 || bottom < y)
		return;

	bigtime_t start = system_time();
	int64 requested = (int64)bytes * (bottom - y + 1);
	int64 written = 0;
	uint8* dst = front + y * frontBPR + x * 4;

	bool changedOnly = fMode == B_FRONT_COPY_CHANGED && fDirectAccess == 0
		&& x >= 0 && y >= 0 && right < frontWidth && bottom < frontHeight
		&& _PrepareShadow(front, frontBPR, frontWidth, frontHeight);

	if (!changedOnly) {
		for (int32 row = y; row <= bottom; row++) {
			if (fMode == B_FRONT_COPY_PLAIN)
				memcpy(dst, src, bytes);
			else
				StreamCopy(dst, src, bytes);
			dst += frontBPR;
			src += srcBPR;
		}
		written = requested;
	} else {
		int32 firstBlock = x / kBlockPixels;
		int32 lastBlock = right / kBlockPixels;
		for (int32 row = y; row <= bottom; row++) {
			uint8* shadowRow = fShadow + row * fShadowBPR;
			uint8* valid = fValid + row * fBlocksPerRow;
			// spans of blocks that need writing are written at once
			int32 spanStart = -1;
			int32 spanEnd = -1;
			for (int32 block = firstBlock; block <= lastBlock + 1; block++) {
				bool write = false;
				int32 left = 0, end = 0;
				if (block <= lastBlock) {
					left = std::max(x, block * kBlockPixels);
					end = std::min(right + 1, (block + 1) * kBlockPixels);
					const uint8* from = src + (left - x) * 4;
					write = valid[block] == 0
						|| differs(shadowRow + left * 4, from,
							(end - left) * 4);
				}
				if (write) {
					if (spanStart < 0)
						spanStart = left;
					spanEnd = end;
					continue;
				}
				if (spanStart >= 0) {
					size_t length = (spanEnd - spanStart) * 4;
					const uint8* from = src + (spanStart - x) * 4;
					StreamCopy(front + row * frontBPR + spanStart * 4, from,
						length);
					copy_to_memory(shadowRow + spanStart * 4, from, length);
					for (int32 b = spanStart / kBlockPixels;
							b <= (spanEnd - 1) / kBlockPixels; b++) {
						// a partly written block stays valid only if it
						// was valid before (the rest still matches)
						if (b * kBlockPixels >= spanStart
							&& (b + 1) * kBlockPixels <= spanEnd)
							valid[b] = 1;
					}
					written += length;
					spanStart = -1;
				}
			}
			src += srcBPR;
		}
	}
	store_fence();

	atomic_add64(&fRects, 1);
	atomic_add64(&fBytesRequested, requested);
	atomic_add64(&fBytesWritten, written);
	atomic_add64(&fCopyTime, system_time() - start);
}


void
FrontCopy::GetStats(front_copy_stats& stats, bool reset)
{
	memset(&stats, 0, sizeof(stats));
	stats.mode = fMode;
	stats.simd = HasSimd() ? 1 : 0;
	stats.rects = atomic_get64(&fRects);
	stats.bytes_requested = atomic_get64(&fBytesRequested);
	stats.bytes_written = atomic_get64(&fBytesWritten);
	stats.copy_time = atomic_get64(&fCopyTime);
	stats.shadow_size = fShadow != NULL ? fShadowBPR * fHeight : 0;
	if (reset) {
		atomic_set64(&fRects, 0);
		atomic_set64(&fBytesRequested, 0);
		atomic_set64(&fBytesWritten, 0);
		atomic_set64(&fCopyTime, 0);
	}
}
