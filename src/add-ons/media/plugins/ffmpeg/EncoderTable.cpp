/*
 * Copyright 2009-2010 Stephan Aßmus <superstippi@gmx.de>
 * All rights reserved. Distributed under the terms of the MIT license.
 */


#include "EncoderTable.h"


const EncoderDescription gEncoderTable[] = {
	// Video codecs
	{
		{
			"MPEG-4 video",
			"mpeg4",
			0,
			AV_CODEC_ID_MPEG4,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY, // TODO: Hm, actually not really /any/ family...
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		23
	},
	{
		{
			"MPEG-1 video",
			"mpeg1video",
			0,
			AV_CODEC_ID_MPEG1VIDEO,
			{ 0 }
		},
		B_MPEG_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		10
	},
	{
		{
			"MPEG-2 video",
			"mpeg2video",
			0,
			AV_CODEC_ID_MPEG2VIDEO,
			{ 0 }
		},
		B_MPEG_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		15
	},
	{
		{
			"Theora video",
			"theora",
			0,
			AV_CODEC_ID_THEORA,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		18
	},
	{
		{
			"DV (Digital Video)",
			"dvvideo",
			0,
			AV_CODEC_ID_DVVIDEO,
			{ 0 }
		},
		B_MISC_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		5
	},
	{
		{
			"MJPEG (Motion JPEG)",
			"mjpeg",
			0,
			AV_CODEC_ID_MJPEG,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		5
	},

	// Renga (F-31 phase 4): the encoders of the build's FFmpeg (libvpx,
	// libopus, libvorbis, LAME), which WebM, Ogg and MP3 files need.
	{
		{
			"VP8 video",
			"vp8",
			0,
			AV_CODEC_ID_VP8,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		60
	},
	{
		{
			"VP9 video",
			"vp9",
			0,
			AV_CODEC_ID_VP9,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		90
	},

	// Renga (F-31 phase 4): Sorenson Spark, FFmpeg's native "flv" encoder
	// (an H.263 variant), the video codec the FLV container and therefore
	// RTMP take without an H.264 encoder (the build's FFmpeg has none).
	{
		{
			"Sorenson Spark video (FLV1)",
			"flv1",
			0,
			AV_CODEC_ID_FLV1,
			{ 0 }
		},
		B_QUICKTIME_FORMAT_FAMILY,
		B_MEDIA_RAW_VIDEO,
		B_MEDIA_ENCODED_VIDEO,
		25
	},

	// Audio codecs
	{
		{
			"Opus audio",
			"opus",
			0,
			AV_CODEC_ID_OPUS,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		12
	},
	{
		{
			"Vorbis audio",
			"vorbis",
			0,
			AV_CODEC_ID_VORBIS,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"MPEG Layer III (MP3)",
			"mp3",
			0,
			AV_CODEC_ID_MP3,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Free Lossless Audio Codec (FLAC)",
			"flac",
			0,
			AV_CODEC_ID_FLAC,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Advanced Audio Coding (AAC)",
			"aac",
			0,
			AV_CODEC_ID_AAC,
			{ 0 }
		},
		B_MPEG_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Raw audio",
			"pcm",
			0,
			0,
			{ 0 }
		},
		B_WAV_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		1
	},
	{
		{
			"Dolby Digital (AC-3)",
			"ac3",
			0,
			AV_CODEC_ID_AC3,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
#if 0
	{
		{
			"Apple Lossless Audio Codec",
			"alac",
			0,
			CODEC_ID_ALAC,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Adaptive Transform Acoustic Coding",
			"atrac",
			0,
			CODEC_ID_ATRAC3,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"MPEG Layer III (MP3)",
			"mp3",
			0,
			CODEC_ID_MP3,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Monkey's Audio",
			"ape",
			0,
			CODEC_ID_APE,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Musepack",
			"mpc",
			0,
			CODEC_ID_MUSEPACK8,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Windows Media Audio (WMA)",
			"wma",
			0,
			CODEC_ID_WMAV2,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	},
	{
		{
			"Vorbis audio",
			"vorbis",
			0,
			CODEC_ID_VORBIS,
			{ 0 }
		},
		B_ANY_FORMAT_FAMILY,
		B_MEDIA_RAW_AUDIO,
		B_MEDIA_ENCODED_AUDIO,
		10
	}
#endif
};

const size_t gEncoderCount = sizeof(gEncoderTable) / sizeof(EncoderDescription);


/*static*/ CodecID
raw_audio_codec_id_for(const media_format& format)
{
	if (format.type != B_MEDIA_RAW_AUDIO)
		return AV_CODEC_ID_NONE;

	if (format.u.raw_audio.byte_order == B_MEDIA_LITTLE_ENDIAN) {
		switch (format.u.raw_audio.format) {
			case media_raw_audio_format::B_AUDIO_FLOAT:
				return AV_CODEC_ID_PCM_F32LE;
			case media_raw_audio_format::B_AUDIO_DOUBLE:
				return AV_CODEC_ID_PCM_F64LE;
			case media_raw_audio_format::B_AUDIO_INT:
				return AV_CODEC_ID_PCM_S32LE;
			case media_raw_audio_format::B_AUDIO_SHORT:
				return AV_CODEC_ID_PCM_S16LE;
			case media_raw_audio_format::B_AUDIO_UCHAR:
				return AV_CODEC_ID_PCM_U8;
			default:
				return AV_CODEC_ID_NONE;
		}
	} else {
		switch (format.u.raw_audio.format) {
			case media_raw_audio_format::B_AUDIO_FLOAT:
				return AV_CODEC_ID_PCM_F32BE;
			case media_raw_audio_format::B_AUDIO_DOUBLE:
				return AV_CODEC_ID_PCM_F64BE;
			case media_raw_audio_format::B_AUDIO_INT:
				return AV_CODEC_ID_PCM_S32BE;
			case media_raw_audio_format::B_AUDIO_SHORT:
				return AV_CODEC_ID_PCM_S16BE;
			case media_raw_audio_format::B_AUDIO_UCHAR:
				return AV_CODEC_ID_PCM_U8;
			default:
				return AV_CODEC_ID_NONE;
		}
	}
}



