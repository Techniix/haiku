/*-
 * This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _VIRTIO_BLK_H
#define _VIRTIO_BLK_H

/* Feature bits */
#define VIRTIO_BLK_F_BARRIER	0x0001	/* Does host support barriers? */
#define VIRTIO_BLK_F_SIZE_MAX	0x0002	/* Indicates maximum segment size */
#define VIRTIO_BLK_F_SEG_MAX	0x0004	/* Indicates maximum # of segments */
#define VIRTIO_BLK_F_GEOMETRY	0x0010	/* Legacy geometry available  */
#define VIRTIO_BLK_F_RO		0x0020	/* Disk is read-only */
#define VIRTIO_BLK_F_BLK_SIZE	0x0040	/* Block size of disk is available*/
#define VIRTIO_BLK_F_SCSI	0x0080	/* Supports scsi command passthru */
#define VIRTIO_BLK_F_FLUSH	0x0200	/* Flush command supported */
#define VIRTIO_BLK_F_TOPOLOGY	0x0400	/* Topology information is available */
#define VIRTIO_BLK_F_CONFIG_WCE 0x0800	/* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ		0x1000	/* Support more than one vq */
#define VIRTIO_BLK_F_DISCARD	0x2000	/* DISCARD is supported */
#define VIRTIO_BLK_F_WRITE_ZEROES	0x4000	/* WRITE ZEROES is supported */

#define VIRTIO_BLK_ID_BYTES	20	/* ID string length */

struct virtio_blk_config {
	/* The capacity (in 512-byte sectors). */
	uint64_t capacity;
	/* The maximum segment size (if VIRTIO_BLK_F_SIZE_MAX) */
	uint32_t size_max;
	/* The maximum number of segments (if VIRTIO_BLK_F_SEG_MAX) */
	uint32_t seg_max;
	/* Geometry of the device (if VIRTIO_BLK_F_GEOMETRY) */
	struct virtio_blk_geometry {
		uint16_t cylinders;
		uint8_t heads;
		uint8_t sectors;
	} geometry;

	/* Block size of device (if VIRTIO_BLK_F_BLK_SIZE) */
	uint32_t blk_size;

	/* Topology of the device (if VIRTIO_BLK_F_TOPOLOGY) */
	struct virtio_blk_topology {
		uint8_t physical_block_exp;
		uint8_t alignment_offset;
		uint16_t min_io_size;
		uint32_t opt_io_size;
	} topology;

	/* Writeback mode (if VIRTIO_BLK_F_CONFIG_WCE) */
	uint8_t writeback;
	uint8_t unused0;

	/* Number of queues (if VIRTIO_BLK_F_MQ) */
	uint16_t num_queues;

	/* Discard limits, all in 512-byte sectors (if VIRTIO_BLK_F_DISCARD) */
	uint32_t max_discard_sectors;
	uint32_t max_discard_seg;
	uint32_t discard_sector_alignment;

	/* Write zeroes limits (if VIRTIO_BLK_F_WRITE_ZEROES) */
	uint32_t max_write_zeroes_sectors;
	uint32_t max_write_zeroes_seg;
	uint8_t write_zeroes_may_unmap;
	uint8_t unused1[3];
} __attribute__((packed));

/* Size of the configuration before the fields added for discard and write
   zeroes; older devices may not provide more. */
#define VIRTIO_BLK_CONFIG_BASE_SIZE	33

/*
 * Command types
 *
 * Usage is a bit tricky as some bits are used as flags and some are not.
 *
 * Rules:
 *   VIRTIO_BLK_T_OUT may be combined with VIRTIO_BLK_T_SCSI_CMD or
 *   VIRTIO_BLK_T_BARRIER.  VIRTIO_BLK_T_FLUSH is a command of its own
 *   and may not be combined with any of the other flags.
 */

/* These two define direction. */
#define VIRTIO_BLK_T_IN		0
#define VIRTIO_BLK_T_OUT	1

/* This bit says it's a scsi command, not an actual read or write. */
#define VIRTIO_BLK_T_SCSI_CMD	2

/* Cache flush command */
#define VIRTIO_BLK_T_FLUSH	4

/* Get device ID command */
#define VIRTIO_BLK_T_GET_ID	8

/* Discard and write zeroes commands (payload: virtio_blk_discard_write_zeroes
   segments) */
#define VIRTIO_BLK_T_DISCARD		11
#define VIRTIO_BLK_T_WRITE_ZEROES	13

/* Barrier before this op. */
#define VIRTIO_BLK_T_BARRIER	0x80000000

/* ID string length */
#define VIRTIO_BLK_ID_BYTES	20

/* This is the first element of the read scatter-gather list. */
struct virtio_blk_outhdr {
	/* VIRTIO_BLK_T* */
	uint32_t type;
	/* io priority. */
	uint32_t ioprio;
	/* Sector (ie. 512 byte offset) */
	uint64_t sector;
};

/* One range of a DISCARD or WRITE_ZEROES request. */
struct virtio_blk_discard_write_zeroes {
	uint64_t sector;
	uint32_t num_sectors;
	uint32_t flags;
} __attribute__((packed));

/* flags of virtio_blk_discard_write_zeroes */
#define VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP	0x1

struct virtio_scsi_inhdr {
	uint32_t errors;
	uint32_t data_len;
	uint32_t sense_len;
	uint32_t residual;
};

/* And this is the final byte of the write scatter-gather list. */
#define VIRTIO_BLK_S_OK		0
#define VIRTIO_BLK_S_IOERR	1
#define VIRTIO_BLK_S_UNSUPP	2

#endif /* _VIRTIO_BLK_H */
