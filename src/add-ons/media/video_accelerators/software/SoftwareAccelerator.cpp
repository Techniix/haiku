/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The reference back end of the video acceleration interface (roadmap
	F-31 phase 5, headers/private/media/VideoAcceleration.h): it decodes
	and encodes with FFmpeg in software, behind exactly the interface a
	hardware back end implements, with surfaces in NV12 kept in areas like
	a driver's. It is never chosen automatically (IsHardware() is false);
	RENGA_VIDEO_ACCELERATION=software or the setting "backend software"
	selects it. The harness uses it to test the plumbing: the roster, the
	FFmpeg plug-in's accelerated paths and MediaWriter/MediaReader.

	A hardware back end (NVDEC/NVENC on the maintainer's NVIDIA driver,
	Mesa's video through LinuxKPI DRM drivers) replaces the FFmpeg calls
	here with its device's; the surfaces, the packet flow and the
	capability list stay as they are.
*/


#include <VideoAcceleration.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include <OS.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}


using namespace BPrivate;


static const int32 kMaxWidth = 4096;
static const int32 kMaxHeight = 2304;


struct ProfileCodec {
	VideoAccelProfile	profile;
	AVCodecID			codec;
	uint32				entrypoints;
};

// Decoding: what the build's FFmpeg decodes; encoding: what it encodes
// (no H.264/HEVC/AV1 encoders in it).
static const ProfileCodec kProfiles[] = {
	{ B_VIDEO_ACCEL_MPEG2_MAIN, AV_CODEC_ID_MPEG2VIDEO,
		B_VIDEO_ACCEL_DECODE | B_VIDEO_ACCEL_ENCODE },
	{ B_VIDEO_ACCEL_MPEG4_SIMPLE, AV_CODEC_ID_MPEG4,
		B_VIDEO_ACCEL_DECODE | B_VIDEO_ACCEL_ENCODE },
	{ B_VIDEO_ACCEL_MPEG4_ADVANCED_SIMPLE, AV_CODEC_ID_MPEG4,
		B_VIDEO_ACCEL_DECODE },
	{ B_VIDEO_ACCEL_H264_CONSTRAINED_BASELINE, AV_CODEC_ID_H264,
		B_VIDEO_ACCEL_DECODE },
	{ B_VIDEO_ACCEL_H264_MAIN, AV_CODEC_ID_H264, B_VIDEO_ACCEL_DECODE },
	{ B_VIDEO_ACCEL_H264_HIGH, AV_CODEC_ID_H264, B_VIDEO_ACCEL_DECODE },
	{ B_VIDEO_ACCEL_HEVC_MAIN, AV_CODEC_ID_HEVC, B_VIDEO_ACCEL_DECODE },
	{ B_VIDEO_ACCEL_VP8, AV_CODEC_ID_VP8,
		B_VIDEO_ACCEL_DECODE | B_VIDEO_ACCEL_ENCODE },
	{ B_VIDEO_ACCEL_VP9_PROFILE0, AV_CODEC_ID_VP9,
		B_VIDEO_ACCEL_DECODE | B_VIDEO_ACCEL_ENCODE },
	{ B_VIDEO_ACCEL_AV1_MAIN, AV_CODEC_ID_AV1, B_VIDEO_ACCEL_DECODE },
};


static AVCodecID
codec_for(VideoAccelProfile profile)
{
	for (size_t i = 0; i < B_COUNT_OF(kProfiles); i++) {
		if (kProfiles[i].profile == profile)
			return kProfiles[i].codec;
	}
	return AV_CODEC_ID_NONE;
}


/*!	A pool of NV12 surfaces, each in an area of its own, like the buffers a
	driver maps for the client.
*/
class SurfacePool {
public:
	SurfacePool()
		:
		fWidth(0),
		fHeight(0)
	{
		for (int i = 0; i < kCount; i++) {
			fAreas[i] = -1;
			fBits[i] = NULL;
			fBusy[i] = false;
		}
	}

	~SurfacePool()
	{
		_Free();
	}

	status_t SetSize(int32 width, int32 height)
	{
		if (width == fWidth && height == fHeight && fAreas[0] >= 0)
			return B_OK;
		_Free();
		fWidth = width;
		fHeight = height;
		fBytesPerRow = (width + 63) & ~63;
		size_t size = (fBytesPerRow * (size_t)height * 3 / 2 + B_PAGE_SIZE
			- 1) & ~(B_PAGE_SIZE - 1);
		for (int i = 0; i < kCount; i++) {
			fAreas[i] = create_area("video accel surface", (void**)&fBits[i],
				B_ANY_ADDRESS, size, B_NO_LOCK,
				B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
			if (fAreas[i] < 0)
				return fAreas[i];
			fBusy[i] = false;
		}
		return B_OK;
	}

	status_t Acquire(VideoAccelSurface* surface)
	{
		for (int i = 0; i < kCount; i++) {
			if (fBusy[i] || fAreas[i] < 0)
				continue;
			fBusy[i] = true;
			surface->width = fWidth;
			surface->height = fHeight;
			surface->format = B_VIDEO_ACCEL_NV12;
			surface->planes[0] = fBits[i];
			surface->planes[1] = fBits[i] + fBytesPerRow * (size_t)fHeight;
			surface->bytesPerRow[0] = surface->bytesPerRow[1] = fBytesPerRow;
			surface->area = fAreas[i];
			surface->timestamp = 0;
			surface->keyFrame = false;
			surface->backend = (void*)(addr_t)i;
			return B_OK;
		}
		return B_WOULD_BLOCK;
	}

	void Release(VideoAccelSurface* surface)
	{
		int index = (int)(addr_t)surface->backend;
		if (index >= 0 && index < kCount)
			fBusy[index] = false;
	}

private:
	void _Free()
	{
		for (int i = 0; i < kCount; i++) {
			if (fAreas[i] >= 0)
				delete_area(fAreas[i]);
			fAreas[i] = -1;
			fBits[i] = NULL;
			fBusy[i] = false;
		}
	}

	static const int kCount = 6;
	area_id		fAreas[kCount];
	uint8*		fBits[kCount];
	bool		fBusy[kCount];
	int32		fWidth;
	int32		fHeight;
	int32		fBytesPerRow;
};


// #pragma mark - decoder


class SoftwareDecoder : public VideoAccelDecoder {
public:
	SoftwareDecoder()
		:
		fContext(NULL),
		fFrame(av_frame_alloc()),
		fPacket(av_packet_alloc()),
		fScaler(NULL),
		fFlushed(false)
	{
	}

	virtual ~SoftwareDecoder()
	{
		sws_freeContext(fScaler);
		av_packet_free(&fPacket);
		av_frame_free(&fFrame);
		avcodec_free_context(&fContext);
	}

	status_t Init(const VideoAccelDecoderSetup& setup)
	{
		const AVCodec* codec = avcodec_find_decoder(codec_for(setup.profile));
		if (codec == NULL)
			return B_NOT_SUPPORTED;
		fContext = avcodec_alloc_context3(codec);
		if (fContext == NULL || fFrame == NULL || fPacket == NULL)
			return B_NO_MEMORY;
		fContext->width = setup.width;
		fContext->height = setup.height;
		fContext->pkt_timebase = (AVRational){ 1, 1000000 };
		if (setup.extradata != NULL && setup.extradataSize > 0) {
			fContext->extradata = (uint8_t*)av_mallocz(setup.extradataSize
				+ AV_INPUT_BUFFER_PADDING_SIZE);
			if (fContext->extradata == NULL)
				return B_NO_MEMORY;
			memcpy(fContext->extradata, setup.extradata, setup.extradataSize);
			fContext->extradata_size = setup.extradataSize;
		}
		if (avcodec_open2(fContext, codec, NULL) < 0)
			return B_ERROR;
		return B_OK;
	}

	virtual status_t SendPacket(const void* data, size_t size,
		bigtime_t timestamp, bool keyFrame)
	{
		int result;
		if (data == NULL) {
			fFlushed = true;
			result = avcodec_send_packet(fContext, NULL);
		} else {
			av_packet_unref(fPacket);
			if (av_new_packet(fPacket, size) < 0)
				return B_NO_MEMORY;
			memcpy(fPacket->data, data, size);
			fPacket->pts = timestamp;
			if (keyFrame)
				fPacket->flags |= AV_PKT_FLAG_KEY;
			result = avcodec_send_packet(fContext, fPacket);
		}
		if (result == AVERROR(EAGAIN))
			return B_WOULD_BLOCK;
		if (result == AVERROR_EOF)
			return B_OK;
		return result < 0 ? B_BAD_DATA : B_OK;
	}

	virtual status_t ReceiveSurface(VideoAccelSurface* surface)
	{
		int result = avcodec_receive_frame(fContext, fFrame);
		if (result == AVERROR(EAGAIN))
			return B_WOULD_BLOCK;
		if (result == AVERROR_EOF)
			return B_LAST_BUFFER_ERROR;
		if (result < 0)
			return B_BAD_DATA;

		status_t status = fPool.SetSize(fFrame->width, fFrame->height);
		if (status == B_OK)
			status = fPool.Acquire(surface);
		if (status != B_OK) {
			av_frame_unref(fFrame);
			return status == B_WOULD_BLOCK ? B_NO_MEMORY : status;
		}

		// into the surface's NV12, as a decoder writes its output surface
		fScaler = sws_getCachedContext(fScaler, fFrame->width, fFrame->height,
			(AVPixelFormat)fFrame->format, fFrame->width, fFrame->height,
			AV_PIX_FMT_NV12, SWS_POINT, NULL, NULL, NULL);
		uint8_t* destination[4] = { surface->planes[0], surface->planes[1],
			NULL, NULL };
		int lines[4] = { surface->bytesPerRow[0], surface->bytesPerRow[1],
			0, 0 };
		if (fScaler == NULL || sws_scale(fScaler, fFrame->data,
				fFrame->linesize, 0, fFrame->height, destination, lines)
					<= 0) {
			fPool.Release(surface);
			av_frame_unref(fFrame);
			return B_ERROR;
		}
		surface->timestamp = fFrame->best_effort_timestamp
			!= AV_NOPTS_VALUE ? fFrame->best_effort_timestamp : fFrame->pts;
		surface->keyFrame = (fFrame->flags & AV_FRAME_FLAG_KEY) != 0;
		av_frame_unref(fFrame);
		return B_OK;
	}

	virtual void ReleaseSurface(VideoAccelSurface* surface)
	{
		fPool.Release(surface);
	}

	virtual void Reset()
	{
		avcodec_flush_buffers(fContext);
		fFlushed = false;
	}

	virtual const char* BackendName() const
	{
		return "software";
	}

private:
	AVCodecContext*	fContext;
	AVFrame*		fFrame;
	AVPacket*		fPacket;
	SwsContext*		fScaler;
	SurfacePool		fPool;
	bool			fFlushed;
};


// #pragma mark - encoder


class SoftwareEncoder : public VideoAccelEncoder {
public:
	SoftwareEncoder()
		:
		fContext(NULL),
		fFrame(av_frame_alloc()),
		fPacket(av_packet_alloc()),
		fScaler(NULL),
		fFrames(0)
	{
	}

	virtual ~SoftwareEncoder()
	{
		sws_freeContext(fScaler);
		av_packet_free(&fPacket);
		av_frame_free(&fFrame);
		avcodec_free_context(&fContext);
	}

	status_t Init(const VideoAccelEncoderSetup& setup)
	{
		if (setup.format != B_VIDEO_ACCEL_NV12)
			return B_NOT_SUPPORTED;
		const AVCodec* codec = avcodec_find_encoder(codec_for(setup.profile));
		if (codec == NULL)
			return B_NOT_SUPPORTED;
		fContext = avcodec_alloc_context3(codec);
		if (fContext == NULL || fFrame == NULL || fPacket == NULL)
			return B_NO_MEMORY;
		fContext->width = setup.width;
		fContext->height = setup.height;
		fContext->pix_fmt = AV_PIX_FMT_YUV420P;
		fContext->time_base = (AVRational){ 1, 1000000 };
		fContext->framerate = av_d2q(setup.frameRate > 0 ? setup.frameRate
			: 30, 1000);
		fContext->gop_size = setup.keyFrameInterval > 0
			? setup.keyFrameInterval : 30;
		fContext->max_b_frames = 0;
		fContext->bit_rate = setup.bitRate > 0 ? setup.bitRate
			: (int64_t)(setup.width * setup.height * 4 * (0.25
				+ setup.quality));
		if (fContext->codec_id == AV_CODEC_ID_VP8
			|| fContext->codec_id == AV_CODEC_ID_VP9) {
			av_opt_set(fContext->priv_data, "deadline", "realtime", 0);
			av_opt_set_int(fContext->priv_data, "cpu-used", 8, 0);
			av_opt_set_int(fContext->priv_data, "lag-in-frames", 0, 0);
		}
		if (avcodec_open2(fContext, codec, NULL) < 0)
			return B_ERROR;
		fFrame->format = AV_PIX_FMT_YUV420P;
		fFrame->width = setup.width;
		fFrame->height = setup.height;
		if (av_frame_get_buffer(fFrame, 0) < 0)
			return B_NO_MEMORY;
		return fPool.SetSize(setup.width, setup.height);
	}

	virtual status_t AcquireSurface(VideoAccelSurface* surface)
	{
		return fPool.Acquire(surface);
	}

	virtual status_t SendSurface(VideoAccelSurface* surface,
		bool forceKeyFrame)
	{
		int result;
		if (surface == NULL)
			result = avcodec_send_frame(fContext, NULL);
		else {
			if (av_frame_make_writable(fFrame) < 0) {
				fPool.Release(surface);
				return B_NO_MEMORY;
			}
			fScaler = sws_getCachedContext(fScaler, surface->width,
				surface->height, AV_PIX_FMT_NV12, fContext->width,
				fContext->height, AV_PIX_FMT_YUV420P, SWS_POINT, NULL, NULL,
				NULL);
			const uint8_t* source[4] = { surface->planes[0],
				surface->planes[1], NULL, NULL };
			int lines[4] = { surface->bytesPerRow[0],
				surface->bytesPerRow[1], 0, 0 };
			if (fScaler != NULL) {
				sws_scale(fScaler, source, lines, 0, surface->height,
					fFrame->data, fFrame->linesize);
			}
			fFrame->pts = surface->timestamp;
			fFrame->pict_type = forceKeyFrame ? AV_PICTURE_TYPE_I
				: AV_PICTURE_TYPE_NONE;
			fPool.Release(surface);
			result = avcodec_send_frame(fContext, fFrame);
			fFrames++;
		}
		if (result == AVERROR(EAGAIN))
			return B_WOULD_BLOCK;
		return result < 0 && result != AVERROR_EOF ? B_ERROR : B_OK;
	}

	virtual status_t ReceivePacket(void* buffer, size_t* _size,
		bigtime_t* _timestamp, bool* _keyFrame)
	{
		if (fPacket->size == 0) {
			int result = avcodec_receive_packet(fContext, fPacket);
			if (result == AVERROR(EAGAIN))
				return B_WOULD_BLOCK;
			if (result == AVERROR_EOF)
				return B_LAST_BUFFER_ERROR;
			if (result < 0)
				return B_ERROR;
		}
		if ((size_t)fPacket->size > *_size) {
			*_size = fPacket->size;
			return B_NO_MEMORY;
		}
		memcpy(buffer, fPacket->data, fPacket->size);
		*_size = fPacket->size;
		*_timestamp = fPacket->pts;
		*_keyFrame = (fPacket->flags & AV_PKT_FLAG_KEY) != 0;
		av_packet_unref(fPacket);
		return B_OK;
	}

	virtual status_t GetExtradata(const void** _data, size_t* _size)
	{
		*_data = fContext->extradata;
		*_size = fContext->extradata_size;
		return B_OK;
	}

	virtual const char* BackendName() const
	{
		return "software";
	}

private:
	AVCodecContext*	fContext;
	AVFrame*		fFrame;
	AVPacket*		fPacket;
	SwsContext*		fScaler;
	SurfacePool		fPool;
	int64			fFrames;
};


// #pragma mark - back end


class SoftwareAccelerator : public VideoAccelerator {
public:
	SoftwareAccelerator()
	{
		for (size_t i = 0; i < B_COUNT_OF(kProfiles); i++) {
			VideoAccelCapability& capability = fCapabilities[i];
			capability.profile = kProfiles[i].profile;
			capability.entrypoints = 0;
			if ((kProfiles[i].entrypoints & B_VIDEO_ACCEL_DECODE) != 0
				&& avcodec_find_decoder(kProfiles[i].codec) != NULL) {
				capability.entrypoints |= B_VIDEO_ACCEL_DECODE;
			}
			if ((kProfiles[i].entrypoints & B_VIDEO_ACCEL_ENCODE) != 0
				&& avcodec_find_encoder(kProfiles[i].codec) != NULL) {
				capability.entrypoints |= B_VIDEO_ACCEL_ENCODE;
			}
			capability.maxWidth = kMaxWidth;
			capability.maxHeight = kMaxHeight;
			capability.formats = B_VIDEO_ACCEL_NV12;
		}
	}

	virtual const char* Name() const { return "software"; }
	virtual const char* DeviceName() const
	{
		return "FFmpeg in software (reference back end)";
	}
	virtual bool IsHardware() const { return false; }

	virtual int32 CountCapabilities() const
	{
		return B_COUNT_OF(kProfiles);
	}

	virtual const VideoAccelCapability* CapabilityAt(int32 index) const
	{
		if (index < 0 || index >= (int32)B_COUNT_OF(kProfiles))
			return NULL;
		return &fCapabilities[index];
	}

	virtual status_t CreateDecoder(const VideoAccelDecoderSetup& setup,
		VideoAccelDecoder** _decoder)
	{
		if (!Supports(setup.profile, B_VIDEO_ACCEL_DECODE, setup.width,
				setup.height)) {
			return B_NOT_SUPPORTED;
		}
		SoftwareDecoder* decoder = new(std::nothrow) SoftwareDecoder;
		if (decoder == NULL)
			return B_NO_MEMORY;
		status_t status = decoder->Init(setup);
		if (status != B_OK) {
			delete decoder;
			return status;
		}
		*_decoder = decoder;
		return B_OK;
	}

	virtual status_t CreateEncoder(const VideoAccelEncoderSetup& setup,
		VideoAccelEncoder** _encoder)
	{
		if (!Supports(setup.profile, B_VIDEO_ACCEL_ENCODE, setup.width,
				setup.height)) {
			return B_NOT_SUPPORTED;
		}
		SoftwareEncoder* encoder = new(std::nothrow) SoftwareEncoder;
		if (encoder == NULL)
			return B_NO_MEMORY;
		status_t status = encoder->Init(setup);
		if (status != B_OK) {
			delete encoder;
			return status;
		}
		*_encoder = encoder;
		return B_OK;
	}

private:
	VideoAccelCapability fCapabilities[B_COUNT_OF(kProfiles)];
};


extern "C" VideoAccelerator*
instantiate_video_accelerator(uint32 version)
{
	if (version != B_VIDEO_ACCELERATION_VERSION)
		return NULL;
	return new(std::nothrow) SoftwareAccelerator;
}
