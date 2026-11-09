/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test of the UDP socket setup QUIC stacks do (Chromium's
	UDPSocketPosix and QUIC packet reader/writer, quiche, ngtcp2):

	 - SO_RCVBUF/SO_SNDBUF of a megabyte or more;
	 - "do not fragment": IP_DONTFRAG / IPV6_DONTFRAG (BSD) or
	   IP_MTU_DISCOVER / IPV6_MTU_DISCOVER = *_PMTUDISC_DO (Linux), with
	   EMSGSIZE for a datagram above the MTU;
	 - ECN: IP_TOS / IPV6_TCLASS to send ECT(0), a per-packet IP_TOS /
	   IPV6_TCLASS control message in sendmsg(), and IP_RECVTOS /
	   IPV6_RECVTCLASS to receive the codepoint in recvmsg();
	 - IP_PKTINFO / IPV6_RECVPKTINFO: destination address and interface of
	   each received datagram;
	 - recvmsg() with all of these at once, and with a control buffer too
	   small (MSG_CTRUNC);
	 - an unknown control message in sendmsg() refused with EINVAL.

	The "do not fragment" checks lower loopback's MTU to 1500 for a moment
	(loopback's own 64 KiB is above any datagram) and restore it.

	Over loopback for IPv4 and, if ::1 is configured, IPv6.
	Output format: see 3rdparty/ce/README.md.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <unistd.h>


#define SUITE "quic_udp"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
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
set_int(int fd, int level, int option, int value)
{
	return setsockopt(fd, level, option, &value, sizeof(value)) == 0
		? 0 : errno;
}


static int
get_int(int fd, int level, int option, int* _value)
{
	socklen_t length = sizeof(*_value);
	return getsockopt(fd, level, option, _value, &length) == 0 ? 0 : errno;
}


struct Family {
	const char*	name;
	int			domain;
	int			level;
	int			recvTos;
	int			tos;
	int			tosMessage;
		// cmsg_type the codepoint arrives as
	int			pktinfo;
	int			pktinfoMessage;
	int			dontFrag;
	int			mtuDiscover;
};

static const Family kIPv4 = { "ipv4", AF_INET, IPPROTO_IP, IP_RECVTOS,
	IP_TOS, IP_RECVTOS, IP_PKTINFO, IP_PKTINFO, IP_DONTFRAG,
	IP_MTU_DISCOVER };
static const Family kIPv6 = { "ipv6", AF_INET6, IPPROTO_IPV6,
	IPV6_RECVTCLASS, IPV6_TCLASS, IPV6_TCLASS, IPV6_RECVPKTINFO,
	IPV6_PKTINFO, IPV6_DONTFRAG, IPV6_MTU_DISCOVER };


static socklen_t
loopback(const Family& family, struct sockaddr_storage& address)
{
	memset(&address, 0, sizeof(address));
	if (family.domain == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_len = sizeof(*in);
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		return sizeof(*in);
	}
	struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
	in6->sin6_len = sizeof(*in6);
	in6->sin6_family = AF_INET6;
	in6->sin6_addr = in6addr_loopback;
	return sizeof(*in6);
}


static void
check_option(const char* name, int fd, int level, int option, int value)
{
	int setError = set_int(fd, level, option, value);
	int readBack = -1;
	int getError = get_int(fd, level, option, &readBack);
	report(name, setError == 0 && getError == 0 && readBack == value,
		"set %s, get %s, value %d (want %d)", strerror(setError),
		strerror(getError), readBack, value);
}


struct Received {
	ssize_t		bytes;
	int			error;
	int			flags;
	int			codepoint;
	bool		haveCodepoint;
	bool		havePktinfo;
	unsigned	ifindex;
	bool		pktinfoAddressOK;
	int			messages;
};


static Received
receive(const Family& family, int fd, size_t controlSize = 256)
{
	Received result = {};
	result.codepoint = -1;
	char data[2048];
	char control[512];
	struct iovec vec = { data, sizeof(data) };
	struct msghdr header = {};
	header.msg_iov = &vec;
	header.msg_iovlen = 1;
	header.msg_control = control;
	header.msg_controllen = controlSize;

	result.bytes = recvmsg(fd, &header, 0);
	result.error = result.bytes < 0 ? errno : 0;
	result.flags = header.msg_flags;
	if (result.bytes < 0)
		return result;

	for (struct cmsghdr* message = CMSG_FIRSTHDR(&header); message != NULL;
			message = CMSG_NXTHDR(&header, message)) {
		result.messages++;
		if (message->cmsg_level != family.level)
			continue;
		if (message->cmsg_type == family.tosMessage) {
			result.haveCodepoint = true;
			if (family.domain == AF_INET)
				result.codepoint = *(unsigned char*)CMSG_DATA(message);
			else
				memcpy(&result.codepoint, CMSG_DATA(message), sizeof(int));
		} else if (message->cmsg_type == family.pktinfoMessage) {
			result.havePktinfo = true;
			if (family.domain == AF_INET) {
				struct in_pktinfo info;
				memcpy(&info, CMSG_DATA(message), sizeof(info));
				result.ifindex = info.ipi_ifindex;
				result.pktinfoAddressOK = info.ipi_addr.s_addr
					== htonl(INADDR_LOOPBACK);
			} else {
				struct in6_pktinfo info;
				memcpy(&info, CMSG_DATA(message), sizeof(info));
				result.ifindex = info.ipi6_ifindex;
				result.pktinfoAddressOK = memcmp(&info.ipi6_addr,
					&in6addr_loopback, sizeof(struct in6_addr)) == 0;
			}
		}
	}
	return result;
}


static ssize_t
send_with_codepoint(const Family& family, int fd,
	const struct sockaddr_storage& to, socklen_t toLength, int codepoint)
{
	char data[] = "quic";
	struct iovec vec = { data, sizeof(data) };
	char control[CMSG_SPACE(sizeof(int))] = {};
	struct msghdr header = {};
	header.msg_name = (void*)&to;
	header.msg_namelen = toLength;
	header.msg_iov = &vec;
	header.msg_iovlen = 1;
	header.msg_control = control;
	header.msg_controllen = sizeof(control);
	struct cmsghdr* message = CMSG_FIRSTHDR(&header);
	message->cmsg_level = family.level;
	message->cmsg_type = family.tos;
	message->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(message), &codepoint, sizeof(int));
	return sendmsg(fd, &header, 0);
}


