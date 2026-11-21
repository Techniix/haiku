/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * System V IPC for Linux programs: shared memory (shmget, shmat, shmdt,
 * shmctl), semaphores (semget, semop, semtimedop, semctl) and message
 * queues (msgget, msgsnd, msgrcv, msgctl), on Haiku's own XSI IPC
 * (src/system/kernel/posix/xsi_*.cpp). Linux and native programs share the
 * same keys and objects, so a Linux client can talk to a native server.
 *
 * What is translated:
 *  - command numbers: semctl's GETPID..SETALL are 11-17 on Linux, 3-9 on
 *    Haiku; the IPC_64 flag (0x100) glibc and musl add is dropped;
 *  - SEM_UNDO is 0x1000 on Linux, 10 on Haiku (IPC_NOWAIT, MSG_NOERROR,
 *    SHM_RDONLY and SHM_RND have the same values);
 *  - the *_ds structures (Linux's x86-64 ipc64_perm, shmid64_ds, semid64_ds
 *    and msqid64_ds layouts);
 *  - semctl's fourth argument, a union passed by value on Linux and by
 *    pointer to Haiku's kernel.
 *
 * Deviations: semtimedop() with a timeout retries every 2 ms with
 * IPC_NOWAIT until the time is up (Haiku's semop() has no timeout);
 * msgrcv()'s MSG_EXCEPT and MSG_COPY, SHM_LOCK/SHM_UNLOCK and the *_INFO
 * and *_STAT listing commands (used by ipcs) are EINVAL.
 *
 * FreeBSD counterpart: sys/compat/linux/linux_ipc.c.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <algorithm>

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>

#include <posix/xsi_message_queue.h>
#include <posix/xsi_semaphore.h>
#include <posix/xsi_shared_memory.h>
#include <util/AutoLock.h>


#define LX_IPC_RMID		0
#define LX_IPC_SET		1
#define LX_IPC_STAT		2
#define LX_IPC_INFO		3
#define LX_IPC_64		0x100

#define LX_IPC_CREAT	01000
#define LX_IPC_EXCL		02000
#define LX_IPC_NOWAIT	04000

#define LX_GETPID		11
#define LX_GETVAL		12
#define LX_GETALL		13
#define LX_GETNCNT		14
#define LX_GETZCNT		15
#define LX_SETVAL		16
#define LX_SETALL		17

#define LX_SEM_UNDO		0x1000

#define LX_SHM_RDONLY	010000
#define LX_SHM_RND		020000
#define LX_SHM_REMAP	040000
#define LX_SHM_EXEC		0100000

#define LX_MSG_NOERROR	010000
#define LX_MSG_EXCEPT	020000
#define LX_MSG_COPY		040000

#define LX_SEMOPM		500		// Linux's limit of operations per call


// x86-64 Linux layouts (arch/x86/include/uapi/asm/*buf.h)
struct linux_ipc64_perm {
	int32	key;
	uint32	uid;
	uint32	gid;
	uint32	cuid;
	uint32	cgid;
	uint32	mode;
	uint16	seq;
	uint16	pad2;
	uint64	unused1;
	uint64	unused2;
};

struct linux_shmid64_ds {
	linux_ipc64_perm perm;
	uint64	segsz;
	int64	atime;
	int64	dtime;
	int64	ctime;
	int32	cpid;
	int32	lpid;
	uint64	nattch;
	uint64	unused4;
	uint64	unused5;
};

struct linux_semid64_ds {
	linux_ipc64_perm perm;
	int64	otime;
	uint64	unused1;
	int64	ctime;
	uint64	unused2;
	uint64	nsems;
	uint64	unused3;
	uint64	unused4;
};

struct linux_msqid64_ds {
	linux_ipc64_perm perm;
	int64	stime;
	int64	rtime;
	int64	ctime;
	uint64	cbytes;
	uint64	qnum;
	uint64	qbytes;
	int32	lspid;
	int32	lrpid;
	uint64	unused4;
	uint64	unused5;
};

struct linux_sembuf {
	uint16	sem_num;
	int16	sem_op;
	int16	sem_flg;
};


static_assert(sizeof(linux_ipc64_perm) == 48, "ipc64_perm layout");
static_assert(sizeof(linux_shmid64_ds) == 112, "shmid64_ds layout");
static_assert(sizeof(linux_semid64_ds) == 104, "semid64_ds layout");
static_assert(sizeof(linux_msqid64_ds) == 120, "msqid64_ds layout");
static_assert(sizeof(linux_sembuf) == sizeof(struct sembuf), "sembuf");


static void
perm_from_haiku(const ipc_perm& perm, linux_ipc64_perm* _perm)
{
	memset(_perm, 0, sizeof(*_perm));
	_perm->key = perm.key;
	_perm->uid = perm.uid;
	_perm->gid = perm.gid;
	_perm->cuid = perm.cuid;
	_perm->cgid = perm.cgid;
	_perm->mode = perm.mode & 0777;
}


static void
perm_to_haiku(const linux_ipc64_perm& perm, ipc_perm* _perm)
{
	_perm->uid = perm.uid;
	_perm->gid = perm.gid;
	_perm->mode = perm.mode & 0777;
}


//! Linux's key/size/flags creation bits for Haiku (the same values).
static int
get_flags(uint64 flags)
{
	return (int)(flags & (LX_IPC_CREAT | LX_IPC_EXCL | 0777));
}


//! The command without the IPC_64 flag.
static int
plain_command(uint64 command)
{
	return (int)(command & ~(uint64)LX_IPC_64);
}


// #pragma mark - shared memory


int64
renguin_sys_shmget(uint64 key, uint64 size, uint64 flags)
{
	// SHM_HUGETLB and SHM_NORESERVE are hints here
	return renguin_encode(_user_xsi_shmget((key_t)(int32)key, (size_t)size,
		get_flags(flags)));
}


int64
renguin_sys_shmat(uint64 id, uint64 address, uint64 flags)
{
	if ((flags & LX_SHM_REMAP) != 0 && address == 0)
		return -LX_EINVAL;
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userAddress = renguin_scratch_alloc(&scratch, sizeof(void*));
	if (userAddress == 0)
		return -LX_EFAULT;
	// SHM_EXEC: Haiku's areas are mapped readable and writable, not
	// executable; nothing in the ladder needs executable shared memory.
	status_t status = _user_xsi_shmat((int)id, (const void*)(addr_t)address,
		(int)(flags & (LX_SHM_RDONLY | LX_SHM_RND)),
		(void**)(addr_t)userAddress);
	if (status != B_OK)
		return renguin_encode(status);
	void* attached;
	if (user_memcpy(&attached, (void*)(addr_t)userAddress, sizeof(attached))
			!= B_OK) {
		return -LX_EFAULT;
	}
	return (int64)(addr_t)attached;
}


int64
renguin_sys_shmdt(uint64 address)
{
	return renguin_encode(_user_xsi_shmdt((const void*)(addr_t)address));
}


int64
renguin_sys_shmctl(uint64 id, uint64 command, uint64 buffer)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	int cmd = plain_command(command);
	switch (cmd) {
		case LX_IPC_RMID:
			return renguin_encode(_user_xsi_shmctl((int)id, IPC_RMID, NULL));

		case LX_IPC_STAT:
		case LX_IPC_SET:
		{
			uint64 user = renguin_scratch_alloc(&scratch, sizeof(shmid_ds));
			if (user == 0)
				return -LX_EFAULT;
			status_t status = _user_xsi_shmctl((int)id, IPC_STAT,
				(shmid_ds*)(addr_t)user);
			if (status != B_OK)
				return renguin_encode(status);
			shmid_ds haiku;
			if (user_memcpy(&haiku, (void*)(addr_t)user, sizeof(haiku))
					!= B_OK) {
				return -LX_EFAULT;
			}
			if (cmd == LX_IPC_SET) {
				linux_shmid64_ds linux;
				if (renguin_copy_in(&linux, buffer, sizeof(linux)) != B_OK)
					return -LX_EFAULT;
				perm_to_haiku(linux.perm, &haiku.shm_perm);
				if (user_memcpy((void*)(addr_t)user, &haiku, sizeof(haiku))
						!= B_OK) {
					return -LX_EFAULT;
				}
				return renguin_encode(_user_xsi_shmctl((int)id, IPC_SET,
					(shmid_ds*)(addr_t)user));
			}
			linux_shmid64_ds linux;
			memset(&linux, 0, sizeof(linux));
			perm_from_haiku(haiku.shm_perm, &linux.perm);
			linux.segsz = haiku.shm_segsz;
			linux.atime = haiku.shm_atime;
			linux.dtime = haiku.shm_dtime;
			linux.ctime = haiku.shm_ctime;
			linux.cpid = haiku.shm_cpid;
			linux.lpid = haiku.shm_lpid;
			linux.nattch = haiku.shm_nattch;
			if (renguin_copy_out(buffer, &linux, sizeof(linux)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}
	}
	// SHM_LOCK, SHM_UNLOCK, IPC_INFO, SHM_INFO, SHM_STAT(_ANY)
	return -LX_EINVAL;
}


// #pragma mark - semaphores


int64
renguin_sys_semget(uint64 key, uint64 count, uint64 flags)
{
	return renguin_encode(_user_xsi_semget((key_t)(int32)key, (int)count,
		get_flags(flags)));
}


/*!	semop() and semtimedop(): the operations with Haiku's flag values. With
	\a timeout (a user timespec), a blocking set of operations is retried
	with IPC_NOWAIT until the time is up.
*/
int64
renguin_sys_semtimedop(uint64 id, uint64 operations, uint64 count,
	uint64 timeout)
{
	if (count == 0 || count > LX_SEMOPM)
		return count == 0 ? -LX_EINVAL : -LX_E2BIG;

	bigtime_t deadline = B_INFINITE_TIMEOUT;
	if (timeout != 0) {
		linux_timespec spec;
		if (renguin_copy_in(&spec, timeout, sizeof(spec)) != B_OK)
			return -LX_EFAULT;
		if (spec.tv_sec < 0 || spec.tv_nsec < 0 || spec.tv_nsec >= 1000000000)
			return -LX_EINVAL;
		deadline = system_time() + spec.tv_sec * 1000000
			+ (spec.tv_nsec + 999) / 1000;
	}

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	size_t size = count * sizeof(struct sembuf);
	uint64 user = renguin_scratch_alloc(&scratch, size);
	if (user == 0)
		return -LX_EFAULT;

	bool allNoWait = true;
	struct sembuf ops[32];
	for (uint64 done = 0; done < count; ) {
		uint64 chunk = std::min(count - done, (uint64)B_COUNT_OF(ops));
		linux_sembuf linuxOps[B_COUNT_OF(ops)];
		if (renguin_copy_in(linuxOps, operations + done * sizeof(linux_sembuf),
				chunk * sizeof(linux_sembuf)) != B_OK) {
			return -LX_EFAULT;
		}
		for (uint64 i = 0; i < chunk; i++) {
			ops[i].sem_num = linuxOps[i].sem_num;
			ops[i].sem_op = linuxOps[i].sem_op;
			int flags = linuxOps[i].sem_flg;
			ops[i].sem_flg = ((flags & LX_IPC_NOWAIT) != 0 ? IPC_NOWAIT : 0)
				| ((flags & LX_SEM_UNDO) != 0 ? SEM_UNDO : 0);
			if ((flags & LX_IPC_NOWAIT) == 0) {
				allNoWait = false;
				if (deadline != B_INFINITE_TIMEOUT)
					ops[i].sem_flg |= IPC_NOWAIT;
			}
		}
		if (user_memcpy((void*)(addr_t)(user + done * sizeof(struct sembuf)),
				ops, chunk * sizeof(struct sembuf)) != B_OK) {
			return -LX_EFAULT;
		}
		done += chunk;
	}

	while (true) {
		status_t status = _user_xsi_semop((int)id,
			(struct sembuf*)(addr_t)user, (size_t)count);
		if (status != EAGAIN || deadline == B_INFINITE_TIMEOUT || allNoWait)
			return renguin_encode(status);
		bigtime_t now = system_time();
		if (now >= deadline)
			return -LX_EAGAIN;
		status = snooze_etc(std::min(deadline - now, (bigtime_t)2000),
			B_SYSTEM_TIMEBASE, B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT);
		if (status == B_INTERRUPTED)
			return -LX_EINTR;
	}
}


int64
renguin_sys_semctl(uint64 id, uint64 number, uint64 command, uint64 argument)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	union semun args;
	memset(&args, 0, sizeof(args));
	int haikuCommand;
	int cmd = plain_command(command);
	switch (cmd) {
		case LX_IPC_RMID:	haikuCommand = IPC_RMID; break;
		case LX_GETPID:		haikuCommand = GETPID; break;
		case LX_GETVAL:		haikuCommand = GETVAL; break;
		case LX_GETNCNT:	haikuCommand = GETNCNT; break;
		case LX_GETZCNT:	haikuCommand = GETZCNT; break;
		case LX_SETVAL:
			haikuCommand = SETVAL;
			args.val = (int)argument;
			break;
		case LX_GETALL:
		case LX_SETALL:
			// unsigned short arrays on both systems, read and written in
			// the program's memory by Haiku's kernel
			haikuCommand = cmd == LX_GETALL ? GETALL : SETALL;
			args.array = (unsigned short*)(addr_t)argument;
			break;

		case LX_IPC_STAT:
		case LX_IPC_SET:
		{
			uint64 user = renguin_scratch_alloc(&scratch, sizeof(semid_ds));
			if (user == 0)
				return -LX_EFAULT;
			args.buf = (semid_ds*)(addr_t)user;
			uint64 userArgs = renguin_scratch_put(&scratch, &args,
				sizeof(args));
			if (userArgs == 0)
				return -LX_EFAULT;
			int result = _user_xsi_semctl((int)id, 0, IPC_STAT,
				(union semun*)(addr_t)userArgs);
			if (result < 0)
				return renguin_encode(result);
			semid_ds haiku;
			if (user_memcpy(&haiku, (void*)(addr_t)user, sizeof(haiku))
					!= B_OK) {
				return -LX_EFAULT;
			}
			if (cmd == LX_IPC_SET) {
				linux_semid64_ds linux;
				if (renguin_copy_in(&linux, argument, sizeof(linux)) != B_OK)
					return -LX_EFAULT;
				perm_to_haiku(linux.perm, &haiku.sem_perm);
				if (user_memcpy((void*)(addr_t)user, &haiku, sizeof(haiku))
						!= B_OK) {
					return -LX_EFAULT;
				}
				return renguin_encode(_user_xsi_semctl((int)id, 0, IPC_SET,
					(union semun*)(addr_t)userArgs));
			}
			linux_semid64_ds linux;
			memset(&linux, 0, sizeof(linux));
			perm_from_haiku(haiku.sem_perm, &linux.perm);
			linux.otime = haiku.sem_otime;
			linux.ctime = haiku.sem_ctime;
			linux.nsems = haiku.sem_nsems;
			if (renguin_copy_out(argument, &linux, sizeof(linux)) != B_OK)
				return -LX_EFAULT;
			return 0;
		}

		default:
			// IPC_INFO, SEM_INFO, SEM_STAT(_ANY)
			return -LX_EINVAL;
	}
	uint64 userArgs = renguin_scratch_put(&scratch, &args, sizeof(args));
	if (userArgs == 0)
		return -LX_EFAULT;
	return renguin_encode(_user_xsi_semctl((int)id, (int)number, haikuCommand,
		(union semun*)(addr_t)userArgs));
}


// #pragma mark - message queues


int64
renguin_sys_msgget(uint64 key, uint64 flags)
{
	return renguin_encode(_user_xsi_msgget((key_t)(int32)key,
		get_flags(flags)));
}


int64
renguin_sys_msgsnd(uint64 id, uint64 message, uint64 size, uint64 flags)
{
	// struct msgbuf is a long and the text on both systems
	return renguin_encode(_user_xsi_msgsnd((int)id,
		(const void*)(addr_t)message, (size_t)size,
		(int)(flags & LX_IPC_NOWAIT)));
}


int64
renguin_sys_msgrcv(uint64 id, uint64 message, uint64 size, uint64 type,
	uint64 flags)
{
	if ((flags & (LX_MSG_EXCEPT | LX_MSG_COPY)) != 0)
		return -LX_EINVAL;
	return renguin_encode(_user_xsi_msgrcv((int)id, (void*)(addr_t)message,
		(size_t)size, (long)type,
		(int)(flags & (LX_IPC_NOWAIT | LX_MSG_NOERROR))));
}


int64
renguin_sys_msgctl(uint64 id, uint64 command, uint64 buffer)
{
	int cmd = plain_command(command);
	if (cmd == LX_IPC_RMID)
		return renguin_encode(_user_xsi_msgctl((int)id, IPC_RMID, NULL));
	if (cmd != LX_IPC_STAT && cmd != LX_IPC_SET)
		return -LX_EINVAL;	// IPC_INFO, MSG_INFO, MSG_STAT(_ANY)

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 user = renguin_scratch_alloc(&scratch, sizeof(msqid_ds));
	if (user == 0)
		return -LX_EFAULT;
	int result = _user_xsi_msgctl((int)id, IPC_STAT, (msqid_ds*)(addr_t)user);
	if (result < 0)
		return renguin_encode(result);
	msqid_ds haiku;
	if (user_memcpy(&haiku, (void*)(addr_t)user, sizeof(haiku)) != B_OK)
		return -LX_EFAULT;
	if (cmd == LX_IPC_SET) {
		linux_msqid64_ds linux;
		if (renguin_copy_in(&linux, buffer, sizeof(linux)) != B_OK)
			return -LX_EFAULT;
		perm_to_haiku(linux.perm, &haiku.msg_perm);
		haiku.msg_qbytes = linux.qbytes > 0xffffffff ? 0xffffffff
			: (msglen_t)linux.qbytes;
		if (user_memcpy((void*)(addr_t)user, &haiku, sizeof(haiku)) != B_OK)
			return -LX_EFAULT;
		return renguin_encode(_user_xsi_msgctl((int)id, IPC_SET,
			(msqid_ds*)(addr_t)user));
	}
	linux_msqid64_ds linux;
	memset(&linux, 0, sizeof(linux));
	perm_from_haiku(haiku.msg_perm, &linux.perm);
	linux.stime = haiku.msg_stime;
	linux.rtime = haiku.msg_rtime;
	linux.ctime = haiku.msg_ctime;
	linux.qnum = haiku.msg_qnum;
	linux.qbytes = haiku.msg_qbytes;
	linux.lspid = haiku.msg_lspid;
	linux.lrpid = haiku.msg_lrpid;
	if (renguin_copy_out(buffer, &linux, sizeof(linux)) != B_OK)
		return -LX_EFAULT;
	return 0;
}
