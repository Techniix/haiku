/*
 * Copyright 2011, Gabriel Hartmann, gabriel.hartmann@gmail.com.
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _UVC_DEFRAMER_H
#define _UVC_DEFRAMER_H


#include "CamDeframer.h"
#include "UVCPayload.h"

#include <USB3.h>


/*!	Puts frames together from UVC payloads (UVCPayloadParser: FID, EOF, ERR,
	frame size) and queues them for the producer. Renga: the old version
	took everything after the header without checking it, ended frames on
	EOF only half the time (its FID test compared before updating), kept
	frames with transmission errors, and queued short ones.
*/
class UVCDeframer : public CamDeframer {
public:
								UVCDeframer(CamDevice *device);
	virtual 					~UVCDeframer();
					// BPositionIO interface
					// write from usb transfers
	virtual ssize_t				Write(const void *buffer, size_t size);

			// the size of an uncompressed frame, 0 for MJPEG
			void				SetFrameSize(size_t size);

private:
	UVCPayloadParser			fParser;
};

#endif /* _UVC_DEFRAMER_H */
