/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux signals: rt_sigaction, rt_sigprocmask, signal delivery with Linux
 * signal frames, rt_sigreturn, sigaltstack, rt_sigpending, rt_sigsuspend,
 * pause, rt_sigtimedwait, rt_sigqueueinfo/rt_tgsigqueueinfo, and the
 * interval timers (alarm, setitimer, getitimer).
 *
 * FreeBSD counterparts: sys/compat/linux/linux_signal.c (linux_do_sigaction(),
 * linux_rt_sigprocmask(), linux_rt_sigtimedwait(), ...) and
 * sys/amd64/linux/linux_sysvec.c (linux_rt_sendsig(), linux_rt_sigreturn()).
 *
 * Haiku keeps doing the bookkeeping: Linux handlers are registered as Haiku
 * signal actions (translated numbers, masks and flags), so Haiku decides when
 * a handler runs, applies its mask, SA_RESETHAND and SA_NODEFER, and runs
 * default actions. Only the frame differs: the kernel patch hands frame setup
 * for foreign threads to renguin_setup_signal_frame(), which builds Linux's
 * struct rt_sigframe (arch/x86/kernel/signal_64.c) and enters the handler
 * the Linux way: rdi = signal, rsi = &siginfo, rdx = &ucontext, the
 * SA_RESTORER trampoline as return address. The trampoline calls
 * rt_sigreturn, which restores the registers, the FPU state, the signal mask
 * and the alternate stack from that frame.
 *
 * Syscall restart follows Linux: when the interrupted system call is to be
 * restarted (Haiku decided that from SA_RESTART), the saved context already
 * points back at the syscall instruction with the call number in rax, so
 * returning from the handler re-issues it.
 *
 * What the module keeps itself: SA_RESTORER and the Linux-side flags and mask
 * of each action (renguin_team::sigactions), and the blocked state of Linux
 * signals Haiku has no number for (renguin_thread::unmapped_blocked).
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stddef.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <arch/x86/arch_cpu.h>
#include <arch/x86/arch_thread.h>
#include <arch/x86/64/iframe.h>
#include <kernel.h>
#include <ksignal.h>
#include <signal_defs.h>
#include <team.h>
#include <thread.h>
#include <user_timer_defs.h>
#include <UserTimer.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>

#include <signal.h>


// sa_flags (include/uapi/asm-generic/signal-defs.h, x86).
#define LX_SA_NOCLDSTOP		0x00000001
#define LX_SA_NOCLDWAIT		0x00000002
#define LX_SA_SIGINFO		0x00000004
#define LX_SA_RESTORER		0x04000000
#define LX_SA_ONSTACK		0x08000000
#define LX_SA_RESTART		0x10000000
#define LX_SA_NODEFER		0x40000000
#define LX_SA_RESETHAND		0x80000000

// sigprocmask how
#define LX_SIG_BLOCK		0
#define LX_SIG_UNBLOCK		1
#define LX_SIG_SETMASK		2

// sigaltstack
#define LX_SS_ONSTACK		1
#define LX_SS_DISABLE		2
#define LX_SS_AUTODISARM	(1u << 31)
#define LX_MINSIGSTKSZ		2048

// si_code values (include/uapi/asm-generic/siginfo.h)
#define LX_SI_USER			0
#define LX_SI_KERNEL		0x80
#define LX_SI_QUEUE			(-1)
#define LX_SI_TIMER			(-2)
#define LX_SI_MESGQ			(-3)
#define LX_SI_ASYNCIO		(-4)
#define LX_SI_TKILL			(-6)

// ucontext uc_flags
#define LX_UC_SIGCONTEXT_SS		0x2
#define LX_UC_STRICT_RESTORE_SS	0x4

// interval timers
#define LX_ITIMER_REAL		0
#define LX_ITIMER_VIRTUAL	1
#define LX_ITIMER_PROF		2

static const size_t kRedZoneSize = 128;
static const size_t kFXSaveSize = 512;
static const size_t kFXSaveSoftwareOffset = 464;
	// bytes 464-511 of the FXSAVE area are left to software; Linux puts its
	// xstate description ("fpx_sw_bytes") there. Zero means "legacy only".


// Linux siginfo_t (128 bytes).
struct linux_siginfo {
	int32	si_signo;
	int32	si_errno;
	int32	si_code;
	int32	__pad0;
	union {
		uint8	raw[112];
		struct {
			int32	pid;
			uint32	uid;
			uint64	value;
		} rt;					// kill, sigqueue, timers
		struct {
			int32	pid;
			uint32	uid;
			int32	status;
			int32	__pad;
			int64	utime;
			int64	stime;
		} child;
		struct {
			uint64	addr;
		} fault;
		struct {
			int64	band;
			int32	fd;
		} poll;
	};
};
static_assert(sizeof(linux_siginfo) == 128, "Linux ABI");

// Linux struct sigcontext (x86-64), as in the ucontext.
struct linux_sigcontext {
	uint64	r8, r9, r10, r11, r12, r13, r14, r15;
	uint64	rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
	uint64	eflags;
	uint16	cs, gs, fs, ss;
	uint64	err;
	uint64	trapno;
	uint64	oldmask;
	uint64	cr2;
	uint64	fpstate;			// struct _fpstate*, or 0
	uint64	reserved1[8];
};
static_assert(sizeof(linux_sigcontext) == 256, "Linux ABI");

// Linux kernel struct ucontext (not glibc's larger ucontext_t).
struct linux_ucontext {
	uint64				uc_flags;
	uint64				uc_link;
	linux_stack			uc_stack;
	linux_sigcontext	uc_mcontext;
	uint64				uc_sigmask;
};
static_assert(sizeof(linux_ucontext) == 304, "Linux ABI");

// Linux struct rt_sigframe (x86-64). The FPU state lies above it.
struct linux_rt_sigframe {
	uint64			pretcode;	// the SA_RESTORER trampoline
	linux_ucontext	uc;
	linux_siginfo	info;
};
static_assert(sizeof(linux_rt_sigframe) == 440, "Linux ABI");

struct linux_itimerval {
	linux_timeval	it_interval;
	linux_timeval	it_value;
};


// #pragma mark - numbers, masks, flags


static inline uint64
linux_bit(int signal)
{
	return (uint64)1 << (signal - 1);
}


//! Linux signals that have no Haiku number: their blocked state is kept by
//! the module, and they can't be delivered.
static uint64
unmapped_signals()
{
	uint64 mask = 0;
	for (int signal = 1; signal <= LX_NSIG; signal++) {
		if (renguin_signal_to_haiku(signal) == 0)
			mask |= linux_bit(signal);
	}
	return mask;
}


static sigset_t
mask_to_haiku(uint64 linuxMask)
{
	sigset_t mask = 0;
	for (int signal = 1; signal <= LX_NSIG; signal++) {
		if ((linuxMask & linux_bit(signal)) == 0)
			continue;
		int haikuSignal = renguin_signal_to_haiku(signal);
		if (haikuSignal != 0)
			mask |= SIGNAL_TO_MASK(haikuSignal);
	}
	return mask;
}


