/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_FILE_SEALS_DEFS_H
#define _SYSTEM_FILE_SEALS_DEFS_H


/*!	Private fcntl() command, used by libgnu's memfd_create() only: makes the
	file open as the descriptor sealable, with the given initial seals (0 for
	MFD_ALLOW_SEALING, F_SEAL_SEAL otherwise, as on Linux; F_SEAL_EXEC is
	added for MFD_NOEXEC_SEAL). The kernel accepts it only for a regular file
	open for reading and writing that has no name left (link count 0), so it
	cannot be applied to a file other processes can still open.
*/
#define F_RENGA_SET_SEALABLE	0x40000000

#define F_SEAL_ALL_KNOWN \
	(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE \
		| F_SEAL_FUTURE_WRITE | F_SEAL_EXEC)


#endif	/* _SYSTEM_FILE_SEALS_DEFS_H */
