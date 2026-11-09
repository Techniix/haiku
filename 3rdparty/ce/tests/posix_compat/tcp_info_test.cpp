/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for getsockopt(IPPROTO_TCP, TCP_INFO), over loopback:
	the structure's size and layout, states through a connection's life,
	negotiated options, the values that must be plausible once data has
	flowed, sequence numbers that match between the two ends, short
	buffers, and the error for a socket that is not TCP.

	Output format: see 3rdparty/ce/README.md.
*/


#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <sys/socket.h>
#include <unistd.h>

#include <OS.h>

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>


#define SUITE "tcp_info"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static int
get_info(int fd, tcp_info& info, socklen_t& length)
{
	memset(&info, 0xa5, sizeof(info));
	length = sizeof(info);
	return getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &length) == 0
		? 0 : errno;
}


static int
listen_socket(sockaddr_in& address)
{
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (sockaddr*)&address, &length) != 0) {
		close(listener);
		return -1;
	}
	return listener;
}


static void
test_layout()
{
	// The part shared with Linux must sit where Linux has it, or code
	// written for Linux reads the wrong fields.
	// Offsets of Linux's struct tcp_info (include/uapi/linux/tcp.h).
	bool linuxPrefix = offsetof(tcp_info, tcpi_rto) == 8
		&& offsetof(tcp_info, tcpi_snd_mss) == 16
		&& offsetof(tcp_info, tcpi_last_data_recv) == 52
		&& offsetof(tcp_info, tcpi_rtt) == 68
		&& offsetof(tcp_info, tcpi_rttvar) == 72
		&& offsetof(tcp_info, tcpi_snd_ssthresh) == 76
		&& offsetof(tcp_info, tcpi_snd_cwnd) == 80
		&& offsetof(tcp_info, tcpi_rcv_space) == 96;
	report("layout", linuxPrefix && sizeof(tcp_info) == 196,
		"rtt at %zu, snd_cwnd at %zu, rcv_space at %zu, size %zu",
		offsetof(tcp_info, tcpi_rtt), offsetof(tcp_info, tcpi_snd_cwnd),
		offsetof(tcp_info, tcpi_rcv_space), sizeof(tcp_info));
}


