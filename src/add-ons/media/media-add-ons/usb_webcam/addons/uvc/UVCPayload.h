/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _UVC_PAYLOAD_H
#define _UVC_PAYLOAD_H


/*!	USB Video Class payloads: frames put together from the payload
	transfers of a video streaming endpoint (UVC 1.5, 2.4.3.3 "Video and
	Still Image Payload Headers"), and the conversions the producer needs
	from the two formats almost every UVC camera offers (YUY2 and MJPEG) to
	B_RGB32. Free of USB and Media Kit dependencies, so that the harness can
	replay payloads through the same code (3rdparty/ce/tests/uvc).

	Rules, as Linux's uvc_video.c applies them: a payload starts with a
	header (bHeaderLength, bmHeaderInfo: FID bit 0, EOF 1, PTS 2, SCR 3,
	RES 4, STI 5, ERR 6, EOH 7). A frame ends with a payload that has EOF
	set, or when the FID toggles (some cameras never set EOF). A frame with
	ERR set in any of its payloads, or (for uncompressed formats) of the
	wrong size, is dropped. After a frame ended, payloads with its FID are
	the tail of a frame the stream lost sync with, and are dropped until
	the FID toggles. Invalid headers are ignored.
*/


#include <SupportDefs.h>

#include <vector>


class UVCPayloadParser {
public:
								UVCPayloadParser(size_t maxFrameSize);

			// 0 for formats whose frames vary in size (MJPEG)
			void				SetFrameSize(size_t size)
									{ fFrameSize = size; }

			// One payload (one isochronous packet, or one bulk transfer).
			// Returns true if a frame was completed; it is then in Frame()
			// until the next call.
			bool				Write(const uint8* payload, size_t size);
			const std::vector<uint8>& Frame() const { return fDone; }
			uint32				PresentationTime() const
									{ return fDonePTS; }

			void				Reset();

			// counters
			uint32				frames;
			uint32				droppedErrors;
			uint32				droppedSize;
			uint32				invalidHeaders;
			uint32				outOfSync;

private:
			bool				_Finish();

			size_t				fMaxFrameSize;
			size_t				fFrameSize;
			std::vector<uint8>	fCurrent;
			std::vector<uint8>	fDone;
			bool				fActive;
			bool				fError;
			int					fFID;		// of the current frame
			int					fLastFID;	// of the last finished frame
			uint32				fPTS;
			uint32				fDonePTS;
};


// Interleaved Y0 U Y1 V (4 bytes per two pixels) to B_RGB32 (B, G, R, A
// in memory), ITU-R BT.601 with studio swing (16-235), as UVC cameras send
// it. \a bytesPerRow of the destination.
void	uvc_yuy2_to_rgb32(const uint8* source, uint8* destination,
			uint32 width, uint32 height, uint32 bytesPerRow);

// MJPEG frames usually leave out the Huffman tables and use the standard
// ones (JPEG Annex K.3; the AVI1/MJPEG convention). Returns the frame with
// a DHT segment of the standard tables before its scan if it has none,
// unchanged otherwise. False if it does not look like a JPEG at all.
bool	uvc_mjpeg_add_huffman_tables(const uint8* frame, size_t size,
			std::vector<uint8>& jpeg);
bool	uvc_mjpeg_has_huffman_tables(const uint8* frame, size_t size);


#endif	// _UVC_PAYLOAD_H
