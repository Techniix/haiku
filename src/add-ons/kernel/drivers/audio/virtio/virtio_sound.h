/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Wire format of the virtio sound device (virtio 1.2, section 5.14).
 *
 * Everything in here is little-endian on the wire. Only the parts of the
 * protocol that this driver uses are declared; jack remapping, channel maps
 * and control elements (VIRTIO_SND_F_CTLS) are left out on purpose.
 *
 * The layouts must match the specification byte for byte, because the device
 * validates request sizes (QEMU, for example, answers VIRTIO_SND_S_BAD_MSG
 * when a SET_PARAMS request is not exactly 24 bytes). The static_asserts at
 * the end of this file keep the compiler honest about that.
 */
#ifndef VIRTIO_SOUND_H
#define VIRTIO_SOUND_H


#include <SupportDefs.h>


// Device configuration space (read with read_device_config()).
// A fourth field, "controls", only exists with VIRTIO_SND_F_CTLS, which this
// driver does not negotiate, so it is not read.
struct virtio_snd_config {
	uint32	jacks;
	uint32	streams;
	uint32	chmaps;
} _PACKED;


// Virtqueue indices
enum {
	VIRTIO_SND_VQ_CONTROL	= 0,
	VIRTIO_SND_VQ_EVENT		= 1,
	VIRTIO_SND_VQ_TX		= 2,
	VIRTIO_SND_VQ_RX		= 3,
	VIRTIO_SND_VQ_MAX		= 4
};


// Stream directions
enum {
	VIRTIO_SND_D_OUTPUT	= 0,
	VIRTIO_SND_D_INPUT	= 1
};


// Request codes and status codes
enum {
	VIRTIO_SND_R_JACK_INFO			= 1,
	VIRTIO_SND_R_JACK_REMAP			= 2,

	VIRTIO_SND_R_PCM_INFO			= 0x0100,
	VIRTIO_SND_R_PCM_SET_PARAMS		= 0x0101,
	VIRTIO_SND_R_PCM_PREPARE		= 0x0102,
	VIRTIO_SND_R_PCM_RELEASE		= 0x0103,
	VIRTIO_SND_R_PCM_START			= 0x0104,
	VIRTIO_SND_R_PCM_STOP			= 0x0105,

	VIRTIO_SND_R_CHMAP_INFO			= 0x0200,

	VIRTIO_SND_S_OK					= 0x8000,
	VIRTIO_SND_S_BAD_MSG			= 0x8001,
	VIRTIO_SND_S_NOT_SUPP			= 0x8002,
	VIRTIO_SND_S_IO_ERR				= 0x8003
};


// Sample formats; these are bit indices into virtio_snd_pcm_info::formats.
enum {
	VIRTIO_SND_PCM_FMT_IMA_ADPCM	= 0,
	VIRTIO_SND_PCM_FMT_MU_LAW		= 1,
	VIRTIO_SND_PCM_FMT_A_LAW		= 2,
	VIRTIO_SND_PCM_FMT_S8			= 3,
	VIRTIO_SND_PCM_FMT_U8			= 4,
	VIRTIO_SND_PCM_FMT_S16			= 5,
	VIRTIO_SND_PCM_FMT_U16			= 6,
	VIRTIO_SND_PCM_FMT_S18_3		= 7,
	VIRTIO_SND_PCM_FMT_U18_3		= 8,
	VIRTIO_SND_PCM_FMT_S20_3		= 9,
	VIRTIO_SND_PCM_FMT_U20_3		= 10,
	VIRTIO_SND_PCM_FMT_S24_3		= 11,
	VIRTIO_SND_PCM_FMT_U24_3		= 12,
	VIRTIO_SND_PCM_FMT_S20			= 13,
	VIRTIO_SND_PCM_FMT_U20			= 14,
	VIRTIO_SND_PCM_FMT_S24			= 15,
	VIRTIO_SND_PCM_FMT_U24			= 16,
	VIRTIO_SND_PCM_FMT_S32			= 17,
	VIRTIO_SND_PCM_FMT_U32			= 18,
	VIRTIO_SND_PCM_FMT_FLOAT		= 19,
	VIRTIO_SND_PCM_FMT_FLOAT64		= 20
};