static void
test_states_and_values()
{
	sockaddr_in address;
	int listener = listen_socket(address);
	tcp_info info;
	socklen_t length;

	int error = get_info(listener, info, length);
	report("listen_state", error == 0 && info.tcpi_state == TCPS_LISTEN
			&& length == sizeof(info),
		"error %d, state %u (want %d), length %u", error, info.tcpi_state,
		TCPS_LISTEN, (unsigned)length);

	int unconnected = socket(AF_INET, SOCK_STREAM, 0);
	error = get_info(unconnected, info, length);
	report("closed_state", error == 0 && info.tcpi_state == TCPS_CLOSED,
		"error %d, state %u", error, info.tcpi_state);
	close(unconnected);

	int client = socket(AF_INET, SOCK_STREAM, 0);
	connect(client, (sockaddr*)&address, sizeof(address));
	int server = accept(listener, NULL, NULL);
	close(listener);

	// Move some data both ways so there are round trip samples.
	char buffer[16384];
	memset(buffer, 'x', sizeof(buffer));
	for (int i = 0; i < 20; i++) {
		send(client, buffer, sizeof(buffer), 0);
		size_t got = 0;
		while (got < sizeof(buffer)) {
			ssize_t bytes = recv(server, buffer, sizeof(buffer) - got, 0);
			if (bytes <= 0)
				break;
			got += bytes;
		}
		send(server, "ok", 2, 0);
		recv(client, buffer, 2, 0);
	}

	tcp_info clientInfo, serverInfo;
	socklen_t clientLength, serverLength;
	int clientError = get_info(client, clientInfo, clientLength);
	int serverError = get_info(server, serverInfo, serverLength);

	report("established_state", clientError == 0 && serverError == 0
			&& clientInfo.tcpi_state == TCPS_ESTABLISHED
			&& serverInfo.tcpi_state == TCPS_ESTABLISHED,
		"client %u, server %u (want %d)", clientInfo.tcpi_state,
		serverInfo.tcpi_state, TCPS_ESTABLISHED);

	// Haiku negotiates all three on loopback.
	uint8_t want = TCPI_OPT_TIMESTAMPS | TCPI_OPT_SACK | TCPI_OPT_WSCALE;
	report("options", (clientInfo.tcpi_options & want) == want,
		"options %#x (want %#x), wscale snd %u rcv %u",
		clientInfo.tcpi_options, want, clientInfo.tcpi_snd_wscale,
		clientInfo.tcpi_rcv_wscale);

	report("mss", clientInfo.tcpi_snd_mss > 0
			&& clientInfo.tcpi_snd_mss <= 65535
			&& clientInfo.tcpi_rcv_mss > 0,
		"snd_mss %u, rcv_mss %u", clientInfo.tcpi_snd_mss,
		clientInfo.tcpi_rcv_mss);

	// Loopback round trips are well under a second; the retransmission
	// timeout has Haiku's floor.
	report("rtt", clientInfo.tcpi_rtt < 1000000
			&& clientInfo.tcpi_rto >= 100000
			&& clientInfo.tcpi_rto <= 60000000,
		"rtt %u us, rttvar %u us, rto %u us", clientInfo.tcpi_rtt,
		clientInfo.tcpi_rttvar, clientInfo.tcpi_rto);

	report("windows", clientInfo.tcpi_snd_cwnd >= clientInfo.tcpi_snd_mss
			&& clientInfo.tcpi_snd_wnd > 0 && clientInfo.tcpi_rcv_space > 0,
		"cwnd %u, ssthresh %u, snd_wnd %u, rcv_space %u",
		clientInfo.tcpi_snd_cwnd, clientInfo.tcpi_snd_ssthresh,
		clientInfo.tcpi_snd_wnd, clientInfo.tcpi_rcv_space);

	// With nothing in flight, what one side sends next is what the other
	// expects next.
	report("sequence_numbers",
		clientInfo.tcpi_snd_nxt == serverInfo.tcpi_rcv_nxt
			&& serverInfo.tcpi_snd_nxt == clientInfo.tcpi_rcv_nxt,
		"client snd_nxt %u / server rcv_nxt %u, server snd_nxt %u / "
		"client rcv_nxt %u", clientInfo.tcpi_snd_nxt,
		serverInfo.tcpi_rcv_nxt, serverInfo.tcpi_snd_nxt,
		clientInfo.tcpi_rcv_nxt);

	report("unfilled_zero", clientInfo.__tcpi_unacked == 0
			&& clientInfo.__tcpi_pad[0] == 0 && clientInfo.__tcpi_pad[15] == 0,
		"reserved fields are zero, not left as the caller's 0xa5 fill");

	snooze(200000);
	get_info(client, clientInfo, clientLength);
	report("last_data_recv", clientInfo.tcpi_last_data_recv >= 150000
			&& clientInfo.tcpi_last_data_recv < 5000000,
		"%u us after 200 ms of silence", clientInfo.tcpi_last_data_recv);

	// A short buffer gets the beginning, as on Linux and FreeBSD.
	uint8_t shortBuffer[8];
	memset(shortBuffer, 0xa5, sizeof(shortBuffer));
	socklen_t shortLength = sizeof(shortBuffer);
	int shortResult = getsockopt(client, IPPROTO_TCP, TCP_INFO, shortBuffer,
		&shortLength);
	report("short_buffer", shortResult == 0 && shortLength == 8
			&& shortBuffer[0] == TCPS_ESTABLISHED,
		"result %d, length %u, state byte %u", shortResult,
		(unsigned)shortLength, shortBuffer[0]);

	// Orderly close from the client: it moves on to FIN_WAIT_2 (or
	// TIME_WAIT once the server closes too), the server to CLOSE_WAIT.
	shutdown(client, SHUT_WR);
	snooze(100000);
	get_info(client, clientInfo, clientLength);
	get_info(server, serverInfo, serverLength);
	report("closing_states", serverInfo.tcpi_state == TCPS_CLOSE_WAIT
			&& (clientInfo.tcpi_state == TCPS_FIN_WAIT_2
				|| clientInfo.tcpi_state == TCPS_FIN_WAIT_1),
		"client %u (want FIN_WAIT_2 %d), server %u (want CLOSE_WAIT %d)",
		clientInfo.tcpi_state, TCPS_FIN_WAIT_2, serverInfo.tcpi_state,
		TCPS_CLOSE_WAIT);

	close(client);
	close(server);
}


static void
test_not_tcp()
{
	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	tcp_info info;
	socklen_t length = sizeof(info);
	int result = getsockopt(udp, IPPROTO_TCP, TCP_INFO, &info, &length);
	int error = errno;
	report("udp_rejected", result < 0,
		"getsockopt on UDP -> %d (%s)", result,
		result < 0 ? strerror(error) : "accepted");
	close(udp);
}


int
main()
{
	test_layout();
	test_states_and_values();
	test_not_tcp();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
