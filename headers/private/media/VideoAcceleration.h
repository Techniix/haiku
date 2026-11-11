/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VIDEO_ACCELERATION_H
#define _VIDEO_ACCELERATION_H


/*!	Hardware video decoding and encoding for the Media Kit (roadmap F-31
	phase 5): the interface between the FFmpeg plug-in (and MediaWriter /
	MediaReader, MediaCodec.h) and the back ends that drive video hardware.

	The shape is VA-API's, so that what a back end has to provide maps onto
	what GPU drivers already offer (libva drivers on Linux, NVIDIA's
	NVDEC/NVENC, Mesa's gallium video):
	- a back end is a "display" (VADisplay): VideoAccelerator, one per
	  device, found by VideoAcceleratorRoster;
	- what it can do is a list of profiles and entry points
	  (vaQueryConfigProfiles(), vaQueryConfigEntrypoints()):
	  VideoAccelProfile x VideoAccelEntrypoint, with the largest picture
	  (VAConfigAttribMaxPictureWidth/Height) and the surface formats
	  (VA_RT_FORMAT_YUV420 = NV12, VA_RT_FORMAT_YUV420_10 = P010);
	- a decoder or encoder is a configuration plus a context
	  (vaCreateConfig() + vaCreateContext()) with its pool of surfaces
	  (vaCreateSurfaces()): VideoAccelDecoder, VideoAccelEncoder;
	- pictures are surfaces (VASurfaceID), read back by mapping them
	  (vaDeriveImage()/vaMapBuffer()): VideoAccelSurface, Lock()/Unlock();
	  a back end whose surfaces live in an area can also hand out the area
	  (for zero-copy presentation later, F-47);
	- synchronisation is per surface (vaSyncSurface()): a surface returned
	  by ReceiveSurface() is complete.

	One deliberate difference: VA-API's decode entry point (VLD) takes
	slices with parsed parameters, so the client must parse the bitstream
	(FFmpeg's hwaccel layer does that for libva). The build's FFmpeg is
	built without libva, and NVIDIA's decoder brings its own parser
	(cuvidParseVideoData()), so VideoAccelDecoder takes whole compressed
	packets, as V4L2's stateful decoders and NVDEC with its parser do. A
	back end on top of a VLD driver (Mesa through a Haiku libva) parses
	with FFmpeg's hwaccel code once the FFmpeg port is built with VA-API;
	the interface does not change for its users.

	Back ends are add-ons in add-ons/media/video_accelerators (system, then
	user, then non-packaged), each exporting

		extern "C" BPrivate::VideoAccelerator*
			instantiate_video_accelerator(uint32 version);

	called with B_VIDEO_ACCELERATION_VERSION; it returns NULL if it finds
	no device. Which one is used: the setting
	~/config/settings/system/video_acceleration ("backend <name>",
	"backend off"), overridden by the environment variable
	RENGA_VIDEO_ACCELERATION (a back end's name or "off"). Without either,
	hardware back ends are used and the "software" reference back end is
	not (it decodes in software and exists to test the plumbing).

	All classes here are private to Renga (no ABI promise); the add-ons
	are built with the system.
*/


#include <OS.h>
#include <ObjectList.h>
#include <String.h>
#include <SupportDefs.h>


#define B_VIDEO_ACCELERATION_VERSION		1
#define RENGA_VIDEO_ACCELERATION_ENVIRONMENT	"RENGA_VIDEO_ACCELERATION"


namespace BPrivate {


//!	Codec profiles (VAProfile).
enum VideoAccelProfile {
	B_VIDEO_ACCEL_PROFILE_NONE = 0,
	B_VIDEO_ACCEL_MPEG2_MAIN,
	B_VIDEO_ACCEL_MPEG4_SIMPLE,
	B_VIDEO_ACCEL_MPEG4_ADVANCED_SIMPLE,
	B_VIDEO_ACCEL_H264_CONSTRAINED_BASELINE,
	B_VIDEO_ACCEL_H264_MAIN,
	B_VIDEO_ACCEL_H264_HIGH,
	B_VIDEO_ACCEL_HEVC_MAIN,
	B_VIDEO_ACCEL_HEVC_MAIN10,
	B_VIDEO_ACCEL_VP8,
	B_VIDEO_ACCEL_VP9_PROFILE0,
	B_VIDEO_ACCEL_VP9_PROFILE2,
	B_VIDEO_ACCEL_AV1_MAIN,
	B_VIDEO_ACCEL_PROFILE_COUNT
};

//!	Entry points (VAEntrypoint): VLD decoding, slice encoding.
enum VideoAccelEntrypoint {
	B_VIDEO_ACCEL_DECODE = 1,
	B_VIDEO_ACCEL_ENCODE = 2
};

//!	Surface formats (VA_RT_FORMAT_* / VA_FOURCC_*).
enum VideoAccelFormat {
	B_VIDEO_ACCEL_NV12 = 1,		// 8-bit 4:2:0: Y plane, interleaved UV
	B_VIDEO_ACCEL_P010 = 2		// 10-bit 4:2:0 in 16-bit words, MSB-aligned
};


struct VideoAccelCapability {
	VideoAccelProfile		profile;
	uint32					entrypoints;	// VideoAccelEntrypoint bits
	int32					maxWidth;
	int32					maxHeight;
	uint32					formats;		// VideoAccelFormat bits
};


/*!	A decoded picture, or one to encode (VASurface). The planes are valid
	between Lock() and Unlock() of its decoder or encoder.
*/
struct VideoAccelSurface {
	int32					width;
	int32					height;
	VideoAccelFormat		format;
	uint8*					planes[2];		// Y, UV
	int32					bytesPerRow[2];
	area_id					area;			// -1: not in a shareable area
	bigtime_t				timestamp;		// presentation time
	bool					keyFrame;
	void*					backend;		// the back end's own handle
};


struct VideoAccelDecoderSetup {
	VideoAccelProfile		profile;
	int32					width;			// coded size; 0 if unknown
	int32					height;
	const void*				extradata;		// codec configuration (avcC,
	size_t					extradataSize;	// hvcC, vpcC, OBUs...)
	int32					surfaces;		// pool size wanted (0: default)
};


struct VideoAccelEncoderSetup {
	VideoAccelProfile		profile;
	int32					width;
	int32					height;
	float					frameRate;
	int32					bitRate;		// bits per second; 0: by quality
	float					quality;		// 0..1
	int32					keyFrameInterval;	// frames; 0: back end's choice
	VideoAccelFormat		format;
};


class VideoAccelDecoder {
public:
	virtual						~VideoAccelDecoder() {}