// Frame rates; these are bit indices into virtio_snd_pcm_info::rates.
enum {
	VIRTIO_SND_PCM_RATE_5512		= 0,
	VIRTIO_SND_PCM_RATE_8000		= 1,
	VIRTIO_SND_PCM_RATE_11025		= 2,
	VIRTIO_SND_PCM_RATE_16000		= 3,
	VIRTIO_SND_PCM_RATE_22050		= 4,
	VIRTIO_SND_PCM_RATE_32000		= 5,
	VIRTIO_SND_PCM_RATE_44100		= 6,
	VIRTIO_SND_PCM_RATE_48000		= 7,
	VIRTIO_SND_PCM_RATE_64000		= 8,
	VIRTIO_SND_PCM_RATE_88200		= 9,
	VIRTIO_SND_PCM_RATE_96000		= 10,
	VIRTIO_SND_PCM_RATE_176400		= 11,
	VIRTIO_SND_PCM_RATE_192000		= 12,
	VIRTIO_SND_PCM_RATE_384000		= 13
};


// Common request/response header
struct virtio_snd_hdr {
	uint32	code;
} _PACKED;


// Generic "give me info about items start_id .. start_id + count - 1" query
struct virtio_snd_query_info {
	struct virtio_snd_hdr	hdr;
	uint32					start_id;
	uint32					count;
	uint32					size;	// size of one info item in the reply
} _PACKED;


// Header shared by every info item
struct virtio_snd_info {
	uint32	hda_fn_nid;
} _PACKED;


// Header of every PCM control request
struct virtio_snd_pcm_hdr {
	struct virtio_snd_hdr	hdr;
	uint32					stream_id;
} _PACKED;


// One reply item of VIRTIO_SND_R_PCM_INFO
struct virtio_snd_pcm_info {
	struct virtio_snd_info	hdr;
	uint32					features;
	uint64					formats;	// bit set of VIRTIO_SND_PCM_FMT_*
	uint64					rates;		// bit set of VIRTIO_SND_PCM_RATE_*
	uint8					direction;	// VIRTIO_SND_D_*
	uint8					channels_min;
	uint8					channels_max;
	uint8					padding[5];
} _PACKED;


// VIRTIO_SND_R_PCM_SET_PARAMS request
struct virtio_snd_pcm_set_params {
	struct virtio_snd_pcm_hdr	hdr;
	uint32						buffer_bytes;	// multiple of period_bytes
	uint32						period_bytes;
	uint32						features;
	uint8						channels;
	uint8						format;			// VIRTIO_SND_PCM_FMT_*
	uint8						rate;			// VIRTIO_SND_PCM_RATE_*
	uint8						padding;
} _PACKED;


// Header that precedes the PCM data of every I/O message on the TX/RX queue
struct virtio_snd_pcm_xfer {
	uint32	stream_id;
} _PACKED;


// Device-writable trailer of every I/O message on the TX/RX queue
struct virtio_snd_pcm_status {
	uint32	status;			// VIRTIO_SND_S_*
	uint32	latency_bytes;
} _PACKED;


static_assert(sizeof(virtio_snd_config) == 12, "bad virtio_snd_config");
static_assert(sizeof(virtio_snd_hdr) == 4, "bad virtio_snd_hdr");
static_assert(sizeof(virtio_snd_query_info) == 16,
	"bad virtio_snd_query_info");
static_assert(sizeof(virtio_snd_pcm_hdr) == 8, "bad virtio_snd_pcm_hdr");
static_assert(sizeof(virtio_snd_pcm_info) == 32, "bad virtio_snd_pcm_info");
static_assert(sizeof(virtio_snd_pcm_set_params) == 24,
	"bad virtio_snd_pcm_set_params");
static_assert(sizeof(virtio_snd_pcm_xfer) == 4, "bad virtio_snd_pcm_xfer");
static_assert(sizeof(virtio_snd_pcm_status) == 8,
	"bad virtio_snd_pcm_status");


#endif	// VIRTIO_SOUND_H
