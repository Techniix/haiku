/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Generated from lkpi_errno_list.h: replace Haiku errno values with FreeBSD
 * ones in LinuxKPI translation units (see lkpi_errno.h).
 */
#undef EPERM
#define EPERM	1
#undef ENOENT
#define ENOENT	2
#undef ESRCH
#define ESRCH	3
#undef EINTR
#define EINTR	4
#undef EIO
#define EIO	5
#undef ENXIO
#define ENXIO	6
#undef E2BIG
#define E2BIG	7
#undef ENOEXEC
#define ENOEXEC	8
#undef EBADF
#define EBADF	9
#undef ECHILD
#define ECHILD	10
#undef EDEADLK
#define EDEADLK	11
#undef ENOMEM
#define ENOMEM	12
#undef EACCES
#define EACCES	13
#undef EFAULT
#define EFAULT	14
#undef ENOTBLK
#define ENOTBLK	15
#undef EBUSY
#define EBUSY	16
#undef EEXIST
#define EEXIST	17
#undef EXDEV
#define EXDEV	18
#undef ENODEV
#define ENODEV	19
#undef ENOTDIR
#define ENOTDIR	20
#undef EISDIR
#define EISDIR	21
#undef EINVAL
#define EINVAL	22
#undef ENFILE
#define ENFILE	23
#undef EMFILE
#define EMFILE	24
#undef ENOTTY
#define ENOTTY	25
#undef ETXTBSY
#define ETXTBSY	26
#undef EFBIG
#define EFBIG	27
#undef ENOSPC
#define ENOSPC	28
#undef ESPIPE
#define ESPIPE	29
#undef EROFS
#define EROFS	30
#undef EMLINK
#define EMLINK	31
#undef EPIPE
#define EPIPE	32
#undef EDOM
#define EDOM	33
#undef ERANGE
#define ERANGE	34
#undef EAGAIN
#define EAGAIN	35
#undef EINPROGRESS
#define EINPROGRESS	36
#undef EALREADY
#define EALREADY	37
#undef ENOTSOCK
#define ENOTSOCK	38
#undef EDESTADDRREQ
#define EDESTADDRREQ	39
#undef EMSGSIZE
#define EMSGSIZE	40
#undef EPROTOTYPE
#define EPROTOTYPE	41
#undef ENOPROTOOPT
#define ENOPROTOOPT	42
#undef EPROTONOSUPPORT
#define EPROTONOSUPPORT	43
#undef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT	44
#undef EOPNOTSUPP
#define EOPNOTSUPP	45
#undef EPFNOSUPPORT
#define EPFNOSUPPORT	46
#undef EAFNOSUPPORT
#define EAFNOSUPPORT	47
#undef EADDRINUSE
#define EADDRINUSE	48
#undef EADDRNOTAVAIL
#define EADDRNOTAVAIL	49
#undef ENETDOWN
#define ENETDOWN	50
#undef ENETUNREACH
#define ENETUNREACH	51
#undef ENETRESET
#define ENETRESET	52
#undef ECONNABORTED
#define ECONNABORTED	53
#undef ECONNRESET
#define ECONNRESET	54
#undef ENOBUFS
#define ENOBUFS	55
#undef EISCONN
#define EISCONN	56
#undef ENOTCONN
#define ENOTCONN	57
#undef ESHUTDOWN
#define ESHUTDOWN	58
#undef ETOOMANYREFS
#define ETOOMANYREFS	59
#undef ETIMEDOUT
#define ETIMEDOUT	60
#undef ECONNREFUSED
#define ECONNREFUSED	61
#undef ELOOP
#define ELOOP	62
#undef ENAMETOOLONG
#define ENAMETOOLONG	63
#undef EHOSTDOWN
#define EHOSTDOWN	64
#undef EHOSTUNREACH
#define EHOSTUNREACH	65
#undef ENOTEMPTY
#define ENOTEMPTY	66
#undef EPROCLIM
#define EPROCLIM	67
#undef EUSERS
#define EUSERS	68
#undef EDQUOT
#define EDQUOT	69
#undef ESTALE
#define ESTALE	70
#undef EREMOTE
#define EREMOTE	71
#undef EBADRPC
#define EBADRPC	72
#undef ERPCMISMATCH
#define ERPCMISMATCH	73
#undef EPROGUNAVAIL
#define EPROGUNAVAIL	74
#undef EPROGMISMATCH
#define EPROGMISMATCH	75
#undef EPROCUNAVAIL
#define EPROCUNAVAIL	76
#undef ENOLCK
#define ENOLCK	77
#undef ENOSYS
#define ENOSYS	78
#undef EFTYPE
#define EFTYPE	79
#undef EAUTH
#define EAUTH	80
#undef ENEEDAUTH
#define ENEEDAUTH	81
#undef EIDRM
#define EIDRM	82
#undef ENOMSG
#define ENOMSG	83
#undef EOVERFLOW
#define EOVERFLOW	84
#undef ECANCELED
#define ECANCELED	85
#undef EILSEQ
#define EILSEQ	86
#undef ENOATTR
#define ENOATTR	87
#undef EDOOFUS
#define EDOOFUS	88
#undef EBADMSG
#define EBADMSG	89
#undef EMULTIHOP
#define EMULTIHOP	90
#undef ENOLINK
#define ENOLINK	91
#undef EPROTO
#define EPROTO	92
#undef ENOTCAPABLE
#define ENOTCAPABLE	93
#undef ECAPMODE
#define ECAPMODE	94
#undef ENOTRECOVERABLE
#define ENOTRECOVERABLE	95
#undef EOWNERDEAD
#define EOWNERDEAD	96
#undef EINTEGRITY
#define EINTEGRITY	97
#undef ELAST
#define ELAST	97

/* FreeBSD aliases (sys/errno.h). Haiku defines these with its own values,
 * so they must be redefined too: a timed-out sleepq wait returns FreeBSD
 * EWOULDBLOCK (== EAGAIN), and LinuxKPI turns any other value into
 * -ERESTARTSYS (linux_add_to_sleepqueue()). */
#undef EWOULDBLOCK
#define EWOULDBLOCK	EAGAIN
#undef ENOTSUP
#define ENOTSUP	EOPNOTSUPP