	/*!	Hands over one compressed packet (a whole frame or temporal unit).
		B_WOULD_BLOCK: take surfaces out first, then send it again.
		NULL data flushes: the remaining surfaces come out, then
		ReceiveSurface() returns B_LAST_BUFFER_ERROR.
	*/
	virtual	status_t			SendPacket(const void* data, size_t size,
									bigtime_t timestamp, bool keyFrame) = 0;
	/*!	The next decoded surface in presentation order, locked for reading
		until ReleaseSurface(). B_WOULD_BLOCK: send more packets.
	*/
	virtual	status_t			ReceiveSurface(VideoAccelSurface* surface)
									= 0;
	virtual	void				ReleaseSurface(VideoAccelSurface* surface)
									= 0;
	//!	Drops everything queued (after a seek).
	virtual	void				Reset() = 0;

	virtual	const char*			BackendName() const = 0;
};


class VideoAccelEncoder {
public:
	virtual						~VideoAccelEncoder() {}

	/*!	A surface to fill with the next picture (Lock()ed for writing);
		hand it back with SendSurface().
	*/
	virtual	status_t			AcquireSurface(VideoAccelSurface* surface)
									= 0;
	//!	NULL flushes.
	virtual	status_t			SendSurface(VideoAccelSurface* surface,
									bool forceKeyFrame) = 0;
	/*!	The next packet into \a buffer (at most \a size bytes; B_NO_MEMORY
		and *_size the size needed if too small). B_WOULD_BLOCK: send more
		surfaces; B_LAST_BUFFER_ERROR after a flush.
	*/
	virtual	status_t			ReceivePacket(void* buffer, size_t* _size,
									bigtime_t* _timestamp,
									bool* _keyFrame) = 0;
	//!	Codec configuration for the container (avcC, ...), after the
	//	first packet or at once; may be empty.
	virtual	status_t			GetExtradata(const void** _data,
									size_t* _size) = 0;

	virtual	const char*			BackendName() const = 0;
};


class VideoAccelerator {
public:
	virtual						~VideoAccelerator() {}

	virtual	const char*			Name() const = 0;		// "software", "nvdec"
	virtual	const char*			DeviceName() const = 0;	// "GeForce RTX 5070"
	//!	Hardware back ends say true; "software" says false.
	virtual	bool				IsHardware() const = 0;

	virtual	int32				CountCapabilities() const = 0;
	virtual	const VideoAccelCapability* CapabilityAt(int32 index) const = 0;

	virtual	status_t			CreateDecoder(
									const VideoAccelDecoderSetup& setup,
									VideoAccelDecoder** _decoder) = 0;
	virtual	status_t			CreateEncoder(
									const VideoAccelEncoderSetup& setup,
									VideoAccelEncoder** _encoder) = 0;

			bool				Supports(VideoAccelProfile profile,
									VideoAccelEntrypoint entrypoint,
									int32 width = 0, int32 height = 0) const
	{
		for (int32 i = 0; i < CountCapabilities(); i++) {
			const VideoAccelCapability* capability = CapabilityAt(i);
			if (capability->profile == profile
				&& (capability->entrypoints & entrypoint) != 0
				&& width <= capability->maxWidth
				&& height <= capability->maxHeight) {
				return true;
			}
		}
		return false;
	}
};


typedef VideoAccelerator* (*instantiate_video_accelerator_func)(
	uint32 version);


/*!	Finds and loads the back ends (libmedia, VideoAcceleration.cpp). */
class VideoAcceleratorRoster {
public:
	static	VideoAcceleratorRoster*	Default();

			int32				CountAccelerators();
			VideoAccelerator*	AcceleratorAt(int32 index);

	/*!	The back end to use for \a profile and \a entrypoint, after the
		setting and RENGA_VIDEO_ACCELERATION; NULL for software decoding
		in the plug-in itself.
	*/
			VideoAccelerator*	Find(VideoAccelProfile profile,
									VideoAccelEntrypoint entrypoint,
									int32 width = 0, int32 height = 0);

	//!	The choice in effect ("auto", "off" or a back end's name).
			BString				Choice() const;

	static	const char*			ProfileName(VideoAccelProfile profile);

								VideoAcceleratorRoster();
									// use Default()

private:
			void				_Load();

			BObjectList<VideoAccelerator> fAccelerators;
			bool				fLoaded;
};


}	// namespace BPrivate


#endif	// _VIDEO_ACCELERATION_H
