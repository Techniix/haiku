/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux inotify interface: a file descriptor that reports changes to
 * watched files and directories as struct inotify_event records read()
 * from it. Names, values and the record layout are those of Linux. The
 * descriptor works with poll(), select() and epoll. Link with -lgnu.
 *
 * Differences from Linux (see 3rdparty/ce/PORTING.md, "inotify"):
 *  - Events for entries of a watched directory carry the entry's name when
 *    the watch knows it: entries present when the watch was added and
 *    entries created or moved in since. That covers every entry, except
 *    the rare entries renamed by an unwatched hard link.
 *  - IN_DELETE_SELF is reported when the last name of the node is
 *    removed, even if it is still open (Linux waits for the last close).
 *  - IN_UNMOUNT is never reported; IN_EXCL_UNLINK is accepted and has no
 *    effect.
 */
#ifndef _GNU_SYS_INOTIFY_H
#define _GNU_SYS_INOTIFY_H


#include <fcntl.h>
#include <stdint.h>
#include <sys/cdefs.h>


/* flags for inotify_init1() */
#define IN_CLOEXEC			O_CLOEXEC
#define IN_NONBLOCK			O_NONBLOCK

/* events (for inotify_add_watch() and in struct inotify_event::mask) */
#define IN_ACCESS			0x00000001	/* file was read */
#define IN_MODIFY			0x00000002	/* file was written or truncated */
#define IN_ATTRIB			0x00000004	/* metadata or attributes changed */
#define IN_CLOSE_WRITE		0x00000008	/* writable file was closed */
#define IN_CLOSE_NOWRITE	0x00000010	/* read-only file was closed */
#define IN_CLOSE			(IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_OPEN				0x00000020	/* file was opened */
#define IN_MOVED_FROM		0x00000040	/* entry moved out of the directory */
#define IN_MOVED_TO			0x00000080	/* entry moved into the directory */
#define IN_MOVE				(IN_MOVED_FROM | IN_MOVED_TO)
#define IN_CREATE			0x00000100	/* entry created in the directory */
#define IN_DELETE			0x00000200	/* entry deleted from the directory */
#define IN_DELETE_SELF		0x00000400	/* watched node was deleted */
#define IN_MOVE_SELF		0x00000800	/* watched node was moved */
#define IN_ALL_EVENTS		0x00000fff

/* reported without being asked for */
#define IN_UNMOUNT			0x00002000	/* not reported on Renga */
#define IN_Q_OVERFLOW		0x00004000	/* events were dropped */
#define IN_IGNORED			0x00008000	/* the watch was removed */

/* flags for inotify_add_watch() */
#define IN_ONLYDIR			0x01000000	/* fail unless a directory */
#define IN_DONT_FOLLOW		0x02000000	/* do not follow a final symlink */
#define IN_EXCL_UNLINK		0x04000000	/* accepted, no effect */
#define IN_MASK_CREATE		0x10000000	/* fail if already watched */
#define IN_MASK_ADD			0x20000000	/* add to an existing mask */
#define IN_ISDIR			0x40000000	/* (in events) subject is a dir */
#define IN_ONESHOT			0x80000000	/* remove after one event */


struct inotify_event {
	int			wd;			/* watch descriptor */
	uint32_t	mask;		/* IN_* */
	uint32_t	cookie;		/* pairs IN_MOVED_FROM with IN_MOVED_TO */
	uint32_t	len;		/* bytes in name, including NUL padding */
	char		name[];		/* entry name for directory watches */
};


__BEGIN_DECLS


int		inotify_init(void);
int		inotify_init1(int flags);
int		inotify_add_watch(int fd, const char* path, uint32_t mask);
int		inotify_rm_watch(int fd, int wd);


__END_DECLS


#endif	/* _GNU_SYS_INOTIFY_H */
