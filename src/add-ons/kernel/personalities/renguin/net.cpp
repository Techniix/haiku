/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux sockets on Haiku's network stack (roadmap F-27, Renguin networking).
 *
 * The socket calls map onto Haiku's own (src/system/kernel/fs/socket.cpp)
 * through their _user_* entry points, with Linux structures translated into
 * scratch space on the caller's user stack (usermem.cpp). What differs
 * between the two ABIs, and is translated here, is what FreeBSD's
 * Linuxulator translates in sys/compat/linux/linux_socket.c:
 *
 *  - socket addresses: Linux has a 16-bit sa_family and no sa_len; the
 *    address family numbers differ (AF_UNIX 1/9, AF_INET 2/1, AF_INET6
 *    10/5); Haiku's sockaddr_in is 32 bytes, Linux's 16. AF_UNIX paths are
 *    looked up in the Linux root like any other path (path.cpp), so a
 *    program inside it binds where it expects and connects to sockets
 *    outside it (Haiku's /tmp, where the Wayland socket lives) where the
 *    root has nothing at that path; returned paths have the root removed.
 *  - socket types and flags: SOCK_NONBLOCK/SOCK_CLOEXEC values, MSG_* flags.
 *  - socket options: level SOL_SOCKET is 1 (Haiku -1), and nearly every
 *    option number differs; IPPROTO_IP, IPPROTO_IPV6 and IPPROTO_TCP
 *    options are mapped by table; unknown ones fail with ENOPROTOOPT, as
 *    Linux does.
 *  - struct msghdr (Linux: size_t msg_iovlen and msg_controllen) and
 *    struct cmsghdr (Linux: size_t cmsg_len); SCM_RIGHTS descriptors are
 *    passed by Haiku's stack unchanged (descriptor numbers are the same in
 *    both worlds), so a Linux program can hand a memfd to a native Wayland
 *    compositor and back.
 *  - recvmmsg()/sendmmsg() as loops over recvmsg()/sendmsg().
 *
 * NETLINK_ROUTE, read-only. glibc's and musl's getifaddrs() and
 * if_nameindex(), "ip addr", "ip route" and many programs ask the kernel
 * for interfaces, addresses and routes over netlink. A netlink socket here
 * is the user end of a datagram socketpair whose other end the module keeps
 * (in the kernel team's descriptor table); a request sent on it is answered
 * at once by writing the reply messages into the kernel end, so the reply
 * is queued before send() returns (musl reads it with MSG_DONTWAIT), poll()
 * sees it, and the descriptor behaves like any socket otherwise (dup, fork,
 * close-on-exec, epoll). Answered: RTM_GETLINK, RTM_GETADDR and RTM_GETROUTE
 * (dumps and single requests), from Haiku's interface list (SIOCGIFCONF on
 * AF_LINK, AF_INET and AF_INET6 sockets) and routing tables (SIOCGRTTABLE).
 * Anything that would change the configuration gets EPERM; other requests
 * EOPNOTSUPP (in an NLMSG_ERROR reply, as Linux sends them).
 *
 * Interface names: Haiku names interfaces by device path ("/dev/net/
 * ipro1000/0") or "loop"; Linux names are at most 15 characters and
 * programs expect "lo". Linux programs see the loopback interface as "lo"
 * and the others as "eth0", "eth1", ... in Haiku's
 * index order (Wi-Fi too: Haiku's link layer reports both as Ethernet);
 * the interface indexes are Haiku's. The SIOCGIF* ioctls take
 * and return these names.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <AutoDeleter.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#include <ByteOrder.h>

#include <fs/fd.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <vfs.h>


//#define TRACE_RENGUIN_NET
#ifdef TRACE_RENGUIN_NET
#	define TRACE(x...) dprintf("renguin net: " x)
#else
#	define TRACE(x...) ;
#endif


// Linux errno values used here (asm-generic/errno.h).
#define LX_ENOTSOCK			88
#define LX_EDESTADDRREQ		89
#define LX_EMSGSIZE			90
#define LX_ENOPROTOOPT		92
#define LX_EPROTONOSUPPORT	93
#define LX_ESOCKTNOSUPPORT	94
#define LX_EAFNOSUPPORT		97
#define LX_EADDRNOTAVAIL	99
#define LX_ENOBUFS			105
#define LX_ENODEV			19

// Address families (linux/socket.h).
#define LX_AF_UNSPEC		0
#define LX_AF_UNIX			1
#define LX_AF_INET			2
#define LX_AF_INET6			10
#define LX_AF_NETLINK		16
#define LX_AF_PACKET		17

// Socket types and creation flags.
#define LX_SOCK_TYPE_MASK	0xf
#define LX_SOCK_NONBLOCK	00004000
#define LX_SOCK_CLOEXEC		02000000

// Message flags.
#define LX_MSG_OOB			0x1
#define LX_MSG_PEEK			0x2
#define LX_MSG_DONTROUTE	0x4
#define LX_MSG_CTRUNC		0x8
#define LX_MSG_TRUNC		0x20
#define LX_MSG_DONTWAIT		0x40
#define LX_MSG_EOR			0x80
#define LX_MSG_WAITALL		0x100
#define LX_MSG_NOSIGNAL		0x4000
#define LX_MSG_MORE			0x8000
#define LX_MSG_WAITFORONE	0x10000
#define LX_MSG_CMSG_CLOEXEC	0x40000000

// Levels and options.
#define LX_SOL_SOCKET		1
#define LX_SOL_IP			0
#define LX_SOL_TCP			6
#define LX_SOL_UDP			17
#define LX_SOL_IPV6			41

#define LX_SO_DEBUG			1
#define LX_SO_REUSEADDR		2
#define LX_SO_TYPE			3
#define LX_SO_ERROR			4
#define LX_SO_DONTROUTE		5
#define LX_SO_BROADCAST		6
#define LX_SO_SNDBUF		7
#define LX_SO_RCVBUF		8
#define LX_SO_KEEPALIVE		9
#define LX_SO_OOBINLINE		10
#define LX_SO_NO_CHECK		11
#define LX_SO_PRIORITY		12
#define LX_SO_LINGER		13
#define LX_SO_REUSEPORT		15
#define LX_SO_PASSCRED		16
#define LX_SO_PEERCRED		17
#define LX_SO_RCVLOWAT		18
#define LX_SO_SNDLOWAT		19
#define LX_SO_RCVTIMEO		20
#define LX_SO_SNDTIMEO		21
#define LX_SO_BINDTODEVICE	25
#define LX_SO_TIMESTAMP		29
#define LX_SO_ACCEPTCONN	30
#define LX_SO_SNDBUFFORCE	32
#define LX_SO_RCVBUFFORCE	33
#define LX_SO_PASSSEC		34
#define LX_SO_MARK			36
#define LX_SO_PROTOCOL		38
#define LX_SO_DOMAIN		39
#define LX_SO_RXQ_OVFL		40
#define LX_SO_BUSY_POLL		46
#define LX_SO_INCOMING_CPU	49

#define LX_SCM_RIGHTS		1
#define LX_SCM_CREDENTIALS	2

#define LX_IP_TOS				1
#define LX_IP_TTL				2
#define LX_IP_HDRINCL			3
#define LX_IP_OPTIONS			4
#define LX_IP_RECVOPTS			6
#define LX_IP_RETOPTS			7
#define LX_IP_PKTINFO			8
#define LX_IP_MTU_DISCOVER		10
#define LX_IP_RECVERR			11
#define LX_IP_RECVTTL			12
#define LX_IP_RECVTOS			13
#define LX_IP_FREEBIND			15
#define LX_IP_MULTICAST_IF		32
#define LX_IP_MULTICAST_TTL		33
#define LX_IP_MULTICAST_LOOP	34
#define LX_IP_ADD_MEMBERSHIP	35
#define LX_IP_DROP_MEMBERSHIP	36

#define LX_IPV6_UNICAST_HOPS	16
#define LX_IPV6_MULTICAST_IF	17
#define LX_IPV6_MULTICAST_HOPS	18
#define LX_IPV6_MULTICAST_LOOP	19
#define LX_IPV6_JOIN_GROUP		20
#define LX_IPV6_LEAVE_GROUP		21
#define LX_IPV6_MTU_DISCOVER	23
#define LX_IPV6_RECVERR			25
#define LX_IPV6_V6ONLY			26
#define LX_IPV6_RECVPKTINFO		49
#define LX_IPV6_PKTINFO			50
#define LX_IPV6_RECVHOPLIMIT	51
#define LX_IPV6_HOPLIMIT		52
#define LX_IPV6_DONTFRAG		62
#define LX_IPV6_RECVTCLASS		66
#define LX_IPV6_TCLASS			67

#define LX_TCP_NODELAY			1
#define LX_TCP_MAXSEG			2
#define LX_TCP_CORK				3
#define LX_TCP_KEEPIDLE			4
#define LX_TCP_KEEPINTVL		5
#define LX_TCP_KEEPCNT			6
#define LX_TCP_SYNCNT			7
#define LX_TCP_LINGER2			8
#define LX_TCP_DEFER_ACCEPT		9
#define LX_TCP_WINDOW_CLAMP		10
#define LX_TCP_INFO				11
#define LX_TCP_QUICKACK			12
#define LX_TCP_CONGESTION		13
#define LX_TCP_USER_TIMEOUT		18
#define LX_TCP_FASTOPEN			23
#define LX_TCP_FASTOPEN_CONNECT	30

// Socket ioctls (linux/sockios.h).
#define LX_SIOCGIFNAME		0x8910
#define LX_SIOCGIFCONF		0x8912
#define LX_SIOCGIFFLAGS		0x8913
#define LX_SIOCGIFADDR		0x8915
#define LX_SIOCGIFDSTADDR	0x8917
#define LX_SIOCGIFBRDADDR	0x8919
#define LX_SIOCGIFNETMASK	0x891b
#define LX_SIOCGIFMETRIC	0x891d
#define LX_SIOCGIFMTU		0x8921
#define LX_SIOCGIFHWADDR	0x8927
#define LX_SIOCGIFINDEX		0x8933
#define LX_SIOCGIFTXQLEN	0x8942
#define LX_SIOCATMARK		0x8905
#define LX_SIOCGSTAMP		0x8906

// Interface flags (linux/if.h).
#define LX_IFF_UP			0x1
#define LX_IFF_BROADCAST	0x2
#define LX_IFF_DEBUG		0x4
#define LX_IFF_LOOPBACK		0x8
#define LX_IFF_POINTOPOINT	0x10
#define LX_IFF_RUNNING		0x40
#define LX_IFF_NOARP		0x80
#define LX_IFF_PROMISC		0x100
#define LX_IFF_ALLMULTI		0x200
#define LX_IFF_MULTICAST	0x1000
#define LX_IFF_LOWER_UP		0x10000

#define LX_ARPHRD_ETHER		1
#define LX_ARPHRD_IEEE80211	801
#define LX_ARPHRD_LOOPBACK	772
#define LX_ARPHRD_NONE		0xfffe

#define LX_IFNAMSIZ			16


struct linux_sockaddr {
	uint16	sa_family;
	char	sa_data[14];
};

struct linux_sockaddr_in {
	uint16	sin_family;
	uint16	sin_port;
	uint32	sin_addr;
	uint8	sin_zero[8];
};

struct linux_sockaddr_in6 {
	uint16	sin6_family;
	uint16	sin6_port;
	uint32	sin6_flowinfo;
	uint8	sin6_addr[16];
	uint32	sin6_scope_id;
};

#define LX_UNIX_PATH_MAX	108
struct linux_sockaddr_un {
	uint16	sun_family;
	char	sun_path[LX_UNIX_PATH_MAX];
};

struct linux_sockaddr_nl {
	uint16	nl_family;
	uint16	nl_pad;
	uint32	nl_pid;
	uint32	nl_groups;
};

struct linux_msghdr {
	uint64	msg_name;
	uint32	msg_namelen;
	uint32	pad1;
	uint64	msg_iov;
	uint64	msg_iovlen;
	uint64	msg_control;
	uint64	msg_controllen;
	uint32	msg_flags;
	uint32	pad2;
};

struct linux_mmsghdr {
	linux_msghdr	msg_hdr;
	uint32			msg_len;
	uint32			pad;
};

struct linux_cmsghdr {
	uint64	cmsg_len;
	int32	cmsg_level;
	int32	cmsg_type;
};

#define LX_CMSG_ALIGN(length)	(((length) + 7) & ~(size_t)7)
#define LX_CMSG_HDRLEN			sizeof(linux_cmsghdr)

struct linux_ifreq {
	char	ifr_name[LX_IFNAMSIZ];
	union {
		linux_sockaddr	addr;
		int32			value;
		uint64			pointer;
		uint8			pad[24];
	};
};

struct linux_ifconf {
	int32	ifc_len;
	int32	pad;
	uint64	ifc_buf;
};

// Netlink (linux/netlink.h, linux/rtnetlink.h, linux/if_link.h,
// linux/if_addr.h).
#define LX_NETLINK_ROUTE	0

struct linux_nlmsghdr {
	uint32	nlmsg_len;
	uint16	nlmsg_type;
	uint16	nlmsg_flags;
	uint32	nlmsg_seq;
	uint32	nlmsg_pid;
};

#define LX_NLMSG_ALIGN(length)	(((length) + 3) & ~(size_t)3)
#define LX_NLMSG_HDRLEN			LX_NLMSG_ALIGN(sizeof(linux_nlmsghdr))

#define LX_NLMSG_NOOP		1
#define LX_NLMSG_ERROR		2
#define LX_NLMSG_DONE		3

#define LX_NLM_F_REQUEST	0x1
#define LX_NLM_F_MULTI		0x2
#define LX_NLM_F_ACK		0x4
#define LX_NLM_F_ROOT		0x100
#define LX_NLM_F_MATCH		0x200
#define LX_NLM_F_DUMP		(LX_NLM_F_ROOT | LX_NLM_F_MATCH)

#define LX_RTM_NEWLINK		16
#define LX_RTM_GETLINK		18
#define LX_RTM_NEWADDR		20
#define LX_RTM_GETADDR		22
#define LX_RTM_NEWROUTE		24
#define LX_RTM_GETROUTE		26
#define LX_RTM_NEWNEIGH		28
#define LX_RTM_GETNEIGH		30
#define LX_RTM_NEWRULE		32
#define LX_RTM_GETRULE		34

struct linux_ifinfomsg {
	uint8	ifi_family;
	uint8	pad;
	uint16	ifi_type;
	int32	ifi_index;
	uint32	ifi_flags;
	uint32	ifi_change;
};

struct linux_ifaddrmsg {
	uint8	ifa_family;
	uint8	ifa_prefixlen;
	uint8	ifa_flags;
	uint8	ifa_scope;
	uint32	ifa_index;
};

struct linux_rtmsg {
	uint8	rtm_family;
	uint8	rtm_dst_len;
	uint8	rtm_src_len;
	uint8	rtm_tos;
	uint8	rtm_table;
	uint8	rtm_protocol;
	uint8	rtm_scope;
	uint8	rtm_type;
	uint32	rtm_flags;
};

struct linux_rtattr {
	uint16	rta_len;
	uint16	rta_type;
};

#define LX_RTA_ALIGN(length)	(((length) + 3) & ~(size_t)3)

#define LX_IFLA_ADDRESS		1
#define LX_IFLA_BROADCAST	2
#define LX_IFLA_IFNAME		3
#define LX_IFLA_MTU			4
#define LX_IFLA_TXQLEN		13
#define LX_IFLA_OPERSTATE	16

#define LX_IFA_ADDRESS		1
#define LX_IFA_LOCAL		2
#define LX_IFA_LABEL		3
#define LX_IFA_BROADCAST	4
#define LX_IFA_FLAGS		8

#define LX_IFA_F_PERMANENT	0x80

#define LX_RTA_DST			1
#define LX_RTA_OIF			4
#define LX_RTA_GATEWAY		5
#define LX_RTA_PRIORITY		6
#define LX_RTA_PREFSRC		7
#define LX_RTA_TABLE		15

#define LX_RT_TABLE_MAIN	254
#define LX_RTPROT_KERNEL	2
#define LX_RTPROT_BOOT		3
#define LX_RT_SCOPE_UNIVERSE	0
#define LX_RT_SCOPE_HOST	254
#define LX_RT_SCOPE_LINK	253
#define LX_RTN_UNICAST		1
#define LX_RTN_LOCAL		2

#define LX_IF_OPER_UNKNOWN	0
#define LX_IF_OPER_DOWN		2
#define LX_IF_OPER_UP		6


// The kernel's own socket calls (socket.cpp: socket(), send() and
// socketpair() work on the kernel team's descriptors) report errors through
// errno; these return a status as the rest of this file expects.

static int
kernel_socket(int family, int type, int protocol)
{
	int fd = ::socket(family, type, protocol);
	return fd < 0 ? (errno < 0 ? errno : B_ERROR) : fd;
}


static ssize_t
kernel_send(int fd, const void* data, size_t length)
{
	ssize_t sent = ::send(fd, data, length, MSG_DONTWAIT);
	return sent < 0 ? (errno < 0 ? errno : B_ERROR) : sent;
}


static status_t
kernel_socketpair(int family, int type, int fds[2])
{
	if (::socketpair(family, type, 0, fds) < 0)
		return errno < 0 ? errno : B_ERROR;
	return B_OK;
}


// #pragma mark - addresses


static int
family_to_haiku(int family)
{
	switch (family) {
		case LX_AF_UNSPEC:	return AF_UNSPEC;
		case LX_AF_UNIX:	return AF_UNIX;
		case LX_AF_INET:	return AF_INET;
		case LX_AF_INET6:	return AF_INET6;
		default:			return -1;
	}
}


static int
family_from_haiku(int family)
{
	switch (family) {
		case AF_UNSPEC:	return LX_AF_UNSPEC;
		case AF_UNIX:	return LX_AF_UNIX;
		case AF_INET:	return LX_AF_INET;
		case AF_INET6:	return LX_AF_INET6;
		default:		return family;
	}
}


/*!	Reads a Linux socket address from \a userAddress and builds the Haiku
	one in \a address. AF_UNIX paths are translated into the Linux root:
	\a create allows the last component not to exist yet (bind()).
	Returns 0 or a negative Linux errno.
*/
static int64
address_from_linux(uint64 userAddress, uint32 length,
	sockaddr_storage* address, socklen_t* _length, bool create)
{
	memset(address, 0, sizeof(*address));
	if (length < sizeof(uint16) || length > 128)
		return -LX_EINVAL;

	uint8 buffer[128];
	if (renguin_copy_in(buffer, userAddress, length) != B_OK)
		return -LX_EFAULT;
	uint16 family = *(uint16*)buffer;

	switch (family) {
		case LX_AF_UNSPEC:
			address->ss_family = AF_UNSPEC;
			address->ss_len = sizeof(sockaddr);
			*_length = sizeof(sockaddr);
			return 0;

		case LX_AF_INET:
		{
			if (length < sizeof(linux_sockaddr_in))
				return -LX_EINVAL;
			linux_sockaddr_in* in = (linux_sockaddr_in*)buffer;
			sockaddr_in* out = (sockaddr_in*)address;
			out->sin_len = sizeof(sockaddr_in);
			out->sin_family = AF_INET;
			out->sin_port = in->sin_port;
			out->sin_addr.s_addr = in->sin_addr;
			*_length = sizeof(sockaddr_in);
			return 0;
		}

		case LX_AF_INET6:
		{
			if (length < offsetof(linux_sockaddr_in6, sin6_scope_id))
				return -LX_EINVAL;
			linux_sockaddr_in6* in6 = (linux_sockaddr_in6*)buffer;
			sockaddr_in6* out = (sockaddr_in6*)address;
			out->sin6_len = sizeof(sockaddr_in6);
			out->sin6_family = AF_INET6;
			out->sin6_port = in6->sin6_port;
			out->sin6_flowinfo = in6->sin6_flowinfo;
			memcpy(&out->sin6_addr, in6->sin6_addr, 16);
			out->sin6_scope_id = length >= sizeof(linux_sockaddr_in6)
				? in6->sin6_scope_id : 0;
			*_length = sizeof(sockaddr_in6);
			return 0;
		}

		case LX_AF_UNIX:
		{
			sockaddr_un* out = (sockaddr_un*)address;
			out->sun_family = AF_UNIX;
			size_t pathLength = length - offsetof(linux_sockaddr_un, sun_path);
			if (pathLength == 0) {
				// autobind / unnamed
				out->sun_len = offsetof(sockaddr_un, sun_path);
				*_length = out->sun_len;
				return 0;
			}
			const char* path = (const char*)buffer
				+ offsetof(linux_sockaddr_un, sun_path);
			if (path[0] == '\0') {
				// Abstract names (a leading NUL byte) are not supported by
				// Haiku's stack yet (roadmap F-52); D-Bus and X11 fall back
				// to path names.
				return create ? -LX_EADDRNOTAVAIL : -111 /* ECONNREFUSED */;
			}
			char* linuxPath = (char*)malloc(B_PATH_NAME_LENGTH);
			if (linuxPath == NULL)
				return -LX_ENOMEM;
			MemoryDeleter pathDeleter(linuxPath);
			size_t copied = strnlen(path, pathLength);
			memcpy(linuxPath, path, copied);
			linuxPath[copied] = '\0';
			status_t status = renguin_translate_path(linuxPath,
				B_PATH_NAME_LENGTH, create ? RENGUIN_PATH_MAY_CREATE : 0);
			if (status != B_OK && status != RENGUIN_STATUS_IN_PROC)
				return renguin_encode(status);
			size_t translated = strlen(linuxPath);
			if (translated >= sizeof(out->sun_path))
				return -LX_ENAMETOOLONG;
			memcpy(out->sun_path, linuxPath, translated + 1);
			out->sun_len = offsetof(sockaddr_un, sun_path) + translated + 1;
			*_length = out->sun_len;
			return 0;
		}

		case LX_AF_NETLINK:
		case LX_AF_PACKET:
		default:
			return -LX_EAFNOSUPPORT;
	}
}


/*!	Writes the Haiku socket address \a address (of \a length bytes) to the
	Linux program as a Linux one, truncated to the buffer's size as Linux
	does; the full length goes to \a userLength.
*/
static int64
address_to_linux(const sockaddr* address, socklen_t length,
	uint64 userAddress, uint64 userLength)
{
	if (userAddress == 0 || userLength == 0)
		return 0;

	uint32 bufferLength;
	if (renguin_copy_in(&bufferLength, userLength, sizeof(bufferLength))
			!= B_OK) {
		return -LX_EFAULT;
	}
	if ((int32)bufferLength < 0)
		return -LX_EINVAL;

	uint8 out[sizeof(linux_sockaddr_un) + 4];
	memset(out, 0, sizeof(out));
	uint32 outLength = 0;

	int family = length >= 2 ? address->sa_family : AF_UNSPEC;
	switch (family) {
		case AF_INET:
		{
			const sockaddr_in* in = (const sockaddr_in*)address;
			linux_sockaddr_in* lx = (linux_sockaddr_in*)out;
			lx->sin_family = LX_AF_INET;
			lx->sin_port = in->sin_port;
			lx->sin_addr = in->sin_addr.s_addr;
			outLength = sizeof(linux_sockaddr_in);
			break;
		}
		case AF_INET6:
		{
			const sockaddr_in6* in6 = (const sockaddr_in6*)address;
			linux_sockaddr_in6* lx = (linux_sockaddr_in6*)out;
			lx->sin6_family = LX_AF_INET6;
			lx->sin6_port = in6->sin6_port;
			lx->sin6_flowinfo = in6->sin6_flowinfo;
			memcpy(lx->sin6_addr, &in6->sin6_addr, 16);
			lx->sin6_scope_id = in6->sin6_scope_id;
			outLength = sizeof(linux_sockaddr_in6);
			break;
		}
		case AF_UNIX:
		{
			const sockaddr_un* un = (const sockaddr_un*)address;
			linux_sockaddr_un* lx = (linux_sockaddr_un*)out;
			lx->sun_family = LX_AF_UNIX;
			size_t pathRoom = length > offsetof(sockaddr_un, sun_path)
				? length - offsetof(sockaddr_un, sun_path) : 0;
			char path[sizeof(un->sun_path) + 1];
			size_t pathLength = strnlen(un->sun_path,
				min_c(pathRoom, sizeof(un->sun_path)));
			memcpy(path, un->sun_path, pathLength);
			path[pathLength] = '\0';
			renguin_host_to_linux_path(path);
			pathLength = strnlen(path, LX_UNIX_PATH_MAX - 1);
			memcpy(lx->sun_path, path, pathLength);
			// Linux reports the family plus the path and its NUL; an
			// unnamed socket is just the family.
			outLength = pathLength == 0 ? sizeof(uint16)
				: offsetof(linux_sockaddr_un, sun_path) + pathLength + 1;
			break;
		}
		default:
			((linux_sockaddr*)out)->sa_family = family_from_haiku(family);
			outLength = sizeof(uint16);
			break;
	}

	uint32 copy = min_c(bufferLength, outLength);
	if (copy > 0 && renguin_copy_out(userAddress, out, copy) != B_OK)
		return -LX_EFAULT;
	if (renguin_copy_out(userLength, &outLength, sizeof(outLength)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


static int
msg_flags_to_haiku(uint64 flags)
{
	int result = 0;
	if ((flags & LX_MSG_OOB) != 0)
		result |= MSG_OOB;
	if ((flags & LX_MSG_PEEK) != 0)
		result |= MSG_PEEK;
	if ((flags & LX_MSG_DONTROUTE) != 0)
		result |= MSG_DONTROUTE;
	if ((flags & LX_MSG_DONTWAIT) != 0)
		result |= MSG_DONTWAIT;
	if ((flags & LX_MSG_EOR) != 0)
		result |= MSG_EOR;
	if ((flags & LX_MSG_WAITALL) != 0)
		result |= MSG_WAITALL;
	if ((flags & LX_MSG_NOSIGNAL) != 0)
		result |= MSG_NOSIGNAL;
	if ((flags & LX_MSG_CMSG_CLOEXEC) != 0)
		result |= MSG_CMSG_CLOEXEC;
	if ((flags & LX_MSG_TRUNC) != 0)
		result |= MSG_TRUNC;
	// MSG_MORE (corking) and MSG_CONFIRM are hints; dropped.
	return result;
}


static uint32
msg_flags_from_haiku(int flags)
{
	uint32 result = 0;
	if ((flags & MSG_OOB) != 0)
		result |= LX_MSG_OOB;
	if ((flags & MSG_EOR) != 0)
		result |= LX_MSG_EOR;
	if ((flags & MSG_TRUNC) != 0)
		result |= LX_MSG_TRUNC;
	if ((flags & MSG_CTRUNC) != 0)
		result |= LX_MSG_CTRUNC;
	return result;
}


// #pragma mark - netlink


/*!	One netlink socket: the user's end (held by reference, so the pointer
	stays unique while the entry exists) and the kernel end the replies are
	written to. An entry whose user end has no open descriptor left is
	released the next time the table is searched.
*/
struct NetlinkSocket {
	file_descriptor*	user;
	int					kernelFD;
	uint32				portID;
	int					protocol;
};

static const int kMaxNetlinkSockets = 64;
static NetlinkSocket sNetlinkSockets[kMaxNetlinkSockets];
static mutex sNetlinkLock = MUTEX_INITIALIZER("renguin netlink");
static int32 sNetlinkCount = 0;
	// entries in use: read() and write() look only when there are any


static void
netlink_collect_locked()
{
	for (int i = 0; i < kMaxNetlinkSockets; i++) {
		NetlinkSocket& socket = sNetlinkSockets[i];
		if (socket.user != NULL && atomic_get(&socket.user->open_count) <= 0) {
			put_fd(socket.user);
			_kern_close(socket.kernelFD);
			socket.user = NULL;
			atomic_add(&sNetlinkCount, -1);
		}
	}
}


/*!	Returns the netlink entry for the caller's \a fd with sNetlinkLock held,
	or NULL (and the lock not held).
*/
static NetlinkSocket*
netlink_lookup(int fd)
{
	file_descriptor* descriptor = get_fd(get_current_io_context(false), fd);
	if (descriptor == NULL)
		return NULL;
	mutex_lock(&sNetlinkLock);
	for (int i = 0; i < kMaxNetlinkSockets; i++) {
		if (sNetlinkSockets[i].user == descriptor) {
			put_fd(descriptor);
			return &sNetlinkSockets[i];
		}
	}
	mutex_unlock(&sNetlinkLock);
	put_fd(descriptor);
	return NULL;
}


static bool
is_netlink(int fd)
{
	NetlinkSocket* socket = netlink_lookup(fd);
	if (socket == NULL)
		return false;
	mutex_unlock(&sNetlinkLock);
	return true;
}


static int64
netlink_socket(int protocol, uint64 flags)
{
	if (protocol != LX_NETLINK_ROUTE)
		return -LX_EPROTONOSUPPORT;

	int fds[2];
	status_t status = kernel_socketpair(AF_UNIX, SOCK_DGRAM, fds);
	if (status != B_OK)
		return renguin_encode(status);

	io_context* kernelContext = get_current_io_context(true);
	io_context* userContext = get_current_io_context(false);

	// Move fds[0] into the caller's table.
	file_descriptor* descriptor = get_fd(kernelContext, fds[0]);
	if (descriptor == NULL) {
		_kern_close(fds[0]);
		_kern_close(fds[1]);
		return -LX_ENOMEM;
	}
	inc_fd_open_count(descriptor);
	int fd = new_fd(userContext, descriptor);
	if (fd < 0) {
		close_fd(kernelContext, descriptor);
		put_fd(descriptor);
		_kern_close(fds[0]);
		_kern_close(fds[1]);
		return renguin_encode(fd);
	}
	_kern_close(fds[0]);
		// the caller's slot now holds the reference get_fd() took and the
		// open count added above

	if ((flags & LX_SOCK_CLOEXEC) != 0)
		_user_fcntl(fd, F_SETFD, FD_CLOEXEC);
	if ((flags & LX_SOCK_NONBLOCK) != 0)
		_user_fcntl(fd, F_SETFL, O_NONBLOCK);
	// Replies are written without blocking; a program that never reads
	// loses them (ENOBUFS on Linux).
	_kern_fcntl(fds[1], F_SETFL, O_NONBLOCK);

	MutexLocker locker(sNetlinkLock);
	netlink_collect_locked();
	for (int i = 0; i < kMaxNetlinkSockets; i++) {
		NetlinkSocket& socket = sNetlinkSockets[i];
		if (socket.user != NULL)
			continue;
		inc_fd_ref_count(descriptor);
		socket.user = descriptor;
		atomic_add(&sNetlinkCount, 1);
		socket.kernelFD = fds[1];
		socket.portID = 0;
		socket.protocol = protocol;
		TRACE("netlink socket %d (kernel %d)\n", fd, fds[1]);
		return fd;
	}
	locker.Unlock();

	_kern_close(fds[1]);
	_user_close(fd);
	return -LX_ENOBUFS;
}


// Interfaces as Linux programs see them.

struct NetInterface {
	uint32	index;
	char	name[LX_IFNAMSIZ];
	char	haikuName[IF_NAMESIZE];
	uint32	flags;			// Linux IFF_*
	uint32	mtu;
	uint16	type;			// ARPHRD_*
	uint8	hardwareAddress[16];
	uint8	hardwareLength;
};

struct NetAddress {
	uint32	index;
	uint8	family;			// Linux AF_*
	uint8	prefixLength;
	uint8	address[16];
	uint8	broadcast[4];
	bool	hasBroadcast;
};

static const int kMaxInterfaces = 16;


static uint32
interface_flags_from_haiku(int flags)
{
	uint32 result = 0;
	if ((flags & IFF_UP) != 0)
		result |= LX_IFF_UP;
	if ((flags & IFF_BROADCAST) != 0)
		result |= LX_IFF_BROADCAST;
	if ((flags & IFF_LOOPBACK) != 0)
		result |= LX_IFF_LOOPBACK | LX_IFF_RUNNING | LX_IFF_LOWER_UP;
	if ((flags & IFF_POINTOPOINT) != 0)
		result |= LX_IFF_POINTOPOINT;
	if ((flags & IFF_NOARP) != 0)
		result |= LX_IFF_NOARP;
	if ((flags & IFF_PROMISC) != 0)
		result |= LX_IFF_PROMISC;
	if ((flags & IFF_ALLMULTI) != 0)
		result |= LX_IFF_ALLMULTI;
	if ((flags & IFF_MULTICAST) != 0)
		result |= LX_IFF_MULTICAST;
	if ((flags & IFF_LINK) != 0)
		result |= LX_IFF_RUNNING | LX_IFF_LOWER_UP;
	return result;
}


static int
prefix_length(const uint8* mask, size_t bytes)
{
	int length = 0;
	for (size_t i = 0; i < bytes; i++) {
		uint8 byte = mask[i];
		while ((byte & 0x80) != 0) {
			length++;
			byte <<= 1;
		}
		if (byte != 0 || mask[i] != 0xff)
			break;
	}
	return length;
}


/*!	A kernel-side socket of the given Haiku family for interface queries,
	or a negative error. The caller closes it with _kern_close().
*/
static int
query_socket(int family)
{
	return kernel_socket(family, SOCK_DGRAM, 0);
}


static int
interface_request(int socket, const char* haikuName, int op, ifreq* request)
{
	memset(request, 0, sizeof(*request));
	strlcpy(request->ifr_name, haikuName, IF_NAMESIZE);
	return _kern_ioctl(socket, op, request, sizeof(*request));
}


/*!	Fills \a interfaces with Haiku's interfaces under their Linux names;
	returns how many.
*/
static int
collect_interfaces(NetInterface* interfaces, int maxCount)
{
	int linkSocket = query_socket(AF_LINK);
	if (linkSocket < 0)
		return 0;

	const size_t kBufferSize = 4096;
	uint8* buffer = (uint8*)malloc(kBufferSize);
	if (buffer == NULL) {
		_kern_close(linkSocket);
		return 0;
	}
	MemoryDeleter bufferDeleter(buffer);

	ifconf config;
	config.ifc_len = kBufferSize;
	config.ifc_buf = buffer;
	if (_kern_ioctl(linkSocket, SIOCGIFCONF, &config, sizeof(config))
			!= B_OK) {
		_kern_close(linkSocket);
		return 0;
	}

	int count = 0, ethernet = 0;
	uint8* position = buffer;
	uint8* end = buffer + config.ifc_len;
	while (position + IF_NAMESIZE + 2 <= end && count < maxCount) {
		ifreq* entry = (ifreq*)position;
		position += _SIZEOF_ADDR_IFREQ(*entry);

		NetInterface& interface = interfaces[count];
		memset(&interface, 0, sizeof(interface));
		strlcpy(interface.haikuName, entry->ifr_name, IF_NAMESIZE);

		ifreq request;
		if (interface_request(linkSocket, interface.haikuName, SIOCGIFINDEX,
				&request) != B_OK) {
			continue;
		}
		interface.index = request.ifr_index;
		int haikuFlags = 0;
		if (interface_request(linkSocket, interface.haikuName, SIOCGIFFLAGS,
				&request) == B_OK) {
			haikuFlags = request.ifr_flags;
		}
		interface.flags = interface_flags_from_haiku(haikuFlags);
		interface.mtu = 1500;
		if (interface_request(linkSocket, interface.haikuName, SIOCGIFMTU,
				&request) == B_OK) {
			interface.mtu = request.ifr_mtu;
		}

		const sockaddr_dl* link = (const sockaddr_dl*)&entry->ifr_addr;
		if (link->sdl_family == AF_LINK) {
			interface.hardwareLength = min_c(link->sdl_alen,
				sizeof(interface.hardwareAddress));
			memcpy(interface.hardwareAddress, LLADDR(link),
				interface.hardwareLength);
		}

		// Haiku's link layer does not tell Wi-Fi from Ethernet (both are
		// IFT_ETHER), so every other interface is "ethN"; Linux programs
		// rarely care, and the hardware type is Ethernet for both.
		if ((haikuFlags & IFF_LOOPBACK) != 0) {
			strcpy(interface.name, "lo");
			interface.type = LX_ARPHRD_LOOPBACK;
			interface.hardwareLength = 6;
			memset(interface.hardwareAddress, 0, 6);
			if (interface.mtu == 0)
				interface.mtu = 65536;
		} else {
			snprintf(interface.name, sizeof(interface.name), "eth%d",
				ethernet++);
			interface.type = interface.hardwareLength == 6
				? LX_ARPHRD_ETHER : LX_ARPHRD_NONE;
		}
		count++;
	}

	_kern_close(linkSocket);
	return count;
}


static const NetInterface*
find_interface_by_index(const NetInterface* interfaces, int count,
	uint32 index)
{
	for (int i = 0; i < count; i++) {
		if (interfaces[i].index == index)
			return &interfaces[i];
	}
	return NULL;
}


static const NetInterface*
find_interface_by_name(const NetInterface* interfaces, int count,
	const char* name, bool haiku)
{
	for (int i = 0; i < count; i++) {
		if (strcmp(haiku ? interfaces[i].haikuName : interfaces[i].name,
				name) == 0) {
			return &interfaces[i];
		}
	}
	return NULL;
}


/*!	The first address of each interface for the Haiku \a family (AF_INET
	or AF_INET6), as Haiku's SIOCGIFCONF reports one per interface.
*/
static int
collect_addresses(int family, const NetInterface* interfaces,
	int interfaceCount, NetAddress* addresses, int maxCount)
{
	int socket = query_socket(family);
	if (socket < 0)
		return 0;

	const size_t kBufferSize = 4096;
	uint8* buffer = (uint8*)malloc(kBufferSize);
	if (buffer == NULL) {
		_kern_close(socket);
		return 0;
	}
	MemoryDeleter bufferDeleter(buffer);

	ifconf config;
	config.ifc_len = kBufferSize;
	config.ifc_buf = buffer;
	if (_kern_ioctl(socket, SIOCGIFCONF, &config, sizeof(config)) != B_OK) {
		_kern_close(socket);
		return 0;
	}

	int count = 0;
	uint8* position = buffer;
	uint8* end = buffer + config.ifc_len;
	while (position + IF_NAMESIZE + 2 <= end && count < maxCount) {
		ifreq* entry = (ifreq*)position;
		position += _SIZEOF_ADDR_IFREQ(*entry);
		if (entry->ifr_addr.sa_family != family)
			continue;

		const NetInterface* interface = find_interface_by_name(interfaces,
			interfaceCount, entry->ifr_name, true);
		if (interface == NULL)
			continue;

		NetAddress& address = addresses[count];
		memset(&address, 0, sizeof(address));
		address.index = interface->index;
		address.family = family_from_haiku(family);

		ifreq request;
		if (family == AF_INET) {
			const sockaddr_in* in = (const sockaddr_in*)&entry->ifr_addr;
			if (in->sin_addr.s_addr == 0)
				continue;
			memcpy(address.address, &in->sin_addr, 4);
			address.prefixLength = 32;
			if (interface_request(socket, entry->ifr_name, SIOCGIFNETMASK,
					&request) == B_OK
				&& request.ifr_mask.sa_family == AF_INET) {
				address.prefixLength = prefix_length((const uint8*)
					&((sockaddr_in*)&request.ifr_mask)->sin_addr, 4);
			}
			if ((interface->flags & LX_IFF_BROADCAST) != 0
				&& interface_request(socket, entry->ifr_name, SIOCGIFBRDADDR,
					&request) == B_OK
				&& request.ifr_broadaddr.sa_family == AF_INET) {
				memcpy(address.broadcast,
					&((sockaddr_in*)&request.ifr_broadaddr)->sin_addr, 4);
				address.hasBroadcast = true;
			}
		} else {
			const sockaddr_in6* in6 = (const sockaddr_in6*)&entry->ifr_addr;
			static const uint8 kZero[16] = {};
			if (memcmp(&in6->sin6_addr, kZero, 16) == 0)
				continue;
			memcpy(address.address, &in6->sin6_addr, 16);
			address.prefixLength = 128;
			if (interface_request(socket, entry->ifr_name, SIOCGIFNETMASK,
					&request) == B_OK
				&& request.ifr_mask.sa_family == AF_INET6) {
				address.prefixLength = prefix_length((const uint8*)
					&((sockaddr_in6*)&request.ifr_mask)->sin6_addr, 16);
			}
		}
		count++;
	}

	_kern_close(socket);
	return count;
}


/*!	Builds netlink messages into a buffer and sends each as one datagram on
	the kernel end of a netlink socket. One message per datagram keeps every
	reply far below the smallest buffer libcs read with (4 KiB).
*/
class NetlinkWriter {
public:
	NetlinkWriter(int fd, uint32 sequence, uint32 portID)
		:
		fFD(fd),
		fSequence(sequence),
		fPortID(portID),
		fLength(0),
		fStatus(B_OK)
	{
	}

	void* Begin(uint16 type, uint16 flags, size_t payloadSize)
	{
		memset(fBuffer, 0, sizeof(fBuffer));
		linux_nlmsghdr* header = (linux_nlmsghdr*)fBuffer;
		header->nlmsg_type = type;
		header->nlmsg_flags = flags;
		header->nlmsg_seq = fSequence;
		header->nlmsg_pid = fPortID;
		fLength = LX_NLMSG_HDRLEN + LX_NLMSG_ALIGN(payloadSize);
		return fBuffer + LX_NLMSG_HDRLEN;
	}

	void Attribute(uint16 type, const void* data, size_t length)
	{
		size_t total = LX_RTA_ALIGN(sizeof(linux_rtattr) + length);
		if (fLength + total > sizeof(fBuffer))
			return;
		linux_rtattr* attribute = (linux_rtattr*)(fBuffer + fLength);
		attribute->rta_len = sizeof(linux_rtattr) + length;
		attribute->rta_type = type;
		memcpy(attribute + 1, data, length);
		fLength += total;
	}

	void AttributeString(uint16 type, const char* string)
	{
		Attribute(type, string, strlen(string) + 1);
	}

	void AttributeU32(uint16 type, uint32 value)
	{
		Attribute(type, &value, sizeof(value));
	}

	void End()
	{
		((linux_nlmsghdr*)fBuffer)->nlmsg_len = fLength;
		ssize_t sent = kernel_send(fFD, fBuffer, fLength);
		TRACE("netlink reply type %u len %zu -> %zd\n",
			((linux_nlmsghdr*)fBuffer)->nlmsg_type, fLength, sent);
		if (sent < 0 && fStatus == B_OK)
			fStatus = sent;
	}

	void Done(uint16 flags)
	{
		int32* error = (int32*)Begin(LX_NLMSG_DONE, flags, sizeof(int32));
		*error = 0;
		End();
	}

	void Error(int linuxErrno, const linux_nlmsghdr* request)
	{
		uint8* payload = (uint8*)Begin(LX_NLMSG_ERROR, 0,
			sizeof(int32) + sizeof(linux_nlmsghdr));
		*(int32*)payload = -linuxErrno;
		memcpy(payload + sizeof(int32), request, sizeof(linux_nlmsghdr));
		End();
	}

	status_t Status() const	{ return fStatus; }

private:
	int		fFD;
	uint32	fSequence;
	uint32	fPortID;
	size_t	fLength;
	status_t fStatus;
	uint8	fBuffer[1024];
};


static void
netlink_links(NetlinkWriter& writer, const linux_nlmsghdr* request,
	const uint8* payload, size_t payloadLength)
{
	NetInterface interfaces[kMaxInterfaces];
	int count = collect_interfaces(interfaces, kMaxInterfaces);

	bool dump = (request->nlmsg_flags & LX_NLM_F_DUMP) != 0;
	int32 wantIndex = 0;
	if (!dump && payloadLength >= sizeof(linux_ifinfomsg))
		wantIndex = ((const linux_ifinfomsg*)payload)->ifi_index;

	bool found = false;
	for (int i = 0; i < count; i++) {
		const NetInterface& interface = interfaces[i];
		if (!dump && (int32)interface.index != wantIndex)
			continue;
		found = true;
		linux_ifinfomsg* info = (linux_ifinfomsg*)writer.Begin(
			LX_RTM_NEWLINK, dump ? LX_NLM_F_MULTI : 0,
			sizeof(linux_ifinfomsg));
		info->ifi_family = LX_AF_UNSPEC;
		info->ifi_type = interface.type;
		info->ifi_index = interface.index;
		info->ifi_flags = interface.flags;
		info->ifi_change = 0xffffffff;
		writer.AttributeString(LX_IFLA_IFNAME, interface.name);
		writer.AttributeU32(LX_IFLA_MTU, interface.mtu);
		writer.AttributeU32(LX_IFLA_TXQLEN, 1000);
		uint8 operState = (interface.flags & LX_IFF_LOWER_UP) != 0
			? LX_IF_OPER_UP : LX_IF_OPER_DOWN;
		if ((interface.flags & LX_IFF_LOOPBACK) != 0)
			operState = LX_IF_OPER_UNKNOWN;
		writer.Attribute(LX_IFLA_OPERSTATE, &operState, 1);
		if (interface.hardwareLength > 0) {
			writer.Attribute(LX_IFLA_ADDRESS, interface.hardwareAddress,
				interface.hardwareLength);
			uint8 broadcast[16];
			memset(broadcast, (interface.flags & LX_IFF_LOOPBACK) != 0
				? 0 : 0xff, sizeof(broadcast));
			writer.Attribute(LX_IFLA_BROADCAST, broadcast,
				interface.hardwareLength);
		}
		writer.End();
	}

	if (dump)
		writer.Done(LX_NLM_F_MULTI);
	else if (!found)
		writer.Error(LX_ENODEV, request);
}


static void
netlink_addresses(NetlinkWriter& writer, const linux_nlmsghdr* request,
	const uint8* payload, size_t payloadLength)
{
	NetInterface interfaces[kMaxInterfaces];
	int interfaceCount = collect_interfaces(interfaces, kMaxInterfaces);

	int wantFamily = LX_AF_UNSPEC;
	if (payloadLength >= 1)
		wantFamily = payload[0];

	NetAddress addresses[kMaxInterfaces * 2];
	int count = 0;
	if (wantFamily == LX_AF_UNSPEC || wantFamily == LX_AF_INET) {
		count += collect_addresses(AF_INET, interfaces, interfaceCount,
			addresses + count, kMaxInterfaces);
	}
	if (wantFamily == LX_AF_UNSPEC || wantFamily == LX_AF_INET6) {
		count += collect_addresses(AF_INET6, interfaces, interfaceCount,
			addresses + count, kMaxInterfaces);
	}

	for (int i = 0; i < count; i++) {
		const NetAddress& address = addresses[i];
		const NetInterface* interface = find_interface_by_index(interfaces,
			interfaceCount, address.index);
		size_t length = address.family == LX_AF_INET ? 4 : 16;

		linux_ifaddrmsg* message = (linux_ifaddrmsg*)writer.Begin(
			LX_RTM_NEWADDR, LX_NLM_F_MULTI, sizeof(linux_ifaddrmsg));
		message->ifa_family = address.family;
		message->ifa_prefixlen = address.prefixLength;
		message->ifa_flags = LX_IFA_F_PERMANENT;
		bool loopback = interface != NULL
			&& (interface->flags & LX_IFF_LOOPBACK) != 0;
		message->ifa_scope = loopback ? LX_RT_SCOPE_HOST
			: (address.family == LX_AF_INET6 && address.address[0] == 0xfe
				&& (address.address[1] & 0xc0) == 0x80)
				? LX_RT_SCOPE_LINK : LX_RT_SCOPE_UNIVERSE;
		message->ifa_index = address.index;
		writer.Attribute(LX_IFA_ADDRESS, address.address, length);
		if (address.family == LX_AF_INET) {
			writer.Attribute(LX_IFA_LOCAL, address.address, length);
			if (address.hasBroadcast)
				writer.Attribute(LX_IFA_BROADCAST, address.broadcast, 4);
			if (interface != NULL)
				writer.AttributeString(LX_IFA_LABEL, interface->name);
		}
		writer.AttributeU32(LX_IFA_FLAGS, LX_IFA_F_PERMANENT);
		writer.End();
	}
	writer.Done(LX_NLM_F_MULTI);
}


static void
netlink_routes_for_family(NetlinkWriter& writer, int haikuFamily,
	const NetInterface* interfaces, int interfaceCount)
{
	int socket = query_socket(haikuFamily);
	if (socket < 0)
		return;

	ifconf config;
	config.ifc_len = sizeof(config.ifc_value);
	if (_kern_ioctl(socket, SIOCGRTSIZE, &config, sizeof(config)) != B_OK
		|| config.ifc_value <= 0) {
		_kern_close(socket);
		return;
	}
	size_t size = config.ifc_value;
	if (size > 64 * 1024)
		size = 64 * 1024;
	uint8* buffer = (uint8*)malloc(size);
	if (buffer == NULL) {
		_kern_close(socket);
		return;
	}
	MemoryDeleter bufferDeleter(buffer);
	config.ifc_len = size;
	config.ifc_buf = buffer;
	if (_kern_ioctl(socket, SIOCGRTTABLE, &config, sizeof(config)) != B_OK) {
		_kern_close(socket);
		return;
	}
	_kern_close(socket);

	size_t addressLength = haikuFamily == AF_INET ? 4 : 16;
	const size_t kBaseSize = IF_NAMESIZE + sizeof(route_entry);
	uint8* position = buffer;
	uint8* end = buffer + size;
	while (position + kBaseSize <= end) {
		ifreq* entry = (ifreq*)position;
		route_entry& route = entry->ifr_route;
		size_t recordSize = kBaseSize;
		// The stack lays the addresses out right behind the entry and
		// points at them in the caller's buffer, which is this one.
		sockaddr* destination = route.destination;
		sockaddr* mask = route.mask;
		sockaddr* gateway = route.gateway;
		if (destination != NULL)
			recordSize += destination->sa_len;
		if (mask != NULL)
			recordSize += mask->sa_len;
		if (gateway != NULL)
			recordSize += gateway->sa_len;
		if (position + recordSize > end || recordSize == kBaseSize)
			break;
		position += recordSize;

		const NetInterface* interface = find_interface_by_name(interfaces,
			interfaceCount, entry->ifr_name, true);
		if (interface == NULL)
			continue;

		const uint8* destinationBytes = NULL;
		const uint8* gatewayBytes = NULL;
		int prefix = 0;
		if (haikuFamily == AF_INET) {
			if (destination != NULL && destination->sa_family == AF_INET)
				destinationBytes = (const uint8*)
					&((sockaddr_in*)destination)->sin_addr;
			if (mask != NULL && mask->sa_family == AF_INET)
				prefix = prefix_length((const uint8*)
					&((sockaddr_in*)mask)->sin_addr, 4);
			if (gateway != NULL && gateway->sa_family == AF_INET)
				gatewayBytes = (const uint8*)
					&((sockaddr_in*)gateway)->sin_addr;
		} else {
			if (destination != NULL && destination->sa_family == AF_INET6)
				destinationBytes = (const uint8*)
					&((sockaddr_in6*)destination)->sin6_addr;
			if (mask != NULL && mask->sa_family == AF_INET6)
				prefix = prefix_length((const uint8*)
					&((sockaddr_in6*)mask)->sin6_addr, 16);
			if (gateway != NULL && gateway->sa_family == AF_INET6)
				gatewayBytes = (const uint8*)
					&((sockaddr_in6*)gateway)->sin6_addr;
		}
		if ((route.flags & RTF_DEFAULT) != 0)
			prefix = 0;
		else if ((route.flags & RTF_HOST) != 0)
			prefix = addressLength * 8;

		linux_rtmsg* message = (linux_rtmsg*)writer.Begin(LX_RTM_NEWROUTE,
			LX_NLM_F_MULTI, sizeof(linux_rtmsg));
		message->rtm_family = family_from_haiku(haikuFamily);
		message->rtm_dst_len = prefix;
		message->rtm_table = LX_RT_TABLE_MAIN;
		message->rtm_protocol = (route.flags & RTF_STATIC) != 0
			? LX_RTPROT_BOOT : LX_RTPROT_KERNEL;
		bool local = (route.flags & RTF_LOCAL) != 0
			|| (interface->flags & LX_IFF_LOOPBACK) != 0;
		message->rtm_scope = (route.flags & RTF_GATEWAY) != 0
			? LX_RT_SCOPE_UNIVERSE
			: local ? LX_RT_SCOPE_HOST : LX_RT_SCOPE_LINK;
		message->rtm_type = LX_RTN_UNICAST;
		writer.AttributeU32(LX_RTA_TABLE, LX_RT_TABLE_MAIN);
		if (prefix > 0 && destinationBytes != NULL)
			writer.Attribute(LX_RTA_DST, destinationBytes, addressLength);
		if ((route.flags & RTF_GATEWAY) != 0 && gatewayBytes != NULL)
			writer.Attribute(LX_RTA_GATEWAY, gatewayBytes, addressLength);
		writer.AttributeU32(LX_RTA_OIF, interface->index);
		writer.End();
	}
}


static void
netlink_routes(NetlinkWriter& writer, const linux_nlmsghdr* request,
	const uint8* payload, size_t payloadLength)
{
	NetInterface interfaces[kMaxInterfaces];
	int interfaceCount = collect_interfaces(interfaces, kMaxInterfaces);

	int wantFamily = payloadLength >= 1 ? payload[0] : LX_AF_UNSPEC;
	if (wantFamily == LX_AF_UNSPEC || wantFamily == LX_AF_INET)
		netlink_routes_for_family(writer, AF_INET, interfaces, interfaceCount);
	if (wantFamily == LX_AF_UNSPEC || wantFamily == LX_AF_INET6) {
		netlink_routes_for_family(writer, AF_INET6, interfaces,
			interfaceCount);
	}
	writer.Done(LX_NLM_F_MULTI);
}


/*!	Answers the netlink requests in \a data (\a length bytes, already in
	the kernel) on \a socket's kernel end.
*/
static void
netlink_handle(NetlinkSocket* socket, const uint8* data, size_t length)
{
	while (length >= sizeof(linux_nlmsghdr)) {
		const linux_nlmsghdr* request = (const linux_nlmsghdr*)data;
		if (request->nlmsg_len < sizeof(linux_nlmsghdr)
			|| request->nlmsg_len > length) {
			break;
		}
		const uint8* payload = data + LX_NLMSG_HDRLEN;
		size_t payloadLength = request->nlmsg_len > LX_NLMSG_HDRLEN
			? request->nlmsg_len - LX_NLMSG_HDRLEN : 0;

		NetlinkWriter writer(socket->kernelFD, request->nlmsg_seq,
			socket->portID);
		TRACE("netlink request type %u flags %#x\n", request->nlmsg_type,
			request->nlmsg_flags);
		switch (request->nlmsg_type) {
			case LX_RTM_GETLINK:
				netlink_links(writer, request, payload, payloadLength);
				break;
			case LX_RTM_GETADDR:
				netlink_addresses(writer, request, payload, payloadLength);
				break;
			case LX_RTM_GETROUTE:
				netlink_routes(writer, request, payload, payloadLength);
				break;
			case LX_RTM_GETNEIGH:
			case LX_RTM_GETRULE:
				// Nothing to list: an empty dump.
				if ((request->nlmsg_flags & LX_NLM_F_DUMP) != 0)
					writer.Done(LX_NLM_F_MULTI);
				else
					writer.Error(LX_EOPNOTSUPP, request);
				break;
			case LX_NLMSG_NOOP:
				break;
			default:
				// Changing interfaces, addresses or routes is Haiku's
				// network preferences' job; NEW/DEL/SET requests are even
				// numbers below 100 other than the GET ones above.
				writer.Error(request->nlmsg_type < 100 ? LX_EPERM
					: LX_EOPNOTSUPP, request);
				break;
		}

		size_t advance = LX_NLMSG_ALIGN(request->nlmsg_len);
		if (advance >= length)
			break;
		data += advance;
		length -= advance;
	}
}


static int64
netlink_send(NetlinkSocket* socket, uint64 buffer, uint64 length)
{
	// called with sNetlinkLock held
	if (length > 16 * 1024)
		return -LX_EMSGSIZE;
	uint8* data = (uint8*)malloc(length + 1);
	if (data == NULL)
		return -LX_ENOMEM;
	MemoryDeleter dataDeleter(data);
	if (renguin_copy_in(data, buffer, length) != B_OK)
		return -LX_EFAULT;
	if (socket->portID == 0)
		socket->portID = thread_get_current_thread()->team->id;
	netlink_handle(socket, data, length);
	return length;
}


static ssize_t gather_iovecs(uint64 userIovecs, uint64 count, uint8* buffer,
	size_t size);


/*!	write() and writev() on a netlink socket send requests, as send() does
	(busybox's ip writes its requests). \a _handled is false for any other
	descriptor.
*/
int64
renguin_netlink_write(int fd, uint64 buffer, uint64 length, bool vector,
	bool* _handled)
{
	*_handled = false;
	if (atomic_get(&sNetlinkCount) == 0)
		return 0;
	NetlinkSocket* netlink = netlink_lookup(fd);
	if (netlink == NULL)
		return 0;
	*_handled = true;

	int64 result;
	if (vector) {
		uint8* data = (uint8*)malloc(16 * 1024);
		if (data == NULL)
			result = -LX_ENOMEM;
		else {
			ssize_t bytes = gather_iovecs(buffer, length, data, 16 * 1024);
			if (bytes >= 0) {
				if (netlink->portID == 0) {
					netlink->portID
						= thread_get_current_thread()->team->id;
				}
				netlink_handle(netlink, data, bytes);
			}
			result = bytes;
			free(data);
		}
	} else
		result = netlink_send(netlink, buffer, length);
	mutex_unlock(&sNetlinkLock);
	return result;
}


/*!	Gathers the bytes described by a Linux iovec array into a kernel buffer
	(netlink requests are small).
*/
static ssize_t
gather_iovecs(uint64 userIovecs, uint64 count, uint8* buffer, size_t size)
{
	if (count > 1024)
		return -LX_EMSGSIZE;
	size_t total = 0;
	for (uint64 i = 0; i < count; i++) {
		linux_iovec iovec;
		if (renguin_copy_in(&iovec, userIovecs + i * sizeof(iovec),
				sizeof(iovec)) != B_OK) {
			return -LX_EFAULT;
		}
		if (iovec.iov_len > size - total)
			return -LX_EMSGSIZE;
		if (renguin_copy_in(buffer + total, iovec.iov_base, iovec.iov_len) != B_OK)
			return -LX_EFAULT;
		total += iovec.iov_len;
	}
	return total;
}


static int64
netlink_name_out(NetlinkSocket* socket, uint64 userAddress, uint64 userLength,
	bool local)
{
	if (userAddress == 0 || userLength == 0)
		return 0;
	linux_sockaddr_nl address;
	memset(&address, 0, sizeof(address));
	address.nl_family = LX_AF_NETLINK;
	if (local) {
		if (socket->portID == 0)
			socket->portID = thread_get_current_thread()->team->id;
		address.nl_pid = socket->portID;
	}
	uint32 bufferLength;
	if (renguin_copy_in(&bufferLength, userLength, sizeof(bufferLength))
			!= B_OK) {
		return -LX_EFAULT;
	}
	uint32 outLength = sizeof(address);
	if (renguin_copy_out(userAddress, &address,
			min_c(bufferLength, outLength)) != B_OK
		|| renguin_copy_out(userLength, &outLength, sizeof(outLength))
			!= B_OK) {
		return -LX_EFAULT;
	}
	return 0;
}


// #pragma mark - socket calls


int64
renguin_sys_socket(int64 family, uint64 type, int64 protocol)
{
	if (family == LX_AF_NETLINK)
		return netlink_socket((int)protocol, type);

	int haikuFamily = family_to_haiku((int)family);
	if (haikuFamily < 0 || haikuFamily == AF_UNSPEC)
		return -LX_EAFNOSUPPORT;
	int kind = type & LX_SOCK_TYPE_MASK;
	if ((type & ~(uint64)(LX_SOCK_TYPE_MASK | LX_SOCK_NONBLOCK
			| LX_SOCK_CLOEXEC)) != 0) {
		return -LX_EINVAL;
	}
	if (kind != SOCK_STREAM && kind != SOCK_DGRAM && kind != SOCK_RAW
		&& kind != SOCK_SEQPACKET) {
		return -LX_ESOCKTNOSUPPORT;
	}
	int haikuType = kind;
	if ((type & LX_SOCK_NONBLOCK) != 0)
		haikuType |= SOCK_NONBLOCK;
	if ((type & LX_SOCK_CLOEXEC) != 0)
		haikuType |= SOCK_CLOEXEC;
	// Linux accepts IPPROTO_IP (0) and the type's own protocol.
	int fd = _user_socket(haikuFamily, haikuType, (int)protocol);
	if (fd == B_NOT_SUPPORTED)
		return -LX_EPROTONOSUPPORT;
	return renguin_encode(fd);
}


int64
renguin_sys_socketpair(int64 family, uint64 type, int64 protocol, uint64 fds)
{
	if (family != LX_AF_UNIX)
		return family == LX_AF_INET || family == LX_AF_INET6
			? -LX_EOPNOTSUPP : -LX_EAFNOSUPPORT;
	int kind = type & LX_SOCK_TYPE_MASK;
	int haikuType = kind;
	if ((type & LX_SOCK_NONBLOCK) != 0)
		haikuType |= SOCK_NONBLOCK;
	if ((type & LX_SOCK_CLOEXEC) != 0)
		haikuType |= SOCK_CLOEXEC;
	// The int[2] layout is the same; the program's buffer is used as is.
	return renguin_encode(_user_socketpair(AF_UNIX, haikuType, (int)protocol,
		(int*)(addr_t)fds));
}


int64
renguin_sys_bind(int fd, uint64 address, uint64 length)
{
	NetlinkSocket* netlink = netlink_lookup(fd);
	if (netlink != NULL) {
		TRACE("netlink bind(%d)\n", fd);
		linux_sockaddr_nl local;
		int64 result = 0;
		if (length < sizeof(local)
			|| renguin_copy_in(&local, address, sizeof(local)) != B_OK) {
			result = -LX_EINVAL;
		} else if (local.nl_family != LX_AF_NETLINK)
			result = -LX_EINVAL;
		else {
			netlink->portID = local.nl_pid != 0 ? local.nl_pid
				: (uint32)thread_get_current_thread()->team->id;
		}
		mutex_unlock(&sNetlinkLock);
		return result;
	}

	sockaddr_storage haiku;
	socklen_t haikuLength;
	int64 result = address_from_linux(address, length, &haiku, &haikuLength,
		true);
	if (result != 0)
		return result;
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 user = renguin_scratch_put(&scratch, &haiku, haikuLength);
	if (user == 0)
		return -LX_EFAULT;
	status_t bindStatus = _user_bind(fd, (sockaddr*)(addr_t)user,
		haikuLength);
	TRACE("bind(%d, family %d len %d) -> %#" B_PRIx32 "\n", fd,
		haiku.ss_family, (int)haikuLength, bindStatus);
	return renguin_encode(bindStatus);
}


int64
renguin_sys_connect(int fd, uint64 address, uint64 length)
{
	NetlinkSocket* netlink = netlink_lookup(fd);
	if (netlink != NULL) {
		mutex_unlock(&sNetlinkLock);
		return 0;		// connecting to the kernel (port 0)
	}

	sockaddr_storage haiku;
	socklen_t haikuLength;
	int64 result = address_from_linux(address, length, &haiku, &haikuLength,
		false);
	if (result != 0)
		return result;
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 user = renguin_scratch_put(&scratch, &haiku, haikuLength);
	if (user == 0)
		return -LX_EFAULT;
	status_t connectStatus = _user_connect(fd, (sockaddr*)(addr_t)user,
		haikuLength);
	TRACE("connect(%d, family %d len %d) -> %#" B_PRIx32 "\n", fd,
		haiku.ss_family, (int)haikuLength, connectStatus);
	result = renguin_encode(connectStatus);
	if (result == -LX_EINTR)
		renguin_set_restart(RENGUIN_RESTART_SYS, NULL);
	return result;
}


int64
renguin_sys_listen(int fd, int64 backlog)
{
	if (backlog < 0)
		backlog = 0;
	status_t status = _user_listen(fd, (int)min_c(backlog, SOMAXCONN));
	TRACE("listen(%d) -> %#" B_PRIx32 "\n", fd, status);
	return renguin_encode(status);
}


int64
renguin_sys_accept4(int fd, uint64 address, uint64 length, uint64 flags)
{
	if ((flags & ~(uint64)(LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC)) != 0)
		return -LX_EINVAL;
	int haikuFlags = 0;
	if ((flags & LX_SOCK_NONBLOCK) != 0)
		haikuFlags |= SOCK_NONBLOCK;
	if ((flags & LX_SOCK_CLOEXEC) != 0)
		haikuFlags |= SOCK_CLOEXEC;

	// Haiku's accept() always wants a length cell, even without an
	// address buffer (Linux allows both to be NULL).
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userAddress = 0;
	socklen_t haikuLength = address != 0 ? sizeof(sockaddr_storage) : 0;
	uint64 userLength = renguin_scratch_put(&scratch, &haikuLength,
		sizeof(haikuLength));
	if (userLength == 0)
		return -LX_EFAULT;
	if (address != 0) {
		userAddress = renguin_scratch_alloc(&scratch,
			sizeof(sockaddr_storage));
		if (userAddress == 0)
			return -LX_EFAULT;
	}

	int accepted = _user_accept(fd, (sockaddr*)(addr_t)userAddress,
		(socklen_t*)(addr_t)userLength, haikuFlags);
	TRACE("accept4(%d, %#" B_PRIx64 ") -> %d\n", fd, address, accepted);
	if (accepted < 0) {
		int64 result = accepted == B_TIMED_OUT ? -LX_EAGAIN
			: renguin_encode(accepted);
		if (result == -LX_EINTR)
			renguin_set_restart(RENGUIN_RESTART_SYS, NULL);
		return result;
	}

	if (address != 0) {
		sockaddr_storage haiku;
		if (renguin_copy_in(&haikuLength, userLength, sizeof(haikuLength))
				!= B_OK
			|| renguin_copy_in(&haiku, userAddress,
				min_c(haikuLength, sizeof(haiku))) != B_OK) {
			_user_close(accepted);
			return -LX_EFAULT;
		}
		int64 result = address_to_linux((sockaddr*)&haiku,
			min_c(haikuLength, sizeof(haiku)), address, length);
		if (result != 0) {
			_user_close(accepted);
			return result;
		}
	}
	return accepted;
}


typedef status_t (*name_function)(int, sockaddr*, socklen_t*);


static int64
get_name(int fd, uint64 address, uint64 length, name_function function,
	bool local)
{
	NetlinkSocket* netlink = netlink_lookup(fd);
	if (netlink != NULL) {
		TRACE("netlink getname(%d)\n", fd);
		int64 result = netlink_name_out(netlink, address, length, local);
		mutex_unlock(&sNetlinkLock);
		return result;
	}

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userAddress = renguin_scratch_alloc(&scratch,
		sizeof(sockaddr_storage));
	socklen_t haikuLength = sizeof(sockaddr_storage);
	uint64 userLength = renguin_scratch_put(&scratch, &haikuLength,
		sizeof(haikuLength));
	if (userAddress == 0 || userLength == 0)
		return -LX_EFAULT;
	status_t status = function(fd, (sockaddr*)(addr_t)userAddress,
		(socklen_t*)(addr_t)userLength);
	if (status != B_OK)
		return renguin_encode(status);

	sockaddr_storage haiku;
	if (renguin_copy_in(&haikuLength, userLength, sizeof(haikuLength)) != B_OK
		|| renguin_copy_in(&haiku, userAddress,
			min_c(haikuLength, sizeof(haiku))) != B_OK) {
		return -LX_EFAULT;
	}
	return address_to_linux((sockaddr*)&haiku,
		min_c(haikuLength, sizeof(haiku)), address, length);
}


int64
renguin_sys_getsockname(int fd, uint64 address, uint64 length)
{
	return get_name(fd, address, length, &_user_getsockname, true);
}


int64
renguin_sys_getpeername(int fd, uint64 address, uint64 length)
{
	return get_name(fd, address, length, &_user_getpeername, false);
}


int64
renguin_sys_shutdown(int fd, int64 how)
{
	if (how < 0 || how > 2)
		return -LX_EINVAL;
	// SHUT_RD/WR/RDWR are 0/1/2 on both systems.
	return renguin_encode(_user_shutdown_socket(fd, (int)how));
}


/*!	Translates a Linux control message buffer for sendmsg() into a Haiku
	one in scratch space. Returns the Haiku buffer's user address (0 if
	there is none) and its length in \a _haikuLength.
*/
static int64
control_to_haiku(renguin_scratch* scratch, uint64 control,
	uint64 controlLength, uint64* _haikuControl, socklen_t* _haikuLength)
{
	*_haikuControl = 0;
	*_haikuLength = 0;
	if (control == 0 || controlLength == 0)
		return 0;
	if (controlLength > 16 * 1024)
		return -LX_ENOBUFS;

	uint8* linux = (uint8*)malloc(controlLength);
	// Haiku's headers are no larger than Linux's (both 16 bytes with the
	// data 8-byte aligned), but Linux lets the last message end unpadded
	// (libwayland sends msg_controllen = CMSG_LEN(n)), while Haiku's
	// messages take CMSG_SPACE(n) each: leave room for the padding.
	size_t haikuSize = controlLength + 8 * (controlLength / LX_CMSG_HDRLEN
		+ 1);
	uint8* haiku = (uint8*)malloc(haikuSize);
	if (linux == NULL || haiku == NULL) {
		free(linux);
		free(haiku);
		return -LX_ENOMEM;
	}
	MemoryDeleter linuxDeleter(linux), haikuDeleter(haiku);
	memset(haiku, 0, haikuSize);
	if (renguin_copy_in(linux, control, controlLength) != B_OK)
		return -LX_EFAULT;

	size_t in = 0, out = 0;
	while (in + LX_CMSG_HDRLEN <= controlLength) {
		const linux_cmsghdr* header = (const linux_cmsghdr*)(linux + in);
		if (header->cmsg_len < LX_CMSG_HDRLEN
			|| header->cmsg_len > controlLength - in) {
			return -LX_EINVAL;
		}
		size_t dataLength = header->cmsg_len - LX_CMSG_HDRLEN;
		const uint8* data = linux + in + LX_CMSG_HDRLEN;

		// (Haiku's SOL_SOCKET is -1, so "mapped" is a flag of its own)
		bool mapped = true;
		int level = 0, type = 0;
		if (header->cmsg_level == LX_SOL_SOCKET
			&& header->cmsg_type == LX_SCM_RIGHTS) {
			level = SOL_SOCKET;
			type = SCM_RIGHTS;
		} else if (header->cmsg_level == LX_SOL_IP
			&& header->cmsg_type == LX_IP_TOS) {
			level = IPPROTO_IP;
			type = IP_TOS;
		} else if (header->cmsg_level == LX_SOL_IPV6
			&& header->cmsg_type == LX_IPV6_TCLASS) {
			level = IPPROTO_IPV6;
			type = IPV6_TCLASS;
		} else
			mapped = false;
		// Others (SCM_CREDENTIALS, IP_PKTINFO for sending) are dropped:
		// Haiku's stack has no use for them; the defaults apply.

		if (mapped) {
			size_t space = CMSG_SPACE(dataLength);
			if (out + space > haikuSize)
				return -LX_ENOBUFS;
			cmsghdr* target = (cmsghdr*)(haiku + out);
			target->cmsg_len = CMSG_LEN(dataLength);
			target->cmsg_level = level;
			target->cmsg_type = type;
			memcpy(CMSG_DATA(target), data, dataLength);
			out += space;
		}
		in += LX_CMSG_ALIGN(header->cmsg_len);
	}

	if (out == 0)
		return 0;
	uint64 user = renguin_scratch_put(scratch, haiku, out);
	if (user == 0)
		return -LX_EFAULT;
	*_haikuControl = user;
	*_haikuLength = out;
	return 0;
}


/*!	Copies the control messages Haiku returned (\a haikuLength bytes at the
	scratch address \a haikuControl) into the Linux buffer; returns the
	Linux length and sets \a _truncated.
*/
static int64
control_to_linux(uint64 haikuControl, socklen_t haikuLength, uint64 control,
	uint64 controlLength, bool* _truncated)
{
	*_truncated = false;
	if (haikuLength == 0 || haikuControl == 0)
		return 0;

	uint8* haiku = (uint8*)malloc(haikuLength);
	uint8* linux = (uint8*)malloc(controlLength);
	if (haiku == NULL || linux == NULL) {
		free(haiku);
		free(linux);
		return -LX_ENOMEM;
	}
	MemoryDeleter haikuDeleter(haiku), linuxDeleter(linux);
	memset(linux, 0, controlLength);
	if (renguin_copy_in(haiku, haikuControl, haikuLength) != B_OK)
		return -LX_EFAULT;

	size_t in = 0, out = 0;
	while (in + sizeof(cmsghdr) <= haikuLength) {
		const cmsghdr* header = (const cmsghdr*)(haiku + in);
		if (header->cmsg_len < CMSG_LEN(0)
			|| header->cmsg_len > haikuLength - in) {
			break;
		}
		size_t dataLength = header->cmsg_len - CMSG_LEN(0);
		const uint8* data = (const uint8*)CMSG_DATA(header);

		int level = header->cmsg_level, type = header->cmsg_type;
		if (level == SOL_SOCKET) {
			level = LX_SOL_SOCKET;
			if (type == SCM_RIGHTS)
				type = LX_SCM_RIGHTS;
			else if (type == SCM_TIMESTAMP)
				type = LX_SO_TIMESTAMP;
					// Linux's SCM_TIMESTAMP is SO_TIMESTAMP; struct
					// timeval has the same layout on x86_64
		} else if (level == IPPROTO_IP) {
			if (type == IP_TOS || type == IP_RECVTOS)
				type = LX_IP_TOS;
			else if (type == IP_PKTINFO)
				type = LX_IP_PKTINFO;
			else if (type == IP_TTL)
				type = LX_IP_TTL;
		} else if (level == IPPROTO_IPV6) {
			if (type == IPV6_TCLASS || type == IPV6_RECVTCLASS)
				type = LX_IPV6_TCLASS;
			else if (type == IPV6_PKTINFO)
				type = LX_IPV6_PKTINFO;
			else if (type == IPV6_HOPLIMIT)
				type = LX_IPV6_HOPLIMIT;
		}

		size_t space = LX_CMSG_ALIGN(LX_CMSG_HDRLEN + dataLength);
		if (out + LX_CMSG_HDRLEN + dataLength > controlLength) {
			*_truncated = true;
			if (type == LX_SCM_RIGHTS && level == LX_SOL_SOCKET) {
				// Linux closes descriptors that do not fit.
				for (size_t i = 0; i + sizeof(int) <= dataLength;
						i += sizeof(int)) {
					_user_close(*(const int*)(data + i));
				}
			}
			in += CMSG_ALIGN(header->cmsg_len);
			continue;
		}
		linux_cmsghdr* target = (linux_cmsghdr*)(linux + out);
		target->cmsg_len = LX_CMSG_HDRLEN + dataLength;
		target->cmsg_level = level;
		target->cmsg_type = type;
		memcpy(linux + out + LX_CMSG_HDRLEN, data, dataLength);
		out += min_c(space, controlLength - out);
		in += CMSG_ALIGN(header->cmsg_len);
	}

	if (out > 0 && renguin_copy_out(control, linux, out) != B_OK)
		return -LX_EFAULT;
	return out;
}


static int64
do_sendmsg(int fd, const linux_msghdr& message, uint64 flags,
	renguin_scratch* callerScratch)
{
	NetlinkSocket* netlink = netlink_lookup(fd);
	if (netlink != NULL) {
		uint8* buffer = (uint8*)malloc(16 * 1024);
		int64 result;
		if (buffer == NULL)
			result = -LX_ENOMEM;
		else {
			ssize_t length = gather_iovecs(message.msg_iov,
				message.msg_iovlen, buffer, 16 * 1024);
			if (length < 0)
				result = length;
			else {
				if (netlink->portID == 0) {
					netlink->portID
						= thread_get_current_thread()->team->id;
				}
				netlink_handle(netlink, buffer, length);
				result = length;
			}
			free(buffer);
		}
		mutex_unlock(&sNetlinkLock);
		return result;
	}

	if (message.msg_iovlen > IOV_MAX)
		return -LX_EMSGSIZE;

	// Scratch space continues below the caller's (sendmmsg() keeps none).
	renguin_scratch localScratch;
	if (callerScratch == NULL)
		renguin_scratch_init(&localScratch);
	renguin_scratch& scratch = callerScratch != NULL
		? *callerScratch : localScratch;

	sockaddr_storage address;
	socklen_t addressLength = 0;
	uint64 userAddress = 0;
	if (message.msg_name != 0 && message.msg_namelen != 0) {
		int64 result = address_from_linux(message.msg_name,
			message.msg_namelen, &address, &addressLength, false);
		if (result != 0)
			return result;
		userAddress = renguin_scratch_put(&scratch, &address, addressLength);
		if (userAddress == 0)
			return -LX_EFAULT;
	}

	uint64 control;
	socklen_t controlLength;
	int64 result = control_to_haiku(&scratch, message.msg_control,
		message.msg_controllen, &control, &controlLength);
	if (result != 0)
		return result;

	msghdr haiku;
	memset(&haiku, 0, sizeof(haiku));
	haiku.msg_name = (void*)(addr_t)userAddress;
	haiku.msg_namelen = addressLength;
	// struct iovec is the same on both systems.
	haiku.msg_iov = (iovec*)(addr_t)message.msg_iov;
	haiku.msg_iovlen = (int)message.msg_iovlen;
	haiku.msg_control = (void*)(addr_t)control;
	haiku.msg_controllen = controlLength;
	uint64 userMessage = renguin_scratch_put(&scratch, &haiku, sizeof(haiku));
	if (userMessage == 0)
		return -LX_EFAULT;

	ssize_t sent = _user_sendmsg(fd, (msghdr*)(addr_t)userMessage,
		msg_flags_to_haiku(flags));
	TRACE("sendmsg(%d, control %d) -> %zd\n", fd, (int)controlLength, sent);
	// SO_SNDTIMEO: Linux reports EAGAIN when the timeout passes.
	return sent == B_TIMED_OUT ? -LX_EAGAIN : renguin_encode(sent);
}


static int64
do_recvmsg(int fd, uint64 userLinuxMessage, linux_msghdr& message,
	uint64 flags, renguin_scratch* callerScratch)
{
	NetlinkSocket* netlink = netlink_lookup(fd);
	bool isNetlink = netlink != NULL;
	if (isNetlink)
		mutex_unlock(&sNetlinkLock);

	if (message.msg_iovlen > IOV_MAX)
		return -LX_EMSGSIZE;

	// Scratch space continues below the caller's: recvfrom() keeps its
	// iovec and message there.
	renguin_scratch localScratch;
	if (callerScratch == NULL)
		renguin_scratch_init(&localScratch);
	renguin_scratch& scratch = callerScratch != NULL
		? *callerScratch : localScratch;

	uint64 userAddress = 0;
	socklen_t addressRoom = sizeof(sockaddr_storage);
	if (message.msg_name != 0 && !isNetlink) {
		userAddress = renguin_scratch_alloc(&scratch, addressRoom);
		if (userAddress == 0)
			return -LX_EFAULT;
	}
	uint64 control = 0;
	size_t controlRoom = 0;
	if (message.msg_control != 0 && message.msg_controllen > 0) {
		controlRoom = min_c(message.msg_controllen, 16 * 1024);
		control = renguin_scratch_alloc(&scratch, controlRoom);
		if (control == 0)
			return -LX_EFAULT;
	}

	msghdr haiku;
	memset(&haiku, 0, sizeof(haiku));
	haiku.msg_name = (void*)(addr_t)userAddress;
	haiku.msg_namelen = userAddress != 0 ? addressRoom : 0;
	haiku.msg_iov = (iovec*)(addr_t)message.msg_iov;
	haiku.msg_iovlen = (int)message.msg_iovlen;
	haiku.msg_control = (void*)(addr_t)control;
	haiku.msg_controllen = controlRoom;
	uint64 userMessage = renguin_scratch_put(&scratch, &haiku, sizeof(haiku));
	if (userMessage == 0)
		return -LX_EFAULT;

	ssize_t received = _user_recvmsg(fd, (msghdr*)(addr_t)userMessage,
		msg_flags_to_haiku(flags));
	TRACE("recvmsg(%d) -> %zd, control %d of %zu\n", fd, received,
		received >= 0 && renguin_copy_in(&haiku, userMessage, sizeof(haiku))
			== B_OK ? (int)haiku.msg_controllen : -1, controlRoom);
	if (received < 0) {
		// SO_RCVTIMEO: Linux reports EAGAIN when the timeout passes.
		int64 result = received == B_TIMED_OUT ? -LX_EAGAIN
			: renguin_encode(received);
		if (result == -LX_EINTR)
			renguin_set_restart(RENGUIN_RESTART_SYS, NULL);
		return result;
	}

	if (renguin_copy_in(&haiku, userMessage, sizeof(haiku)) != B_OK)
		return -LX_EFAULT;

	// The source address.
	if (message.msg_name != 0) {
		if (isNetlink) {
			linux_sockaddr_nl kernel;
			memset(&kernel, 0, sizeof(kernel));
			kernel.nl_family = LX_AF_NETLINK;
			uint32 copy = min_c(message.msg_namelen, sizeof(kernel));
			if (renguin_copy_out(message.msg_name, &kernel, copy) != B_OK)
				return -LX_EFAULT;
			message.msg_namelen = sizeof(kernel);
		} else if (haiku.msg_namelen == 0) {
			message.msg_namelen = 0;
		} else {
			sockaddr_storage address;
			socklen_t length = min_c(haiku.msg_namelen, sizeof(address));
			if (renguin_copy_in(&address, userAddress, length) != B_OK)
				return -LX_EFAULT;
			uint64 lengthCell = renguin_scratch_put(&scratch,
				&message.msg_namelen, sizeof(uint32));
			if (lengthCell == 0)
				return -LX_EFAULT;
			int64 result = address_to_linux((sockaddr*)&address, length,
				message.msg_name, lengthCell);
			if (result != 0)
				return result;
			if (renguin_copy_in(&message.msg_namelen, lengthCell,
					sizeof(uint32)) != B_OK) {
				return -LX_EFAULT;
			}
		}
	}

	bool truncated = false;
	int64 controlLength = 0;
	if (control != 0) {
		controlLength = control_to_linux(control, haiku.msg_controllen,
			message.msg_control, message.msg_controllen, &truncated);
		if (controlLength < 0)
			return controlLength;
	}
	message.msg_controllen = controlLength;
	message.msg_flags = msg_flags_from_haiku(haiku.msg_flags)
		| (truncated ? LX_MSG_CTRUNC : 0);

	// Write back the fields recvmsg() changes.
	if (renguin_copy_out(userLinuxMessage + offsetof(linux_msghdr,
				msg_namelen), &message.msg_namelen, sizeof(uint32)) != B_OK
		|| renguin_copy_out(userLinuxMessage + offsetof(linux_msghdr,
				msg_controllen), &message.msg_controllen, sizeof(uint64))
			!= B_OK
		|| renguin_copy_out(userLinuxMessage + offsetof(linux_msghdr,
				msg_flags), &message.msg_flags, sizeof(uint32)) != B_OK) {
		return -LX_EFAULT;
	}
	return received;
}


int64
renguin_sys_sendmsg(int fd, uint64 userMessage, uint64 flags)
{
	linux_msghdr message;
	if (renguin_copy_in(&message, userMessage, sizeof(message)) != B_OK)
		return -LX_EFAULT;
	return do_sendmsg(fd, message, flags, NULL);
}


int64
renguin_sys_recvmsg(int fd, uint64 userMessage, uint64 flags)
{
	linux_msghdr message;
	if (renguin_copy_in(&message, userMessage, sizeof(message)) != B_OK)
		return -LX_EFAULT;
	return do_recvmsg(fd, userMessage, message, flags, NULL);
}


int64
renguin_sys_sendto(int fd, uint64 buffer, uint64 length, uint64 flags,
	uint64 address, uint64 addressLength)
{
	NetlinkSocket* netlink = netlink_lookup(fd);
	if (netlink != NULL) {
		int64 result = netlink_send(netlink, buffer, length);
		mutex_unlock(&sNetlinkLock);
		return result;
	}

	if (address == 0 || addressLength == 0) {
		ssize_t sent = _user_send(fd, (const void*)(addr_t)buffer, length,
			msg_flags_to_haiku(flags));
		return sent == B_TIMED_OUT ? -LX_EAGAIN : renguin_encode(sent);
	}

	sockaddr_storage haiku;
	socklen_t haikuLength;
	int64 result = address_from_linux(address, addressLength, &haiku,
		&haikuLength, false);
	if (result != 0)
		return result;
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 user = renguin_scratch_put(&scratch, &haiku, haikuLength);
	if (user == 0)
		return -LX_EFAULT;
	return renguin_encode(_user_sendto(fd, (const void*)(addr_t)buffer,
		length, msg_flags_to_haiku(flags), (sockaddr*)(addr_t)user,
		haikuLength));
}


int64
renguin_sys_recvfrom(int fd, uint64 buffer, uint64 length, uint64 flags,
	uint64 address, uint64 addressLength)
{
	// One path for all: a recvmsg() with a single iovec in scratch space.
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	linux_iovec iovec = { buffer, length };
	uint64 userIovec = renguin_scratch_put(&scratch, &iovec, sizeof(iovec));
	if (userIovec == 0)
		return -LX_EFAULT;

	linux_msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_iov = userIovec;
	message.msg_iovlen = 1;
	if (address != 0 && addressLength != 0) {
		uint32 room;
		if (renguin_copy_in(&room, addressLength, sizeof(room)) != B_OK)
			return -LX_EFAULT;
		if ((int32)room < 0)
			return -LX_EINVAL;
		message.msg_name = address;
		message.msg_namelen = room;
	}
	uint64 userMessage = renguin_scratch_put(&scratch, &message,
		sizeof(message));
	if (userMessage == 0)
		return -LX_EFAULT;
	int64 result = do_recvmsg(fd, userMessage, message, flags, &scratch);
	if (result >= 0 && message.msg_name != 0) {
		if (renguin_copy_out(addressLength, &message.msg_namelen,
				sizeof(uint32)) != B_OK) {
			return -LX_EFAULT;
		}
	}
	return result;
}


int64
renguin_sys_sendmmsg(int fd, uint64 vector, uint64 count, uint64 flags)
{
	if (count > 1024)
		count = 1024;
	uint64 sent = 0;
	for (; sent < count; sent++) {
		uint64 entry = vector + sent * sizeof(linux_mmsghdr);
		linux_msghdr message;
		if (renguin_copy_in(&message, entry, sizeof(message)) != B_OK)
			return sent > 0 ? (int64)sent : -LX_EFAULT;
		int64 result = do_sendmsg(fd, message, flags, NULL);
		if (result < 0)
			return sent > 0 ? (int64)sent : result;
		uint32 length = (uint32)result;
		if (renguin_copy_out(entry + offsetof(linux_mmsghdr, msg_len),
				&length, sizeof(length)) != B_OK) {
			return sent > 0 ? (int64)sent : -LX_EFAULT;
		}
	}
	return sent;
}


int64
renguin_sys_recvmmsg(int fd, uint64 vector, uint64 count, uint64 flags,
	uint64 timeout)
{
	// The timeout is checked between messages only, as on Linux.
	bigtime_t deadline = B_INFINITE_TIMEOUT;
	if (timeout != 0) {
		int64 spec[2];
		if (renguin_copy_in(spec, timeout, sizeof(spec)) != B_OK)
			return -LX_EFAULT;
		deadline = system_time() + spec[0] * 1000000 + spec[1] / 1000;
	}
	if (count > 1024)
		count = 1024;
	uint64 received = 0;
	for (; received < count; received++) {
		uint64 entry = vector + received * sizeof(linux_mmsghdr);
		linux_msghdr message;
		if (renguin_copy_in(&message, entry, sizeof(message)) != B_OK)
			return received > 0 ? (int64)received : -LX_EFAULT;
		uint64 messageFlags = flags & ~(uint64)LX_MSG_WAITFORONE;
		if (received > 0 && (flags & LX_MSG_WAITFORONE) != 0)
			messageFlags |= LX_MSG_DONTWAIT;
		int64 result = do_recvmsg(fd, entry, message, messageFlags, NULL);
		if (result < 0)
			return received > 0 ? (int64)received : result;
		uint32 length = (uint32)result;
		if (renguin_copy_out(entry + offsetof(linux_mmsghdr, msg_len),
				&length, sizeof(length)) != B_OK) {
			return received > 0 ? (int64)received : -LX_EFAULT;
		}
		if (system_time() >= deadline) {
			received++;
			break;
		}
	}
	return received;
}


// #pragma mark - socket options


/*!	Maps a Linux level/option pair to Haiku's. Returns false for options
	Haiku does not have. \a _accept is set for options that are accepted and
	ignored (performance hints Haiku's stack has no use for), and \a _int
	tells whether the value is an int (all mapped options but SO_LINGER,
	SO_RCVTIMEO/SNDTIMEO, SO_PEERCRED, SO_BINDTODEVICE and the membership
	structures).
*/
static bool
option_to_haiku(int64 level, int64 option, int* _level, int* _option,
	bool* _accept)
{
	*_accept = false;
	switch (level) {
		case LX_SOL_SOCKET:
			*_level = SOL_SOCKET;
			switch (option) {
				case LX_SO_DEBUG:		*_option = SO_DEBUG; return true;
				case LX_SO_REUSEADDR:	*_option = SO_REUSEADDR; return true;
				case LX_SO_TYPE:		*_option = SO_TYPE; return true;
				case LX_SO_ERROR:		*_option = SO_ERROR; return true;
				case LX_SO_DONTROUTE:	*_option = SO_DONTROUTE; return true;
				case LX_SO_BROADCAST:	*_option = SO_BROADCAST; return true;
				case LX_SO_SNDBUF:
				case LX_SO_SNDBUFFORCE:	*_option = SO_SNDBUF; return true;
				case LX_SO_RCVBUF:
				case LX_SO_RCVBUFFORCE:	*_option = SO_RCVBUF; return true;
				case LX_SO_KEEPALIVE:	*_option = SO_KEEPALIVE; return true;
				case LX_SO_OOBINLINE:	*_option = SO_OOBINLINE; return true;
				case LX_SO_LINGER:		*_option = SO_LINGER; return true;
				case LX_SO_REUSEPORT:	*_option = SO_REUSEPORT; return true;
				case LX_SO_PEERCRED:	*_option = SO_PEERCRED; return true;
				case LX_SO_RCVLOWAT:	*_option = SO_RCVLOWAT; return true;
				case LX_SO_SNDLOWAT:	*_option = SO_SNDLOWAT; return true;
				case LX_SO_RCVTIMEO:	*_option = SO_RCVTIMEO; return true;
				case LX_SO_SNDTIMEO:	*_option = SO_SNDTIMEO; return true;
				case LX_SO_ACCEPTCONN:	*_option = SO_ACCEPTCONN; return true;
				case LX_SO_TIMESTAMP:	*_option = SO_TIMESTAMP; return true;
				case LX_SO_PRIORITY:
				case LX_SO_PASSCRED:
				case LX_SO_PASSSEC:
				case LX_SO_MARK:
				case LX_SO_RXQ_OVFL:
				case LX_SO_BUSY_POLL:
				case LX_SO_INCOMING_CPU:
				case LX_SO_NO_CHECK:
					*_accept = true;
					return false;
			}
			return false;

		case LX_SOL_IP:
			*_level = IPPROTO_IP;
			switch (option) {
				case LX_IP_TOS:			*_option = IP_TOS; return true;
				case LX_IP_TTL:			*_option = IP_TTL; return true;
				case LX_IP_HDRINCL:		*_option = IP_HDRINCL; return true;
				case LX_IP_PKTINFO:		*_option = IP_PKTINFO; return true;
				case LX_IP_RECVTOS:		*_option = IP_RECVTOS; return true;
				case LX_IP_MTU_DISCOVER: *_option = IP_MTU_DISCOVER; return true;
				case LX_IP_MULTICAST_IF: *_option = IP_MULTICAST_IF; return true;
				case LX_IP_MULTICAST_TTL:
					*_option = IP_MULTICAST_TTL;
					return true;
				case LX_IP_MULTICAST_LOOP:
					*_option = IP_MULTICAST_LOOP;
					return true;
				case LX_IP_ADD_MEMBERSHIP:
					*_option = IP_ADD_MEMBERSHIP;
					return true;
				case LX_IP_DROP_MEMBERSHIP:
					*_option = IP_DROP_MEMBERSHIP;
					return true;
				case LX_IP_RECVERR:
				case LX_IP_RECVTTL:
				case LX_IP_FREEBIND:
					*_accept = true;
					return false;
			}
			return false;

		case LX_SOL_IPV6:
			*_level = IPPROTO_IPV6;
			switch (option) {
				case LX_IPV6_UNICAST_HOPS:
					*_option = IPV6_UNICAST_HOPS;
					return true;
				case LX_IPV6_MULTICAST_IF:
					*_option = IPV6_MULTICAST_IF;
					return true;
				case LX_IPV6_MULTICAST_HOPS:
					*_option = IPV6_MULTICAST_HOPS;
					return true;
				case LX_IPV6_MULTICAST_LOOP:
					*_option = IPV6_MULTICAST_LOOP;
					return true;
				case LX_IPV6_JOIN_GROUP:
					*_option = IPV6_JOIN_GROUP;
					return true;
				case LX_IPV6_LEAVE_GROUP:
					*_option = IPV6_LEAVE_GROUP;
					return true;
				case LX_IPV6_V6ONLY:	*_option = IPV6_V6ONLY; return true;
				case LX_IPV6_RECVPKTINFO:
					*_option = IPV6_RECVPKTINFO;
					return true;
				case LX_IPV6_PKTINFO:	*_option = IPV6_PKTINFO; return true;
				case LX_IPV6_RECVHOPLIMIT:
					*_option = IPV6_RECVHOPLIMIT;
					return true;
				case LX_IPV6_HOPLIMIT:	*_option = IPV6_HOPLIMIT; return true;
				case LX_IPV6_RECVTCLASS:
					*_option = IPV6_RECVTCLASS;
					return true;
				case LX_IPV6_TCLASS:	*_option = IPV6_TCLASS; return true;
				case LX_IPV6_DONTFRAG:	*_option = IPV6_DONTFRAG; return true;
				case LX_IPV6_MTU_DISCOVER:
					*_option = IPV6_MTU_DISCOVER;
					return true;
				case LX_IPV6_RECVERR:
					*_accept = true;
					return false;
			}
			return false;

		case LX_SOL_TCP:
			*_level = IPPROTO_TCP;
			switch (option) {
				case LX_TCP_NODELAY:	*_option = TCP_NODELAY; return true;
				case LX_TCP_MAXSEG:		*_option = TCP_MAXSEG; return true;
				case LX_TCP_CORK:		*_option = TCP_NOPUSH; return true;
				case LX_TCP_KEEPIDLE:	*_option = TCP_KEEPIDLE; return true;
				case LX_TCP_KEEPINTVL:	*_option = TCP_KEEPINTVL; return true;
				case LX_TCP_KEEPCNT:	*_option = TCP_KEEPCNT; return true;
				case LX_TCP_QUICKACK:
				case LX_TCP_USER_TIMEOUT:
				case LX_TCP_DEFER_ACCEPT:
				case LX_TCP_SYNCNT:
				case LX_TCP_LINGER2:
				case LX_TCP_WINDOW_CLAMP:
					*_accept = true;
					return false;
			}
			return false;

		case LX_SOL_UDP:
			// UDP_CORK, UDP_SEGMENT, UDP_GRO: hints.
			*_accept = true;
			return false;
	}
	return false;
}


static int
socket_family(int fd)
{
	sockaddr_storage address;
	socklen_t length = sizeof(address);
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userAddress = renguin_scratch_alloc(&scratch, sizeof(address));
	uint64 userLength = renguin_scratch_put(&scratch, &length,
		sizeof(length));
	if (userAddress == 0 || userLength == 0)
		return -1;
	if (_user_getsockname(fd, (sockaddr*)(addr_t)userAddress,
			(socklen_t*)(addr_t)userLength) != B_OK) {
		return -1;
	}
	if (renguin_copy_in(&address, userAddress, 2) != B_OK)
		return -1;
	return address.ss_family;
}


int64
renguin_sys_setsockopt(int fd, int64 level, int64 option, uint64 value,
	uint64 length)
{
	TRACE("setsockopt(%d, %" B_PRId64 ", %" B_PRId64 ")\n", fd, level,
		option);
	if (is_netlink(fd)) {
		// SO_RCVBUF, NETLINK_EXT_ACK, NETLINK_GET_STRICT_CHK, group
		// membership...: accepted.
		return 0;
	}

	int haikuLevel, haikuOption;
	bool accept;
	if (!option_to_haiku(level, option, &haikuLevel, &haikuOption, &accept)) {
		if (accept) {
			// Still fail on a descriptor that is not a socket.
			if (socket_family(fd) < 0)
				return -LX_ENOTSOCK;
			return 0;
		}
		return -LX_ENOPROTOOPT;
	}
	if (length > 256)
		return -LX_EINVAL;
	// int, struct linger, struct timeval and the multicast requests have
	// the same layout on both systems; the program's buffer is passed on.
	return renguin_encode(_user_setsockopt(fd, haikuLevel, haikuOption,
		(const void*)(addr_t)value, (socklen_t)length));
}


int64
renguin_sys_getsockopt(int fd, int64 level, int64 option, uint64 value,
	uint64 userLength)
{
	uint32 length;
	if (renguin_copy_in(&length, userLength, sizeof(length)) != B_OK)
		return -LX_EFAULT;
	if ((int32)length < 0)
		return -LX_EINVAL;

	if (is_netlink(fd)) {
		int32 result = 0;
		if (level == LX_SOL_SOCKET && option == LX_SO_TYPE)
			result = SOCK_RAW;
		else if (level == LX_SOL_SOCKET && option == LX_SO_DOMAIN)
			result = LX_AF_NETLINK;
		else if (level == LX_SOL_SOCKET && (option == LX_SO_RCVBUF
				|| option == LX_SO_SNDBUF))
			result = 212992;
		else if (level != LX_SOL_SOCKET || option != LX_SO_ERROR)
			return -LX_ENOPROTOOPT;
		uint32 outLength = min_c(length, sizeof(result));
		if (renguin_copy_out(value, &result, outLength) != B_OK
			|| renguin_copy_out(userLength, &outLength, sizeof(outLength))
				!= B_OK) {
			return -LX_EFAULT;
		}
		return 0;
	}

	if (level == LX_SOL_SOCKET && (option == LX_SO_DOMAIN
			|| option == LX_SO_PROTOCOL)) {
		int family = socket_family(fd);
		if (family < 0)
			return -LX_ENOTSOCK;
		int32 result;
		if (option == LX_SO_DOMAIN)
			result = family_from_haiku(family);
		else {
			int32 type = 0;
			socklen_t typeLength = sizeof(type);
			renguin_scratch scratch;
			renguin_scratch_init(&scratch);
			uint64 userType = renguin_scratch_alloc(&scratch, sizeof(type));
			uint64 userTypeLength = renguin_scratch_put(&scratch,
				&typeLength, sizeof(typeLength));
			if (userType == 0 || userTypeLength == 0
				|| _user_getsockopt(fd, SOL_SOCKET, SO_TYPE,
					(void*)(addr_t)userType,
					(socklen_t*)(addr_t)userTypeLength) != B_OK
				|| renguin_copy_in(&type, userType, sizeof(type)) != B_OK) {
				type = 0;
			}
			result = family == AF_UNIX ? 0
				: type == SOCK_STREAM ? IPPROTO_TCP
				: type == SOCK_DGRAM ? IPPROTO_UDP : 0;
		}
		uint32 outLength = min_c(length, sizeof(result));
		if (renguin_copy_out(value, &result, outLength) != B_OK
			|| renguin_copy_out(userLength, &outLength, sizeof(outLength))
				!= B_OK) {
			return -LX_EFAULT;
		}
		return 0;
	}

	int haikuLevel, haikuOption;
	bool accept;
	if (!option_to_haiku(level, option, &haikuLevel, &haikuOption, &accept)) {
		if (accept) {
			int32 zero = 0;
			uint32 outLength = min_c(length, sizeof(zero));
			if (renguin_copy_out(value, &zero, outLength) != B_OK
				|| renguin_copy_out(userLength, &outLength,
					sizeof(outLength)) != B_OK) {
				return -LX_EFAULT;
			}
			return 0;
		}
		return -LX_ENOPROTOOPT;
	}

	// socklen_t is 32 bits on both; the program's buffers are used.
	status_t status = _user_getsockopt(fd, haikuLevel, haikuOption,
		(void*)(addr_t)value, (socklen_t*)(addr_t)userLength);
	if (status != B_OK)
		return renguin_encode(status);

	// Translate values that differ.
	if (haikuLevel == SOL_SOCKET && haikuOption == SO_ERROR && length >= 4) {
		int32 error;
		if (renguin_copy_in(&error, value, sizeof(error)) == B_OK
			&& error != 0) {
			error = renguin_errno_from_haiku(error);
			renguin_copy_out(value, &error, sizeof(error));
		}
	}
	if (haikuLevel == SOL_SOCKET && (haikuOption == SO_REUSEADDR
			|| haikuOption == SO_KEEPALIVE || haikuOption == SO_BROADCAST
			|| haikuOption == SO_REUSEPORT || haikuOption == SO_OOBINLINE
			|| haikuOption == SO_DEBUG || haikuOption == SO_DONTROUTE
			|| haikuOption == SO_ACCEPTCONN) && length >= 4) {
		// Haiku reports the option's flag bit; Linux reports 1.
		int32 flag;
		if (renguin_copy_in(&flag, value, sizeof(flag)) == B_OK) {
			flag = flag != 0 ? 1 : 0;
			renguin_copy_out(value, &flag, sizeof(flag));
		}
	}
	return 0;
}


// #pragma mark - socket ioctls


static int64
ifreq_out(uint64 arg, const linux_ifreq& request)
{
	return renguin_copy_out(arg, &request, sizeof(request)) == B_OK
		? 0 : -LX_EFAULT;
}


static void
linux_inet_address(linux_sockaddr* target, const uint8* address)
{
	memset(target, 0, sizeof(*target));
	linux_sockaddr_in* in = (linux_sockaddr_in*)target;
	in->sin_family = LX_AF_INET;
	memcpy(&in->sin_addr, address, 4);
}


/*!	The socket ioctls Linux programs use to list interfaces (busybox
	ifconfig, glibc's and musl's if_nametoindex() and if_indextoname(),
	net-tools). Returns B_DEV_INVALID_IOCTL for requests it does not know.
*/
int64
renguin_net_ioctl(int fd, uint32 command, uint64 arg, bool* _handled)
{
	*_handled = true;
	TRACE("ioctl(%d, %#" B_PRIx32 ")\n", fd, command);
	switch (command) {
		case LX_SIOCGIFNAME:
		case LX_SIOCGIFFLAGS:
		case LX_SIOCGIFADDR:
		case LX_SIOCGIFBRDADDR:
		case LX_SIOCGIFNETMASK:
		case LX_SIOCGIFDSTADDR:
		case LX_SIOCGIFMETRIC:
		case LX_SIOCGIFMTU:
		case LX_SIOCGIFHWADDR:
		case LX_SIOCGIFINDEX:
		case LX_SIOCGIFTXQLEN:
		{
			linux_ifreq request;
			if (renguin_copy_in(&request, arg, sizeof(request)) != B_OK)
				return -LX_EFAULT;
			request.ifr_name[LX_IFNAMSIZ - 1] = '\0';

			NetInterface interfaces[kMaxInterfaces];
			int count = collect_interfaces(interfaces, kMaxInterfaces);
			const NetInterface* interface;
			if (command == LX_SIOCGIFNAME) {
				interface = find_interface_by_index(interfaces, count,
					(uint32)request.value);
			} else {
				interface = find_interface_by_name(interfaces, count,
					request.ifr_name, false);
			}
			if (interface == NULL)
				return -LX_ENODEV;

			switch (command) {
				case LX_SIOCGIFNAME:
					strlcpy(request.ifr_name, interface->name, LX_IFNAMSIZ);
					break;
				case LX_SIOCGIFINDEX:
					request.value = interface->index;
					break;
				case LX_SIOCGIFFLAGS:
					// ifr_flags is a short on Linux
					memset(request.pad, 0, sizeof(request.pad));
					*(uint16*)request.pad = (uint16)interface->flags;
					break;
				case LX_SIOCGIFMTU:
					request.value = interface->mtu;
					break;
				case LX_SIOCGIFMETRIC:
					request.value = 0;
					break;
				case LX_SIOCGIFTXQLEN:
					request.value = 1000;
					break;
				case LX_SIOCGIFHWADDR:
					memset(request.pad, 0, sizeof(request.pad));
					request.addr.sa_family = interface->type;
					memcpy(request.addr.sa_data, interface->hardwareAddress,
						min_c(interface->hardwareLength, 14));
					break;
				default:
				{
					NetAddress addresses[kMaxInterfaces];
					int addressCount = collect_addresses(AF_INET, interfaces,
						count, addresses, kMaxInterfaces);
					const NetAddress* address = NULL;
					for (int i = 0; i < addressCount; i++) {
						if (addresses[i].index == interface->index) {
							address = &addresses[i];
							break;
						}
					}
					if (address == NULL)
						return -LX_EADDRNOTAVAIL;
					uint8 bytes[4];
					if (command == LX_SIOCGIFADDR
						|| command == LX_SIOCGIFDSTADDR) {
						memcpy(bytes, address->address, 4);
					} else if (command == LX_SIOCGIFBRDADDR) {
						if (!address->hasBroadcast)
							return -LX_EADDRNOTAVAIL;
						memcpy(bytes, address->broadcast, 4);
					} else {
						uint32 mask = address->prefixLength == 0 ? 0
							: B_HOST_TO_BENDIAN_INT32(~(uint32)0
								<< (32 - address->prefixLength));
						memcpy(bytes, &mask, 4);
					}
					linux_inet_address(&request.addr, bytes);
					break;
				}
			}
			return ifreq_out(arg, request);
		}

		case LX_SIOCGIFCONF:
		{
			linux_ifconf config;
			if (renguin_copy_in(&config, arg, sizeof(config)) != B_OK)
				return -LX_EFAULT;
			NetInterface interfaces[kMaxInterfaces];
			int count = collect_interfaces(interfaces, kMaxInterfaces);
			NetAddress addresses[kMaxInterfaces];
			int addressCount = collect_addresses(AF_INET, interfaces, count,
				addresses, kMaxInterfaces);
			int32 written = 0;
			for (int i = 0; i < addressCount; i++) {
				if (config.ifc_buf != 0
					&& written + (int32)sizeof(linux_ifreq)
						> config.ifc_len) {
					break;
				}
				if (config.ifc_buf != 0) {
					const NetInterface* interface = find_interface_by_index(
						interfaces, count, addresses[i].index);
					linux_ifreq request;
					memset(&request, 0, sizeof(request));
					strlcpy(request.ifr_name,
						interface != NULL ? interface->name : "?",
						LX_IFNAMSIZ);
					linux_inet_address(&request.addr, addresses[i].address);
					if (renguin_copy_out(config.ifc_buf + written, &request,
							sizeof(request)) != B_OK) {
						return -LX_EFAULT;
					}
				}
				written += sizeof(linux_ifreq);
			}
			config.ifc_len = written;
			if (renguin_copy_out(arg, &config, sizeof(int32)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		case LX_SIOCATMARK:
		{
			int result = _user_sockatmark(fd);
			if (result < 0)
				return renguin_encode(result);
			int32 value = result;
			return renguin_copy_out(arg, &value, sizeof(value)) == B_OK
				? 0 : -LX_EFAULT;
		}
	}

	*_handled = false;
	return -LX_ENOTTY;
}
