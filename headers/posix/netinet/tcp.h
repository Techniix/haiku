/*
 * Copyright 2006-2012 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NETINET_TCP_H
#define NETINET_TCP_H


#include <endian.h>
#include <stdint.h>


struct tcphdr {
	uint16_t	th_sport;	/* source port */
	uint16_t	th_dport;	/* destination port */
	uint32_t	th_seq;
	uint32_t	th_ack;

#if BIG_ENDIAN
	uint8_t		th_off : 4;	/* data offset */
	uint8_t		th_x2 : 4;	/* unused */
#else
	uint8_t		th_x2 : 4;
	uint8_t		th_off : 4;
#endif
	uint8_t		th_flags;
	uint16_t	th_win;
	uint16_t	th_sum;
	uint16_t	th_urp;		/* end of urgent data offset */
} _PACKED;

#if 0
#define	TH_FIN	0x01
#define	TH_SYN	0x02
#define	TH_RST	0x04
#define	TH_PUSH	0x08
#define	TH_ACK	0x10
#define	TH_URG	0x20
#define	TH_ECE	0x40
#define	TH_CWR	0x80

#define	TCPOPT_EOL				0
#define	TCPOPT_NOP				1
#define	TCPOPT_MAXSEG			2
#define	TCPOPT_WINDOW			3
#define	TCPOPT_SACK_PERMITTED	4
#define	TCPOPT_SACK				5
#define	TCPOPT_TIMESTAMP		8
#define	TCPOPT_SIGNATURE		19

#define	MAX_TCPOPTLEN			40
	/* absolute maximum TCP options length */

#define	TCPOLEN_MAXSEG			4
#define	TCPOLEN_WINDOW			3
#define	TCPOLEN_SACK			8
#define	TCPOLEN_SACK_PERMITTED	2
#define	TCPOLEN_TIMESTAMP		10
#define	TCPOLEN_TSTAMP_APPA		(TCPOLEN_TIMESTAMP + 2)
	/* see RFC 1323, appendix A */
#define	TCPOLEN_SIGNATURE		18

#define TCPOPT_TSTAMP_HDR \
    (TCPOPT_NOP << 24 | TCPOPT_NOP << 16 | TCPOPT_TIMESTAMP << 8 | TCPOLEN_TIMESTAMP)

#define TCP_MSS					536
#define TCP_MAXWIN				65535
#define TCP_MAX_WINSHIFT		14

#endif

/* options that can be set using setsockopt() and level IPPROTO_TCP */

#define	TCP_NODELAY				0x01
	/* don't delay sending smaller packets */
#define	TCP_MAXSEG				0x02
	/* set maximum segment size */
#define TCP_NOPUSH				0x04
	/* don't use TH_PUSH */
#define TCP_NOOPT				0x08
	/* don't use any TCP options */
/* Keepalive tuning (FreeBSD values; Linux uses other numbers, but code
   uses the names). In seconds, except TCP_KEEPCNT (probes). They apply to
   connections with SO_KEEPALIVE set. */
#define TCP_KEEPIDLE			0x100
	/* idle time before the first probe (default 7200) */
#define TCP_KEEPINTVL			0x200
	/* time between probes (default 75) */
#define TCP_KEEPCNT				0x400
	/* unanswered probes before the connection is dropped (default 9) */

#define TCP_INFO				0x20
	/* read-only: struct tcp_info below (FreeBSD's option value) */

/* tcpi_options bits */
#define TCPI_OPT_TIMESTAMPS		0x01
#define TCPI_OPT_SACK			0x02
#define TCPI_OPT_WSCALE			0x04
#define TCPI_OPT_ECN			0x08

/* Connection details, read with getsockopt(fd, IPPROTO_TCP, TCP_INFO, ...).
   The layout is FreeBSD's, whose first part (up to tcpi_rcv_space) has the
   same layout as Linux's, so code reading tcpi_rtt, tcpi_rttvar,
   tcpi_snd_mss, tcpi_snd_cwnd or tcpi_rcv_space works unchanged. Units
   follow FreeBSD: times in microseconds, windows and thresholds in bytes
   (Linux counts tcpi_snd_cwnd in segments), tcpi_state is a TCPS_* value
   from <netinet/tcp_fsm.h>. Fields starting with __ are not filled in and
   read as zero. A shorter buffer receives the beginning of the structure. */
struct tcp_info {
	uint8_t		tcpi_state;
	uint8_t		__tcpi_ca_state;
	uint8_t		__tcpi_retransmits;
	uint8_t		__tcpi_probes;
	uint8_t		__tcpi_backoff;
	uint8_t		tcpi_options;			/* TCPI_OPT_* */
	uint8_t		tcpi_snd_wscale : 4,	/* window scale shift, send */
				tcpi_rcv_wscale : 4;	/* window scale shift, receive */

	uint32_t	tcpi_rto;				/* retransmission timeout, usec */
	uint32_t	__tcpi_ato;
	uint32_t	tcpi_snd_mss;
	uint32_t	tcpi_rcv_mss;

	uint32_t	__tcpi_unacked;
	uint32_t	__tcpi_sacked;
	uint32_t	__tcpi_lost;
	uint32_t	__tcpi_retrans;
	uint32_t	__tcpi_fackets;

	uint32_t	__tcpi_last_data_sent;
	uint32_t	__tcpi_last_ack_sent;
	uint32_t	tcpi_last_data_recv;	/* usec since a segment arrived */
	uint32_t	__tcpi_last_ack_recv;

	uint32_t	__tcpi_pmtu;
	uint32_t	__tcpi_rcv_ssthresh;
	uint32_t	tcpi_rtt;				/* smoothed round trip time, usec */
	uint32_t	tcpi_rttvar;			/* round trip time variation, usec */
	uint32_t	tcpi_snd_ssthresh;		/* slow start threshold, bytes */
	uint32_t	tcpi_snd_cwnd;			/* congestion window, bytes */
	uint32_t	__tcpi_advmss;
	uint32_t	__tcpi_reordering;

	uint32_t	__tcpi_rcv_rtt;
	uint32_t	tcpi_rcv_space;			/* free receive buffer, bytes */

	/* FreeBSD extensions */
	uint32_t	tcpi_snd_wnd;			/* peer's advertised window, bytes */
	uint32_t	tcpi_snd_bwnd;			/* unused */
	uint32_t	tcpi_snd_nxt;			/* next sequence number to send */
	uint32_t	tcpi_rcv_nxt;			/* next sequence number expected */
	uint32_t	tcpi_toe_tid;			/* unused */
	uint32_t	tcpi_snd_rexmitpack;	/* not counted yet, zero */
	uint32_t	tcpi_rcv_ooopack;		/* not counted yet, zero */
	uint32_t	tcpi_snd_zerowin;		/* not counted yet, zero */

	uint32_t	__tcpi_pad[16];			/* room to grow */
};

#endif	/* NETINET_TCP_H */