static uint64
mask_from_haiku(sigset_t haikuMask)
{
	uint64 mask = 0;
	for (int haikuSignal = 1; haikuSignal <= 64; haikuSignal++) {
		if ((haikuMask & SIGNAL_TO_MASK(haikuSignal)) == 0)
			continue;
		int signal = renguin_signal_from_haiku(haikuSignal);
		if (signal != 0)
			mask |= linux_bit(signal);
	}
	return mask;
}


static int
flags_to_haiku(uint64 flags)
{
	int haikuFlags = 0;
	if ((flags & LX_SA_NOCLDSTOP) != 0)
		haikuFlags |= SA_NOCLDSTOP;
	if ((flags & LX_SA_NOCLDWAIT) != 0)
		haikuFlags |= SA_NOCLDWAIT;
	if ((flags & LX_SA_SIGINFO) != 0)
		haikuFlags |= SA_SIGINFO;
	if ((flags & LX_SA_ONSTACK) != 0)
		haikuFlags |= SA_ONSTACK;
	if ((flags & LX_SA_RESTART) != 0)
		haikuFlags |= SA_RESTART;
	if ((flags & LX_SA_NODEFER) != 0)
		haikuFlags |= SA_NODEFER;
	if ((flags & LX_SA_RESETHAND) != 0)
		haikuFlags |= SA_RESETHAND;
	return haikuFlags;
}


static uint64
flags_from_haiku(int haikuFlags)
{
	uint64 flags = 0;
	if ((haikuFlags & SA_NOCLDSTOP) != 0)
		flags |= LX_SA_NOCLDSTOP;
	if ((haikuFlags & SA_NOCLDWAIT) != 0)
		flags |= LX_SA_NOCLDWAIT;
	if ((haikuFlags & SA_SIGINFO) != 0)
		flags |= LX_SA_SIGINFO;
	if ((haikuFlags & SA_ONSTACK) != 0)
		flags |= LX_SA_ONSTACK;
	if ((haikuFlags & SA_RESTART) != 0)
		flags |= LX_SA_RESTART;
	if ((haikuFlags & SA_NODEFER) != 0)
		flags |= LX_SA_NODEFER;
	if ((haikuFlags & SA_RESETHAND) != 0)
		flags |= LX_SA_RESETHAND;
	return flags;
}


static int32
code_from_haiku(int32 code)
{
	if (code < 0)
		return code;	// already Linux's (SI_TKILL from tgkill())
	switch (code) {
		case SI_USER:		return LX_SI_USER;
		case SI_QUEUE:		return LX_SI_QUEUE;
		case SI_TIMER:		return LX_SI_TIMER;
		case SI_ASYNCIO:	return LX_SI_ASYNCIO;
		case SI_MESGQ:		return LX_SI_MESGQ;
	}
	// Haiku numbers each signal's codes from a multiple of ten (ILL_ILLOPC =
	// 10, FPE_INTDIV = 20, ..., POLL_IN = 70); Linux from 1, in the same
	// order.
	if (code >= ILL_ILLOPC && code <= POLL_HUP && code % 10 <= 7)
		return code % 10 + 1;
	return LX_SI_KERNEL;
}


static void
siginfo_from_haiku(const siginfo_t& info, int linuxSignal,
	linux_siginfo* _info)
{
	memset(_info, 0, sizeof(*_info));
	_info->si_signo = linuxSignal;
	_info->si_code = code_from_haiku(info.si_code);
	// si_errno stays 0: Linux doesn't set it for the signals Haiku sends
	// (Haiku puts EFAULT into its SIGSEGV).

	// As Linux's siginfo_layout(): signals sent by a process (SI_USER,
	// SI_TKILL, SI_QUEUE, ...) carry the sender; kernel-generated ones carry
	// what the signal is about.
	if (_info->si_code <= 0) {
		_info->rt.pid = info.si_pid;
		_info->rt.uid = info.si_uid;
		_info->rt.value = (uint64)(addr_t)info.si_value.sival_ptr;
		return;
	}

	switch (linuxSignal) {
		case LX_SIGSEGV:
		case LX_SIGBUS:
		case LX_SIGILL:
		case LX_SIGFPE:
		case LX_SIGTRAP:
			_info->fault.addr = (uint64)(addr_t)info.si_addr;
			break;

		case LX_SIGCHLD:
		{
			_info->child.pid = info.si_pid;
			_info->child.uid = info.si_uid;
			int status = info.si_status;
			if (info.si_code != CLD_EXITED) {
				// a signal number
				status = renguin_signal_from_haiku(status);
				if (status == 0)
					status = LX_SIGKILL;
			}
			_info->child.status = status;
			break;
		}

		case LX_SIGIO:
			_info->poll.band = info.si_band;
			break;

		default:
			_info->rt.pid = info.si_pid;
			_info->rt.uid = info.si_uid;
			_info->rt.value = (uint64)(addr_t)info.si_value.sival_ptr;
			break;
	}
}


// #pragma mark - alternate signal stack


static bool
on_signal_stack(Thread* thread, addr_t sp)
{
	// Linux's on_sig_stack(): the stack grows down from base + size.
	return thread->signal_stack_enabled && sp > thread->signal_stack_base
		&& sp - thread->signal_stack_base <= thread->signal_stack_size;
}


static void
get_signal_stack(Thread* thread, addr_t sp, linux_stack* stack)
{
	renguin_thread* data = (renguin_thread*)thread->personality_data;
	memset(stack, 0, sizeof(*stack));
	if (!thread->signal_stack_enabled) {
		stack->ss_flags = LX_SS_DISABLE;
	} else {
		stack->ss_sp = thread->signal_stack_base;
		stack->ss_size = thread->signal_stack_size;
		stack->ss_flags = on_signal_stack(thread, sp) ? LX_SS_ONSTACK : 0;
	}
	if (data != NULL && data->altstack_autodisarm)
		stack->ss_flags |= (int32)LX_SS_AUTODISARM;
}


//! Linux's do_sigaltstack(): returns 0 or a negative Linux errno.
static int64
set_signal_stack(Thread* thread, addr_t sp, const linux_stack& stack)
{
	if (on_signal_stack(thread, sp))
		return -LX_EPERM;

	uint32 mode = (uint32)stack.ss_flags & ~LX_SS_AUTODISARM;
	if (mode != LX_SS_DISABLE && mode != LX_SS_ONSTACK && mode != 0)
		return -LX_EINVAL;

	bool autodisarm = ((uint32)stack.ss_flags & LX_SS_AUTODISARM) != 0;
	renguin_thread* data = renguin_current_thread_data(autodisarm);
	if (autodisarm && data == NULL)
		return -LX_ENOMEM;

	if (mode == LX_SS_DISABLE) {
		thread->signal_stack_enabled = false;
		thread->signal_stack_base = 0;
		thread->signal_stack_size = 0;
	} else {
		if (stack.ss_size < LX_MINSIGSTKSZ)
			return -LX_ENOMEM;
		if (!IS_USER_ADDRESS((addr_t)stack.ss_sp)
			|| !IS_USER_ADDRESS((addr_t)(stack.ss_sp + stack.ss_size - 1))
			|| stack.ss_sp + stack.ss_size < stack.ss_sp) {
			return -LX_EFAULT;
		}
		thread->signal_stack_base = (addr_t)stack.ss_sp;
		thread->signal_stack_size = (size_t)stack.ss_size;
		thread->signal_stack_enabled = true;
	}
	if (data != NULL)
		data->altstack_autodisarm = autodisarm;
	return 0;
}