static void
test_family(const Family& family)
{
	char name[64];
	int receiver = socket(family.domain, SOCK_DGRAM, 0);
	int sender = socket(family.domain, SOCK_DGRAM, 0);
	struct sockaddr_storage address;
	socklen_t length = loopback(family, address);
	if (receiver < 0 || sender < 0
		|| bind(receiver, (struct sockaddr*)&address, length) != 0) {
		snprintf(name, sizeof(name), "%s_available", family.name);
		report(name, family.domain == AF_INET6, "INFO %s loopback not "
			"available: %s", family.name, strerror(errno));
		close(receiver);
		close(sender);
		return;
	}
	getsockname(receiver, (struct sockaddr*)&address, &length);

	// Never block for long when a datagram did not arrive.
	struct timeval timeout = { 2, 0 };
	setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	// Buffer sizes: Chromium asks for 1 MiB (receive) and 1.25 MiB (send)
	// and treats failure as an error.
	int rcvError = set_int(receiver, SOL_SOCKET, SO_RCVBUF, 1048576);
	int sndError = set_int(sender, SOL_SOCKET, SO_SNDBUF, 1310720);
	int rcv = 0, snd = 0;
	get_int(receiver, SOL_SOCKET, SO_RCVBUF, &rcv);
	get_int(sender, SOL_SOCKET, SO_SNDBUF, &snd);
	snprintf(name, sizeof(name), "%s_buffer_sizes", family.name);
	report(name, rcvError == 0 && sndError == 0 && rcv >= 1048576
			&& snd >= 1310720,
		"SO_RCVBUF %s (%d), SO_SNDBUF %s (%d)", strerror(rcvError), rcv,
		strerror(sndError), snd);

	// Do not fragment, both spellings.
	snprintf(name, sizeof(name), "%s_dontfrag", family.name);
	check_option(name, sender, family.level, family.dontFrag, 1);
	snprintf(name, sizeof(name), "%s_mtu_discover", family.name);
	check_option(name, sender, family.level, family.mtuDiscover,
		IP_PMTUDISC_DO);
	int dontFrag = -1;
	get_int(sender, family.level, family.dontFrag, &dontFrag);
	snprintf(name, sizeof(name), "%s_mtu_discover_sets_dontfrag",
		family.name);
	report(name, dontFrag == 1, "%s after *_PMTUDISC_DO: %d",
		family.domain == AF_INET ? "IP_DONTFRAG" : "IPV6_DONTFRAG", dontFrag);

	// ECN and packet info on the receiver.
	snprintf(name, sizeof(name), "%s_recvtos", family.name);
	check_option(name, receiver, family.level, family.recvTos, 1);
	snprintf(name, sizeof(name), "%s_pktinfo", family.name);
	check_option(name, receiver, family.level, family.pktinfo, 1);

	// ECT(0) for every packet through the socket option.
	snprintf(name, sizeof(name), "%s_tos_option", family.name);
	check_option(name, sender, family.level, family.tos, 0x02);

	ssize_t sent = sendto(sender, "hello", 5, 0, (struct sockaddr*)&address,
		length);
	Received received = receive(family, receiver);
	snprintf(name, sizeof(name), "%s_receive_codepoint", family.name);
	report(name, sent == 5 && received.bytes == 5 && received.haveCodepoint
			&& received.codepoint == 0x02,
		"sent %zd, received %zd (%s), codepoint %s %#x (want 0x2)", sent,
		received.bytes, strerror(received.error),
		received.haveCodepoint ? "present" : "MISSING", received.codepoint);
	snprintf(name, sizeof(name), "%s_receive_pktinfo", family.name);
	report(name, received.havePktinfo && received.pktinfoAddressOK
			&& received.ifindex > 0,
		"pktinfo %s, address %s, ifindex %u",
		received.havePktinfo ? "present" : "MISSING",
		received.pktinfoAddressOK ? "loopback" : "WRONG", received.ifindex);

	// Per-packet codepoint in sendmsg() (ECT(1)) overrides the option.
	sent = send_with_codepoint(family, sender, address, length, 0x01);
	int sendError = sent < 0 ? errno : 0;
	received = receive(family, receiver);
	snprintf(name, sizeof(name), "%s_sendmsg_codepoint", family.name);
	report(name, sent == 5 && received.haveCodepoint
			&& received.codepoint == 0x01,
		"sendmsg %zd (%s), received codepoint %#x (want 0x1)", sent,
		strerror(sendError), received.codepoint);

	// The option still applies to the next packet without a message.
	sendto(sender, "again", 5, 0, (struct sockaddr*)&address, length);
	received = receive(family, receiver);
	snprintf(name, sizeof(name), "%s_option_after_sendmsg", family.name);
	report(name, received.codepoint == 0x02, "codepoint %#x (want 0x2)",
		received.codepoint);

	// An unknown control message is refused, as on Linux.
	char data[] = "x";
	struct iovec vec = { data, 1 };
	char control[CMSG_SPACE(sizeof(int))] = {};
	struct msghdr header = {};
	header.msg_name = &address;
	header.msg_namelen = length;
	header.msg_iov = &vec;
	header.msg_iovlen = 1;
	header.msg_control = control;
	header.msg_controllen = sizeof(control);
	struct cmsghdr* message = CMSG_FIRSTHDR(&header);
	message->cmsg_level = family.level;
	message->cmsg_type = 9999;
	message->cmsg_len = CMSG_LEN(sizeof(int));
	errno = 0;
	sent = sendmsg(sender, &header, 0);
	snprintf(name, sizeof(name), "%s_sendmsg_unknown_einval", family.name);
	report(name, sent == -1 && errno == EINVAL, "sendmsg %zd %s", sent,
		strerror(errno));

	// Control buffer too small: the datagram arrives, MSG_CTRUNC is set.
	sendto(sender, "small", 5, 0, (struct sockaddr*)&address, length);
	received = receive(family, receiver, CMSG_SPACE(1) - 1);
	snprintf(name, sizeof(name), "%s_ctrunc", family.name);
	report(name, received.bytes == 5 && (received.flags & MSG_CTRUNC) != 0,
		"received %zd (%s), flags %#x (want MSG_CTRUNC %#x)", received.bytes,
		strerror(received.error), received.flags, MSG_CTRUNC);

	// With DF, a datagram above the path MTU fails with EMSGSIZE; without,
	// it is fragmented and arrives. Loopback's MTU (64 KiB) is above any
	// datagram, so it is lowered to an Ethernet MTU for this.
	int interfaceSocket = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq request = {};
	strlcpy(request.ifr_name, "loop", sizeof(request.ifr_name));
	ioctl(interfaceSocket, SIOCGIFMTU, &request, sizeof(request));
	int originalMTU = request.ifr_mtu;
	request.ifr_mtu = 1500;
	int mtuResult = ioctl(interfaceSocket, SIOCSIFMTU, &request, sizeof(request));

	static char big[4000];
	errno = 0;
	sent = sendto(sender, big, sizeof(big), 0, (struct sockaddr*)&address,
		length);
	int bigError = sent < 0 ? errno : 0;
	snprintf(name, sizeof(name), "%s_dontfrag_emsgsize", family.name);
	report(name, mtuResult == 0 && sent == -1 && bigError == EMSGSIZE,
		"MTU 1500 (%s), 4000-byte datagram with DF: %zd %s",
		mtuResult == 0 ? "set" : strerror(errno), sent, strerror(bigError));

	set_int(sender, family.level, family.mtuDiscover, IP_PMTUDISC_DONT);
	get_int(sender, family.level, family.dontFrag, &dontFrag);
	sent = sendto(sender, big, sizeof(big), 0, (struct sockaddr*)&address,
		length);
	snprintf(name, sizeof(name), "%s_fragmented_without_df", family.name);
	if (family.domain == AF_INET) {
		char bigReceive[8192];
		ssize_t bigReceived = recv(receiver, bigReceive, sizeof(bigReceive),
			0);
		report(name, dontFrag == 0 && sent == (ssize_t)sizeof(big)
				&& bigReceived == (ssize_t)sizeof(big),
			"DF now %d, sent %zd, received %zd", dontFrag, sent, bigReceived);
	} else {
		// IPv6 fragment reassembly does not work in Haiku's stack (see the
		// F-43 notes in RENGA_MASTER_ROADMAP.md); QUIC never fragments. Only
		// check that the send is no longer refused.
		report(name, dontFrag == 0 && sent == (ssize_t)sizeof(big),
			"DF now %d, sent %zd (reception not checked: IPv6 reassembly is "
			"a known gap)", dontFrag, sent);
	}

	request.ifr_mtu = originalMTU > 0 ? originalMTU : 65536;
	ioctl(interfaceSocket, SIOCSIFMTU, &request, sizeof(request));
	close(interfaceSocket);

	// Options off again: no control messages.
	set_int(receiver, family.level, family.recvTos, 0);
	set_int(receiver, family.level, family.pktinfo, 0);
	sendto(sender, "plain", 5, 0, (struct sockaddr*)&address, length);
	received = receive(family, receiver);
	snprintf(name, sizeof(name), "%s_options_off", family.name);
	report(name, received.bytes == 5 && received.messages == 0,
		"received %zd with %d control messages", received.bytes,
		received.messages);

	close(sender);
	close(receiver);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_family(kIPv4);
	test_family(kIPv6);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
