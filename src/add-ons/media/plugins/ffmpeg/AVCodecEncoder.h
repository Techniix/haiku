/*
 * Copyright 2009, Stephan Aßmus <superstippi@gmx.de>.
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef AVCODEC_ENCODER_H
#define AVCODEC_ENCODER_H


#include <MediaFormats.h>

extern "C" {
	#include "avcodec.h"
	#include "swscale.h"
	#include "libavutil/fifo.h"
}

#include "EncoderPlugin.h"

#include <String.h>
#include <VideoAcceleration.h>


typedef AVCodecID CodecID;


class AVCodecEncoder : public Encoder {
public:
								AVCodecEncoder(uint32 codecID,
									int bitRateScale);

	virtual						~AVCodecEncoder();

	virtual	status_t			AcceptedFormat(
									const media_format* proposedInputFormat,
									media_format* _acceptedInputFormat = NULL);

	virtual	status_t			SetUp(const media_format* inputFormat);

	virtual status_t			GetEncodeParameters(
									encode_parameters* parameters) const;
	virtual status_t			SetEncodeParameters(
									encode_parameters* parameters);

	virtual status_t			Encode(const void* buffer, int64 frameCount,
									media_encode_info* info);

	// TODO: Turns out we need Flush() after all. We buffer encoded audio
	// in a FIFO, since the user suggested buffer size may not fit the
	// codec buffer size.

private:
			void				_Init();
			status_t			_Setup();

			bool				_OpenCodecIfNeeded();
			void				_PublishExtradata();
			status_t			_ConvertAudio(const uint8* buffer,
									int64 frameCount);

			status_t			_EncodeAudio(const void* buffer,
									int64 frameCount,
									media_encode_info* info);
			status_t			_EncodeAudio(const uint8* buffer,
									size_t bufferSize, int64 frameCount,
									media_encode_info* info);

			status_t			_EncodeVideo(const void* buffer,
									int64 frameCount,
									media_encode_info* info);
			status_t			_EncodeVideoFrame(AVFrame* frame,
									AVPacket* pkt,
									media_encode_info* info);

			// Renga (F-31 phase 5): encoding through a video acceleration
			// back end (VideoAcceleration.h)
			void				_SetUpAcceleration();
			status_t			_EncodeAcceleratedVideo(const void* buffer,
									int64 frameCount,
									media_encode_info* info);
			status_t			_WriteAcceleratedPackets(
									media_encode_info* info);

private:
			media_format		fInputFormat;
			encode_parameters	fEncodeParameters;
			int					fBitRateScale;

			// FFmpeg related members
			// TODO: Refactor common base class from AVCodec[De|En]Coder!
			CodecID				fCodecID;
			const AVCodec*		fCodec;
			AVCodecContext*		fCodecContext;

			BPrivate::VideoAccelEncoder* fAccelEncoder;
			SwsContext*			fAccelSwsContext;
			BString				fAccelName;
			
			enum {
				CODEC_INIT_NEEDED = 0,
				CODEC_INIT_DONE,
				CODEC_INIT_FAILED
			};
			uint32                          fCodecInitStatus;

			// For video (color space conversion):
			AVFrame*			fFrame;
			SwsContext*			fSwsContext;

			// For encoded audio:
#if LIBAVCODEC_VERSION_MAJOR >= 60
			AVFifo*				fAudioFifo;
#else
			AVFifoBuffer*		fAudioFifo;
#endif

			int64				fFramesWritten;
			// Renga: the codec takes another sample format than the input
			// (planar float for AAC and Vorbis, ...): converted per frame
			bool				fConvertAudio;

			uint8*				fChunkBuffer;
};

#endif // AVCODEC_ENCODER_H