int64
renguin_sys_sigaltstack(uint64 newStack, uint64 oldStack)
{
	Thread* thread = thread_get_current_thread();
	iframe* frame = x86_get_user_iframe();
	addr_t sp = frame != NULL ? frame->user_sp : 0;

	linux_stack old;
	get_signal_stack(thread, sp, &old);

	if (newStack != 0) {
		linux_stack stack;
		if (renguin_copy_in(&stack, newStack, sizeof(stack)) != B_OK)
			return -LX_EFAULT;
		int64 result = set_signal_stack(thread, sp, stack);
		if (result != 0)
			return result;
	}

	if (oldStack != 0 && renguin_copy_out(oldStack, &old, sizeof(old)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


// #pragma mark - actions and masks


/*!	rt_sigaction. Signals with a Haiku number are registered with Haiku, so
	that it runs the handler (through renguin_setup_signal_frame()) or the
	default action; the rest are only remembered.
*/
int64
renguin_sys_rt_sigaction(uint64 signal, uint64 action, uint64 oldAction,
	uint64 sigsetSize)
{
	if (sigsetSize != sizeof(uint64))
		return -LX_EINVAL;
	if (signal < 1 || signal > LX_NSIG)
		return -LX_EINVAL;
	if (action != 0 && (signal == LX_SIGKILL || signal == LX_SIGSTOP))
		return -LX_EINVAL;

	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return -LX_EINVAL;

	if (signal == LX_SIGKILL || signal == LX_SIGSTOP) {
		// Only reading is allowed (see above), and Haiku refuses even that.
		renguin_sigaction old;
		memset(&old, 0, sizeof(old));
		if (oldAction != 0
			&& renguin_copy_out(oldAction, &old, sizeof(old)) != B_OK) {
			return -LX_EFAULT;
		}
		return 0;
	}

	renguin_sigaction act;
	if (action != 0 && renguin_copy_in(&act, action, sizeof(act)) != B_OK)
		return -LX_EFAULT;

	int haikuSignal = renguin_signal_to_haiku((int)signal);

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userAct = 0;
	uint64 userOld = 0;
	if (haikuSignal != 0) {
		userOld = renguin_scratch_alloc(&scratch, sizeof(struct sigaction));
		if (userOld == 0)
			return -LX_EFAULT;
		if (action != 0) {
			struct sigaction haikuAct;
			memset(&haikuAct, 0, sizeof(haikuAct));
			haikuAct.sa_handler = (__sighandler_t)(addr_t)act.handler;
			haikuAct.sa_mask = mask_to_haiku(act.mask);
			haikuAct.sa_flags = flags_to_haiku(act.flags);
			userAct = renguin_scratch_put(&scratch, &haikuAct,
				sizeof(haikuAct));
			if (userAct == 0)
				return -LX_EFAULT;
		}
	}

	MutexLocker locker(data->lock);
	renguin_sigaction old = data->sigactions[signal];

	if (haikuSignal != 0) {
		status_t status = _user_sigaction(haikuSignal,
			(const struct sigaction*)(addr_t)userAct,
			(struct sigaction*)(addr_t)userOld);
		if (status != B_OK)
			return renguin_encode(status);

		struct sigaction haikuOld;
		if (user_memcpy(&haikuOld, (void*)(addr_t)userOld, sizeof(haikuOld))
				!= B_OK) {
			return -LX_EFAULT;
		}
		// Haiku's view wins: SA_RESETHAND may have reset the handler, and
		// actions inherited across exec are only known to Haiku.
		if ((uint64)(addr_t)haikuOld.sa_handler != old.handler) {
			old.handler = (uint64)(addr_t)haikuOld.sa_handler;
			old.flags = flags_from_haiku(haikuOld.sa_flags);
			old.restorer = 0;
			old.mask = mask_from_haiku(haikuOld.sa_mask);
		}
	}

	if (action != 0) {
		act.mask &= ~(linux_bit(LX_SIGKILL) | linux_bit(LX_SIGSTOP));
		data->sigactions[signal] = act;
	}
	locker.Unlock();

	if (oldAction != 0
		&& renguin_copy_out(oldAction, &old, sizeof(old)) != B_OK) {
		return -LX_EFAULT;
	}
	return 0;
}


/*!	Sets the calling thread's mask of blocked signals, Linux style. The
	part Haiku can represent goes to Haiku; the rest is kept in
	renguin_thread::unmapped_blocked.
*/
static int64
set_signal_mask(int how, const uint64* linuxSet, uint64* _oldLinuxSet)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userSet = 0;
	if (linuxSet != NULL) {
		sigset_t set = mask_to_haiku(*linuxSet);
		userSet = renguin_scratch_put(&scratch, &set, sizeof(set));
		if (userSet == 0)
			return -LX_EFAULT;
	}
	uint64 userOld = renguin_scratch_alloc(&scratch, sizeof(sigset_t));
	if (userOld == 0)
		return -LX_EFAULT;

	// Haiku's SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK are Linux's plus one.
	status_t status = _user_set_signal_mask(how + 1,
		(const sigset_t*)(addr_t)userSet, (sigset_t*)(addr_t)userOld);
	if (status != B_OK)
		return renguin_encode(status);

	sigset_t old;
	if (user_memcpy(&old, (void*)(addr_t)userOld, sizeof(old)) != B_OK)
		return -LX_EFAULT;

	renguin_thread* data = renguin_current_thread_data(false);
	uint64 unmappedOld = data != NULL ? data->unmapped_blocked : 0;
	if (_oldLinuxSet != NULL)
		*_oldLinuxSet = mask_from_haiku(old) | unmappedOld;

	if (linuxSet != NULL) {
		uint64 unmapped = *linuxSet & unmapped_signals();
		uint64 newUnmapped = unmappedOld;
		switch (how) {
			case LX_SIG_BLOCK:		newUnmapped |= unmapped; break;
			case LX_SIG_UNBLOCK:	newUnmapped &= ~unmapped; break;
			case LX_SIG_SETMASK:	newUnmapped = unmapped; break;
		}
		if (newUnmapped != unmappedOld) {
			data = renguin_current_thread_data(true);
			if (data != NULL)
				data->unmapped_blocked = newUnmapped;
		}
	}
	return 0;
}


int64
renguin_sys_rt_sigprocmask(uint64 how, uint64 set, uint64 oldSet,
	uint64 sigsetSize)
{
	if (sigsetSize != sizeof(uint64))
		return -LX_EINVAL;

	uint64 linuxSet;
	if (set != 0) {
		if (how > LX_SIG_SETMASK)
			return -LX_EINVAL;
		if (renguin_copy_in(&linuxSet, set, sizeof(linuxSet)) != B_OK)
			return -LX_EFAULT;
	}

	uint64 old;
	int64 result = set_signal_mask(set != 0 ? (int)how : LX_SIG_BLOCK,
		set != 0 ? &linuxSet : NULL, &old);
	if (result != 0)
		return result;

	if (oldSet != 0 && renguin_copy_out(oldSet, &old, sizeof(old)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_rt_sigpending(uint64 set, uint64 sigsetSize)
{
	if (sigsetSize > sizeof(uint64))
		return -LX_EINVAL;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userSet = renguin_scratch_alloc(&scratch, sizeof(sigset_t));
	if (userSet == 0)
		return -LX_EFAULT;
	status_t status = _user_sigpending((sigset_t*)(addr_t)userSet);
	if (status != B_OK)
		return renguin_encode(status);

	sigset_t pending;
	if (user_memcpy(&pending, (void*)(addr_t)userSet, sizeof(pending)) != B_OK)
		return -LX_EFAULT;
	uint64 linuxPending = mask_from_haiku(pending);
	if (renguin_copy_out(set, &linuxPending, sigsetSize) != B_OK)
		return -LX_EFAULT;
	return 0;
}


static int64
suspend(sigset_t mask)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userMask = renguin_scratch_put(&scratch, &mask, sizeof(mask));
	if (userMask == 0)
		return -LX_EFAULT;
	status_t status = _user_sigsuspend((const sigset_t*)(addr_t)userMask);
	// Returns only when a handler has run (or the thread is dying).
	return status == B_OK ? -LX_EINTR : renguin_encode(status);
}


int64
renguin_sys_rt_sigsuspend(uint64 mask, uint64 sigsetSize)
{
	if (sigsetSize != sizeof(uint64))
		return -LX_EINVAL;
	uint64 linuxMask;
	if (renguin_copy_in(&linuxMask, mask, sizeof(linuxMask)) != B_OK)
		return -LX_EFAULT;
	return suspend(mask_to_haiku(linuxMask));
}


int64
renguin_sys_pause()
{
	uint64 current;
	int64 result = set_signal_mask(LX_SIG_BLOCK, NULL, &current);
	if (result != 0)
		return result;
	return suspend(mask_to_haiku(current));
}


int64
renguin_sys_rt_sigtimedwait(uint64 set, uint64 info, uint64 timeout,
	uint64 sigsetSize)
{
	if (sigsetSize != sizeof(uint64))
		return -LX_EINVAL;
	uint64 linuxSet;
	if (renguin_copy_in(&linuxSet, set, sizeof(linuxSet)) != B_OK)
		return -LX_EFAULT;

	uint32 flags = 0;
	bigtime_t waitTimeout = 0;
	if (timeout != 0) {
		int64 timespec[2];
		if (renguin_copy_in(timespec, timeout, sizeof(timespec)) != B_OK)
			return -LX_EFAULT;
		if (timespec[0] < 0 || timespec[1] < 0 || timespec[1] >= 1000000000)
			return -LX_EINVAL;
		if (timespec[0] < B_INFINITE_TIMEOUT / 1000000 - 1) {
			flags = B_RELATIVE_TIMEOUT;
			waitTimeout = timespec[0] * 1000000
				+ (timespec[1] + 999) / 1000;
		}
	}

	sigset_t haikuSet = mask_to_haiku(linuxSet);
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userSet = renguin_scratch_put(&scratch, &haikuSet, sizeof(haikuSet));
	uint64 userInfo = renguin_scratch_alloc(&scratch, sizeof(siginfo_t));
	if (userSet == 0 || userInfo == 0)
		return -LX_EFAULT;

	status_t status = _user_sigwait((const sigset_t*)(addr_t)userSet,
		(siginfo_t*)(addr_t)userInfo, flags, waitTimeout);
	if (status == B_WOULD_BLOCK || status == B_TIMED_OUT)
		return -LX_EAGAIN;
	if (status == B_INTERRUPTED) {
		// Haiku restarts this one even after a handler; Linux fails with
		// EINTR then (it continues, with the time left, only when no handler
		// ran). Haiku has already saved the deadline.
		renguin_set_restart(RENGUIN_RESTART_NOHAND, NULL);
		return -LX_EINTR;
	}
	if (status != B_OK)
		return renguin_encode(status);

	siginfo_t haikuInfo;
	if (user_memcpy(&haikuInfo, (void*)(addr_t)userInfo, sizeof(haikuInfo))
			!= B_OK) {
		return -LX_EFAULT;
	}
	int linuxSignal = renguin_signal_from_haiku(haikuInfo.si_signo);
	if (info != 0) {
		linux_siginfo linuxInfo;
		siginfo_from_haiku(haikuInfo, linuxSignal, &linuxInfo);
		if (renguin_copy_out(info, &linuxInfo, sizeof(linuxInfo)) != B_OK)
			return -LX_EFAULT;
	}
	return linuxSignal;
}


/*!	rt_sigqueueinfo/rt_tgsigqueueinfo: queue a signal with the caller's
	si_code and value. As Linux's do_rt_sigqueueinfo(), only a process
	signalling itself may claim a non-negative code or SI_TKILL.
*/
static int64
queue_signal(int32 tgid, int32 tid, uint64 signal, uint64 info)
{
	if (signal < 1 || signal > LX_NSIG)
		return -LX_EINVAL;
	linux_siginfo linuxInfo;
	if (renguin_copy_in(&linuxInfo, info, sizeof(linuxInfo)) != B_OK)
		return -LX_EFAULT;
	Thread* current = thread_get_current_thread();
	if ((linuxInfo.si_code >= 0 || linuxInfo.si_code == LX_SI_TKILL)
		&& tgid != current->team->id) {
		return -LX_EPERM;
	}

	int haikuSignal = renguin_signal_to_haiku((int)signal);
	if (haikuSignal == 0)
		return -LX_EINVAL;

	// Negative codes travel unchanged (signal.cpp hands them back to Linux
	// as they are); non-negative ones only come from the process itself and
	// are delivered as SI_USER.
	Signal haikuInfo(haikuSignal,
		linuxInfo.si_code < 0 ? linuxInfo.si_code : SI_USER, B_OK,
		current->team->id);
	union sigval value;
	value.sival_ptr = (void*)(addr_t)linuxInfo.rt.value;
	haikuInfo.SetUserValue(value);

	uint32 flags = B_CHECK_PERMISSION | SIGNAL_FLAG_QUEUING_REQUIRED;
	status_t status = tid >= 0
		? send_signal_to_thread_id(tid, haikuInfo, flags)
		: send_signal_to_team_id(tgid, haikuInfo, flags);
	if (status == B_BAD_TEAM_ID || status == B_BAD_THREAD_ID)
		return -LX_ESRCH;
	return renguin_encode(status);
}


int64
renguin_sys_rt_sigqueueinfo(int64 pid, uint64 signal, uint64 info)
{
	if (pid <= 0)
		return -LX_ESRCH;
	return queue_signal((int32)pid, -1, signal, info);
}


int64
renguin_sys_rt_tgsigqueueinfo(int64 tgid, int64 tid, uint64 signal,
	uint64 info)
{
	if (tgid <= 0 || tid <= 0)
		return -LX_EINVAL;
	Thread* thread = Thread::Get((thread_id)tid);
	if (thread == NULL)
		return -LX_ESRCH;
	bool sameGroup = thread->team->id == (team_id)tgid;
	thread->ReleaseReference();
	if (!sameGroup)
		return -LX_ESRCH;
	return queue_signal((int32)tgid, (int32)tid, signal, info);
}


// #pragma mark - temporary masks


/*!	Installs \a linuxMask for the duration of a wait (ppoll, pselect6), the
	way Haiku's sigsuspend_internal() does: returns the mask to go back to.
	Pass it to renguin_pop_signal_mask() when the wait is over.
*/
sigset_t
renguin_push_signal_mask(uint64 linuxMask)
{
	Thread* thread = thread_get_current_thread();
	sigset_t oldMask;
	{
		InterruptsSpinLocker locker(thread->team->signal_lock);
		// After a restart the mask in effect is still the temporary one.
		oldMask = thread->sigsuspend_original_unblocked_mask != 0
			? ~thread->sigsuspend_original_unblocked_mask
			: thread->sig_block_mask;
		thread->sigsuspend_original_unblocked_mask = 0;
	}
	sigset_t mask = mask_to_haiku(linuxMask);
	sigprocmask(SIG_SETMASK, &mask, NULL);
	return oldMask;
}


/*!	Ends a temporary mask. When the wait was interrupted, the temporary
	mask stays until the signal is handled: handle_signals() puts the old
	one in the handler's frame (so it returns when the handler does), or
	restarts the call if no handler ran.
*/
void
renguin_pop_signal_mask(sigset_t oldMask, bool interrupted)
{
	if (!interrupted) {
		sigprocmask(SIG_SETMASK, &oldMask, NULL);
		return;
	}
	Thread* thread = thread_get_current_thread();
	InterruptsSpinLocker locker(thread->team->signal_lock);
	thread->sigsuspend_original_unblocked_mask = ~oldMask;
		// never 0: KILL and STOP are never blocked
}


// #pragma mark - syscall restart


/*!	For calls that return partial progress after a signal interrupted a
	later step: Haiku's _user_read()/_user_write() asked for a restart when
	they were interrupted, but the call is returning what it did, which
	must not be thrown away by re-running it.
*/
void
renguin_cancel_restart()
{
	Thread* thread = thread_get_current_thread();
	if ((thread->flags & THREAD_FLAGS_RESTART_SYSCALL) != 0)
		atomic_and(&thread->flags, ~THREAD_FLAGS_RESTART_SYSCALL);
}


void
renguin_set_restart(int policy, const bigtime_t* deadline)
{
	Thread* thread = thread_get_current_thread();
	renguin_thread* data = renguin_current_thread_data(true);
	if (data == NULL) {
		if (policy != RENGUIN_RESTART_SYS)
			return;		// can't record the policy: just fail with EINTR
	} else
		data->restart_policy = (uint8)policy;

	if (deadline != NULL) {
		memcpy(thread->syscall_restart.parameters, deadline,
			sizeof(*deadline));
	}
	atomic_or(&thread->flags, THREAD_FLAGS_RESTART_SYSCALL);
}


bool
renguin_restarted_deadline(bigtime_t* _deadline)
{
	Thread* thread = thread_get_current_thread();
	if ((thread->flags & THREAD_FLAGS_SYSCALL_RESTARTED) == 0)
		return false;
	memcpy(_deadline, thread->syscall_restart.parameters, sizeof(*_deadline));
	return true;
}


// #pragma mark - FPU state


// Kernel globals describing the FPU save format (arch/x86/arch_cpu.cpp,
// arch/x86/64/thread.cpp). x86-64 Haiku saves the user FPU state into
// Thread::arch_info.user_fpu_state with FXSAVE, XSAVE or XSAVEC (patched in
// at boot); gInitialState holds the state a new thread starts with.
extern uint64 gFPUSaveLength;
extern bool gHasXsave;
extern uint64 gXsaveMask;
extern arch_thread gInitialState;

static const size_t kXSaveHeaderOffset = kFXSaveSize;
static const size_t kXSaveHeaderSize = 64;
static const uint64 kXCompBVCompacted = (uint64)1 << 63;


//! Bytes of Thread::arch_info.user_fpu_state in use: the legacy area, and
//! with XSAVE the header and the extended components.
static size_t
fpu_state_size()
{
	size_t size = gHasXsave ? (size_t)gFPUSaveLength : kFXSaveSize;
	size = max_c(size, kFXSaveSize + (gHasXsave ? kXSaveHeaderSize : 0));
	return min_c(size, sizeof(gInitialState.user_fpu_state));
}


static inline uint64
read64(const uint8* p)
{
	uint64 value;
	memcpy(&value, p, sizeof(value));
	return value;
}


static inline void
write64(uint8* p, uint64 value)
{
	memcpy(p, &value, sizeof(value));
}


//! Gives the thread the initial FPU state (what Linux does for a signal
//! handler and for a new program). The way to userland loads it.
void
renguin_reset_fpu_state(Thread* thread)
{
	memcpy(thread->arch_info.user_fpu_state, gInitialState.user_fpu_state,
		fpu_state_size());
}


/*!	Prepares the first bytes of the FPU state for a signal frame: the legacy
	FXSAVE area, plus the XSAVE header. XSAVEC doesn't write components that
	are in their initial state (it only clears their XSTATE_BV bit), so
	those parts of the legacy area are filled in from the initial state to
	show what the registers really hold.
*/
static void
prepare_fpu_frame(const uint8* state, uint8* legacy)
{
	memcpy(legacy, state, kFXSaveSize
		+ (gHasXsave ? kXSaveHeaderSize : 0));
	if (gHasXsave) {
		const uint8* initial = gInitialState.user_fpu_state;
		uint64 xstateBV = read64(legacy + kXSaveHeaderOffset);
		if ((xstateBV & 1) == 0) {
			// x87: control/status/tag words, opcode, pointers; ST0-7
			memcpy(legacy, initial, 24);
			memcpy(legacy + 32, initial + 32, 128);
		}
		if ((xstateBV & 2) == 0)
			memcpy(legacy + 160, initial + 160, 256);	// XMM0-15
		write64(legacy + kXSaveHeaderOffset, xstateBV | 3);
	}
	// Linux describes extended state in the software-reserved bytes
	// (fpx_sw_bytes); zero means "plain FXSAVE", so libcs and debuggers don't
	// interpret the rest. rt_sigreturn reads it back as it was written.
	memset(legacy + kFXSaveSoftwareOffset, 0,
		kFXSaveSize - kFXSaveSoftwareOffset);
}


/*!	Makes FPU state read back from a signal frame safe to load with
	FXRSTOR/XRSTOR, which fault in the kernel on bad values: MXCSR bits the
	CPU doesn't have, and an invalid XSAVE header. The header's format word
	(XCOMP_BV) is taken from the thread's own current state, and only
	components the kernel enabled may be marked present.
*/
static void
sanitize_fpu_frame(uint8* legacy, const uint8* current)
{
	uint32 mxcsrMask;
	memcpy(&mxcsrMask, current + 28, sizeof(mxcsrMask));
	if (mxcsrMask == 0)
		mxcsrMask = 0xffbf;		// the architectural default
	uint32 mxcsr;
	memcpy(&mxcsr, legacy + 24, sizeof(mxcsr));
	mxcsr &= mxcsrMask;
	memcpy(legacy + 24, &mxcsr, sizeof(mxcsr));
	memcpy(legacy + 28, &mxcsrMask, sizeof(mxcsrMask));
	memcpy(legacy + kFXSaveSoftwareOffset, current + kFXSaveSoftwareOffset,
		kFXSaveSize - kFXSaveSoftwareOffset);

	if (gHasXsave) {
		uint64 xcompBV = read64(current + kXSaveHeaderOffset + 8);
		uint64 xstateBV = (read64(legacy + kXSaveHeaderOffset) | 3)
			& gXsaveMask;
		if ((xcompBV & kXCompBVCompacted) != 0)
			xstateBV &= xcompBV;
		memset(legacy + kXSaveHeaderOffset, 0, kXSaveHeaderSize);
		write64(legacy + kXSaveHeaderOffset, xstateBV);
		write64(legacy + kXSaveHeaderOffset + 8, xcompBV);
	}
}


// #pragma mark - delivery


/*!	The signal frame can't be built or read back: raise SIGSEGV, as Linux's
	force_sigsegv()/signal_fault() do. The thread is sent to address 0, so
	it faults the moment it is back in userland; the kernel turns that fault
	into a SIGSEGV that can't be blocked or ignored
	(foreign_personality_fault_signal()). If SIGSEGV itself could not be
	delivered (\a failingSignal, a Haiku number, or 0), its handler is
	dropped first, so the fault ends the program.
*/
static void
force_sigsegv(Thread* thread, iframe* frame, int failingSignal)
{
	if (failingSignal == SIGSEGV) {
		TeamLocker teamLocker(thread->team);
		struct sigaction& action = thread->team->SignalActionFor(SIGSEGV);
		action.sa_handler = SIG_DFL;
		action.sa_flags = 0;
	}

	// Remember where the thread was: a SIGSEGV handler gets that context
	// (and si_code SI_KERNEL), as with Linux's force_sigsegv(), not the
	// fetch from address 0.
	renguin_thread* data = renguin_current_thread_data(true);
	if (data != NULL) {
		data->forced_sigsegv = true;
		data->forced_sigsegv_ip = frame->ip;
	}
	frame->ip = 0;
}


/*!	The frame for a signal could not be built: undo the handler's block
	mask, which Haiku set before calling us (Linux sets it only once the
	frame is in place). The coming SIGSEGV updates the pending-signals flag.
*/
static void
frame_setup_failed(Thread* thread, iframe* frame, int failingSignal,
	struct signal_frame_data* data)
{
	{
		InterruptsSpinLocker locker(thread->team->signal_lock);
		thread->sig_block_mask = data->context.uc_sigmask;
	}
	force_sigsegv(thread, frame, failingSignal);
}


/*!	Kernel hook (foreign_personality.h): builds a Linux rt_sigframe and
	points the thread at the handler. Called by Haiku's setup_signal_frame()
	once the handler's block mask is in place; \a data holds the signal
	info, the mask to restore (data->context.uc_sigmask) and the syscall
	restart flags that were in effect (data->thread_flags).

	Layout, from the top of the chosen stack down (Linux's get_sigframe()):
	the 128-byte red zone (skipped when switching to the alternate stack),
	the FPU state (64-byte aligned: the FXSAVE area, then, with XSAVE, the
	header and extended state), then the frame, placed so that the handler
	starts with rsp % 16 == 8, as after a call. The handler starts with the
	initial FPU state, as on Linux.
*/
status_t
renguin_setup_signal_frame(Thread* thread, struct sigaction* action,
	struct signal_frame_data* data)
{
	iframe* frame = x86_get_user_iframe();
	renguin_team* teamData = (renguin_team*)thread->team->personality_data;
	if (frame == NULL || teamData == NULL)
		return B_BAD_VALUE;

	// A restart, if any, is decided below; a flag left from an earlier
	// restart must not reach the handler's first system call.
	atomic_and(&thread->flags, ~THREAD_FLAGS_SYSCALL_RESTARTED);

	int haikuSignal = data->info.si_signo;
	int linuxSignal = renguin_signal_from_haiku(haikuSignal);
	if (linuxSignal == 0) {
		frame_setup_failed(thread, frame, haikuSignal, data);
		return B_BAD_VALUE;
	}

	renguin_sigaction linuxAction;
	{
		MutexLocker locker(teamData->lock);
		linuxAction = teamData->sigactions[linuxSignal];
	}
	uint64 linuxFlags = flags_from_haiku(action->sa_flags)
		| (linuxAction.flags & LX_SA_RESTORER);
	if ((linuxFlags & LX_SA_RESTORER) == 0 || linuxAction.restorer == 0) {
		// x86-64 Linux needs the trampoline from libc; without one the
		// kernel can't build a frame either.
		frame_setup_failed(thread, frame, haikuSignal, data);
		return B_BAD_VALUE;
	}

	renguin_thread* threadData = (renguin_thread*)thread->personality_data;

	// The SIGSEGV that force_sigsegv() arranged: report the real context.
	bool forced = false;
	addr_t forcedIP = 0;
	if (threadData != NULL && threadData->forced_sigsegv) {
		threadData->forced_sigsegv = false;
		if (linuxSignal == LX_SIGSEGV && frame->ip == 0) {
			forced = true;
			forcedIP = threadData->forced_sigsegv_ip;
		}
	}

	// The frame, built here and copied out in one go. The context to save:
	// if the interrupted system call is to be restarted, the saved context
	// re-issues it (Linux does this before building the frame, in
	// handle_signal()).
	linux_rt_sigframe sigframe;
	memset(&sigframe, 0, sizeof(sigframe));
	sigframe.pretcode = linuxAction.restorer;
	linux_ucontext& uc = sigframe.uc;
	uc.uc_flags = LX_UC_SIGCONTEXT_SS | LX_UC_STRICT_RESTORE_SS;
	get_signal_stack(thread, frame->user_sp, &uc.uc_stack);

	linux_sigcontext& context = uc.uc_mcontext;
	context.r8 = frame->r8;
	context.r9 = frame->r9;
	context.r10 = frame->r10;
	context.r11 = frame->r11;
	context.r12 = frame->r12;
	context.r13 = frame->r13;
	context.r14 = frame->r14;
	context.r15 = frame->r15;
	context.rdi = frame->di;
	context.rsi = frame->si;
	context.rbp = frame->bp;
	context.rbx = frame->bx;
	context.rdx = frame->dx;
	context.rax = frame->ax;
	context.rcx = frame->cx;
	context.rsp = frame->user_sp;
	context.rip = forced ? forcedIP : frame->ip;
	context.eflags = frame->flags;
	context.cs = (uint16)frame->cs;
	context.ss = (uint16)frame->ss;
	context.err = frame->error_code;
	context.trapno = frame->vector;
	if ((linuxSignal == LX_SIGSEGV || linuxSignal == LX_SIGBUS) && !forced)
		context.cr2 = (uint64)(addr_t)data->info.si_addr;
	if ((data->thread_flags & THREAD_FLAGS_RESTART_SYSCALL) != 0
		&& (threadData == NULL
			|| threadData->restart_policy != RENGUIN_RESTART_NOHAND)) {
		context.rax = frame->orig_rax;
		context.rip = frame->ip - 2;		// the syscall instruction
	}
	if (threadData != NULL)
		threadData->restart_policy = RENGUIN_RESTART_SYS;

	uint64 oldMask = mask_from_haiku(data->context.uc_sigmask)
		| (threadData != NULL ? threadData->unmapped_blocked : 0);
	uc.uc_sigmask = oldMask;
	context.oldmask = oldMask;
	siginfo_from_haiku(data->info, linuxSignal, &sigframe.info);
	if (forced) {
		sigframe.info.si_code = LX_SI_KERNEL;
		sigframe.info.fault.addr = 0;
	}

	// Choose the stack.
	bool onSignalStack = on_signal_stack(thread, frame->user_sp);
	bool switchedStack = false;
	addr_t sp = frame->user_sp - kRedZoneSize;
	if ((linuxFlags & LX_SA_ONSTACK) != 0 && thread->signal_stack_enabled
		&& !onSignalStack) {
		sp = thread->signal_stack_base + thread->signal_stack_size;
		onSignalStack = switchedStack = true;
	}

	size_t fpuSize = fpu_state_size();
	addr_t fpState = ROUNDDOWN(sp - fpuSize, 64);
	context.fpstate = fpState;
	addr_t frameAddress = ROUNDDOWN(fpState - sizeof(linux_rt_sigframe), 16)
		- 8;

	// Running out of the alternate stack is an error, as on Linux, rather
	// than a silent overwrite of whatever lies below it.
	if (onSignalStack && (frameAddress <= (addr_t)uc.uc_stack.ss_sp
			|| frameAddress - (addr_t)uc.uc_stack.ss_sp
				> uc.uc_stack.ss_size)) {
		frame_setup_failed(thread, frame, haikuSignal, data);
		return B_BAD_ADDRESS;
	}

	uint8 legacy[kFXSaveSize + kXSaveHeaderSize];
	const uint8* state = thread->arch_info.user_fpu_state;
	prepare_fpu_frame(state, legacy);
	size_t legacySize = kFXSaveSize + (gHasXsave ? kXSaveHeaderSize : 0);

	status_t status = renguin_copy_out(fpState, legacy, legacySize);
	if (status == B_OK && fpuSize > legacySize) {
		status = renguin_copy_out(fpState + legacySize, state + legacySize,
			fpuSize - legacySize);
	}
	if (status == B_OK)
		status = renguin_copy_out(frameAddress, &sigframe, sizeof(sigframe));
	if (status != B_OK) {
		// No usable stack (overflow, or a bad alternate stack).
		frame_setup_failed(thread, frame, haikuSignal, data);
		return status;
	}

	// SS_AUTODISARM: the alternate stack is off while the handler runs;
	// rt_sigreturn puts back what uc_stack says.
	if (switchedStack && threadData != NULL
		&& threadData->altstack_autodisarm) {
		thread->signal_stack_enabled = false;
	}

	// Enter the handler, with the initial FPU state.
	renguin_reset_fpu_state(thread);
	frame->fpu = thread->arch_info.user_fpu_state;
	frame->user_sp = frameAddress;
	frame->ip = (addr_t)action->sa_handler;
	frame->di = linuxSignal;
	frame->si = frameAddress + offsetof(linux_rt_sigframe, info);
	frame->dx = frameAddress + offsetof(linux_rt_sigframe, uc);
	frame->ax = 0;
	frame->flags &= ~(uint64)(X86_EFLAGS_TRAP | X86_EFLAGS_DIRECTION
		| X86_EFLAGS_RESUME);
	return B_OK;
}


/*!	rt_sigreturn: called by the SA_RESTORER trampoline when a handler
	returns. The handler's "ret" popped pretcode, so the frame starts 8 bytes
	below the stack pointer. Returns the restored rax.
*/
int64
renguin_sys_rt_sigreturn()
{
	Thread* thread = thread_get_current_thread();
	iframe* frame = x86_get_user_iframe();
	if (frame == NULL)
		return -LX_EINVAL;

	addr_t frameAddress = frame->user_sp - 8;
	linux_ucontext uc;
	if (renguin_copy_in(&uc, frameAddress + offsetof(linux_rt_sigframe, uc),
			sizeof(uc)) != B_OK
		|| !IS_USER_ADDRESS((addr_t)uc.uc_mcontext.rip)) {
		force_sigsegv(thread, frame, 0);
		return frame->ax;
	}

	// The FPU state, if any. It goes straight into the thread's save area
	// (the extended part) or through sanitize_fpu_frame() (the part the CPU
	// checks), which also refuses what would fault in the kernel.
	size_t fpuSize = fpu_state_size();
	size_t legacySize = kFXSaveSize + (gHasXsave ? kXSaveHeaderSize : 0);
	uint8 legacy[kFXSaveSize + kXSaveHeaderSize];
	uint8* state = thread->arch_info.user_fpu_state;
	addr_t fpState = (addr_t)uc.uc_mcontext.fpstate;
	if (fpState != 0) {
		// The frame says "plain FXSAVE" to userland. The rest is only ours
		// to read back if fpstate still points where the hook put it (just
		// above the frame); otherwise the program supplied its own 512-byte
		// FXSAVE image and, as on Linux, the extended state starts over.
		addr_t frameEnd = frameAddress + sizeof(linux_rt_sigframe);
		bool ours = (fpState & 63) == 0 && fpState >= frameEnd + 8
			&& fpState < frameEnd + 24;
		size_t readSize = ours ? legacySize : kFXSaveSize;
		if (renguin_copy_in(legacy, fpState, readSize) != B_OK
			|| (ours && fpuSize > legacySize
				&& renguin_copy_in(state + legacySize, fpState + legacySize,
					fpuSize - legacySize) != B_OK)) {
			force_sigsegv(thread, frame, 0);
			return frame->ax;
		}
		if (!ours && gHasXsave) {
			// only x87 and SSE present; everything else to initial state
			memset(legacy + kXSaveHeaderOffset, 0, kXSaveHeaderSize);
			write64(legacy + kXSaveHeaderOffset, 3);
		}
	}

	// Signal mask (the scratch space lies below the frame, which is still
	// on the stack).
	if (set_signal_mask(LX_SIG_SETMASK, &uc.uc_sigmask, NULL) != 0) {
		force_sigsegv(thread, frame, 0);
		return frame->ax;
	}

	// Alternate stack: put back what was in effect, unless the handler is
	// still running on it (Linux's restore_altstack() fails the same way).
	set_signal_stack(thread, frame->user_sp, uc.uc_stack);

	const linux_sigcontext& context = uc.uc_mcontext;
	frame->r8 = context.r8;
	frame->r9 = context.r9;
	frame->r10 = context.r10;
	frame->r11 = context.r11;
	frame->r12 = context.r12;
	frame->r13 = context.r13;
	frame->r14 = context.r14;
	frame->r15 = context.r15;
	frame->di = context.rdi;
	frame->si = context.rsi;
	frame->bp = context.rbp;
	frame->bx = context.rbx;
	frame->dx = context.rdx;
	frame->cx = context.rcx;
	frame->user_sp = context.rsp;
	frame->ip = context.rip;
	frame->flags = (frame->flags & ~(uint64)X86_EFLAGS_USER_FLAGS)
		| (context.eflags & X86_EFLAGS_USER_FLAGS);
	frame->orig_rax = (uint64)-1;
		// as Linux: this is not a system call that could be restarted

	if (fpState != 0) {
		sanitize_fpu_frame(legacy, state);
		memcpy(state, legacy, legacySize);
	} else {
		// no FPU state in the frame: Linux starts over from the initial one
		renguin_reset_fpu_state(thread);
	}
	frame->fpu = state;

	return (int64)context.rax;
}


// #pragma mark - interval timers


static int32
timer_for(uint64 which)
{
	switch (which) {
		case LX_ITIMER_REAL:	return USER_TIMER_REAL_TIME_ID;
		case LX_ITIMER_VIRTUAL:	return USER_TIMER_TEAM_USER_TIME_ID;
		case LX_ITIMER_PROF:	return USER_TIMER_TEAM_TOTAL_TIME_ID;
		default:				return -1;
	}
}


static void
timeval_from_bigtime(bigtime_t time, linux_timeval* value)
{
	value->tv_sec = time / 1000000;
	value->tv_usec = time % 1000000;
}


static void
itimerval_from_info(const user_timer_info& info, linux_itimerval* value)
{
	timeval_from_bigtime(info.interval, &value->it_interval);
	// B_INFINITE_TIMEOUT: not armed
	timeval_from_bigtime(info.remaining_time == B_INFINITE_TIMEOUT
		? 0 : info.remaining_time, &value->it_value);
}


static bool
bigtime_from_timeval(const linux_timeval& value, bigtime_t* _time)
{
	if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000)
		return false;
	if (value.tv_sec >= B_INFINITE_TIMEOUT / 1000000 - 1)
		*_time = B_INFINITE_TIMEOUT - 1;
	else
		*_time = value.tv_sec * 1000000 + value.tv_usec;
	return true;
}


static int64
set_itimer(uint64 which, const linux_itimerval& value,
	linux_itimerval* _oldValue)
{
	int32 timer = timer_for(which);
	if (timer < 0)
		return -LX_EINVAL;

	bigtime_t start, interval;
	if (!bigtime_from_timeval(value.it_value, &start)
		|| !bigtime_from_timeval(value.it_interval, &interval)) {
		return -LX_EINVAL;
	}
	if (start == 0)
		start = B_INFINITE_TIMEOUT;		// disarm

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userOld = renguin_scratch_alloc(&scratch, sizeof(user_timer_info));
	if (userOld == 0)
		return -LX_EFAULT;

	status_t status = _user_set_timer(timer, -1, start, interval,
		B_RELATIVE_TIMEOUT, (user_timer_info*)(addr_t)userOld);
	if (status != B_OK)
		return renguin_encode(status);

	if (_oldValue != NULL) {
		user_timer_info info;
		if (user_memcpy(&info, (void*)(addr_t)userOld, sizeof(info)) != B_OK)
			return -LX_EFAULT;
		itimerval_from_info(info, _oldValue);
	}
	return 0;
}


int64
renguin_sys_setitimer(uint64 which, uint64 newValue, uint64 oldValue)
{
	linux_itimerval value;
	memset(&value, 0, sizeof(value));		// NULL: disarm, as Linux
	if (newValue != 0
		&& renguin_copy_in(&value, newValue, sizeof(value)) != B_OK) {
		return -LX_EFAULT;
	}

	linux_itimerval old;
	int64 result = set_itimer(which, value, &old);
	if (result != 0)
		return result;
	if (oldValue != 0 && renguin_copy_out(oldValue, &old, sizeof(old)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


int64
renguin_sys_getitimer(uint64 which, uint64 value)
{
	int32 timer = timer_for(which);
	if (timer < 0)
		return -LX_EINVAL;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userInfo = renguin_scratch_alloc(&scratch, sizeof(user_timer_info));
	if (userInfo == 0)
		return -LX_EFAULT;
	status_t status = _user_get_timer(timer, -1,
		(user_timer_info*)(addr_t)userInfo);
	if (status != B_OK)
		return renguin_encode(status);

	user_timer_info info;
	if (user_memcpy(&info, (void*)(addr_t)userInfo, sizeof(info)) != B_OK)
		return -LX_EFAULT;
	linux_itimerval linuxValue;
	itimerval_from_info(info, &linuxValue);
	if (renguin_copy_out(value, &linuxValue, sizeof(linuxValue)) != B_OK)
		return -LX_EFAULT;
	return 0;
}


//! alarm: SIGALRM after \a seconds; returns the seconds left of the
//! previous alarm, rounded as Linux's alarm_setitimer() does.
int64
renguin_sys_alarm(uint64 seconds)
{
	linux_itimerval value;
	memset(&value, 0, sizeof(value));
	value.it_value.tv_sec = (int64)(uint32)seconds;

	linux_itimerval old;
	if (set_itimer(LX_ITIMER_REAL, value, &old) != 0)
		return 0;
	int64 remaining = old.it_value.tv_sec;
	if ((remaining == 0 && old.it_value.tv_usec != 0)
		|| old.it_value.tv_usec >= 500000) {
		remaining++;
	}
	return remaining;
}
