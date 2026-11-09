/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NETINET_TCP_FSM_H
#define _NETINET_TCP_FSM_H


/* TCP connection states, as reported in tcp_info.tcpi_state (TCP_INFO).
   The numbering is the BSD one (RFC 793 names). */

#define TCPS_CLOSED				0
#define TCPS_LISTEN				1
#define TCPS_SYN_SENT			2
#define TCPS_SYN_RECEIVED		3
#define TCPS_ESTABLISHED		4
#define TCPS_CLOSE_WAIT			5
#define TCPS_FIN_WAIT_1			6
#define TCPS_CLOSING			7
#define TCPS_LAST_ACK			8
#define TCPS_FIN_WAIT_2			9
#define TCPS_TIME_WAIT			10

#define TCP_NSTATES				11


#endif	/* _NETINET_TCP_FSM_H */
