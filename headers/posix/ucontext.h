/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _UCONTEXT_H
#define _UCONTEXT_H


/* ucontext_t and mcontext_t are defined in <signal.h>. */
#include <signal.h>
#include <sys/cdefs.h>


__BEGIN_DECLS

/* XSI user contexts (POSIX.1-2001; obsolescent, but widely used for
   coroutines). Implemented for x86_64. */
int		getcontext(ucontext_t* context);
int		setcontext(const ucontext_t* context);
void	makecontext(ucontext_t* context, void (*function)(void),
			int argumentCount, ...);
int		swapcontext(ucontext_t* oldContext, const ucontext_t* newContext);

__END_DECLS


#endif	/* _UCONTEXT_H */
