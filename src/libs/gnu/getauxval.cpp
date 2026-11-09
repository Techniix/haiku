/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 *
 * getauxval(): Haiku's runtime loader does not pass an auxiliary vector
 * the way Linux does, so the values are derived from the process and the
 * program image when first asked for:
 *
 *  - AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY: from the program's ELF header,
 *    which is mapped at the start of its text image.
 *  - AT_BASE: the runtime loader's load address.
 *  - AT_HWCAP: on x86, CPUID leaf 1 EDX, as on Linux. AT_HWCAP2: 0.
 *  - AT_RANDOM: 16 random bytes, generated once per process.
 *  - AT_EXECFN: the program's path. AT_PLATFORM: the architecture name.
 *  - AT_PAGESZ, AT_CLKTCK, AT_UID/EUID/GID/EGID, AT_SECURE,
 *    AT_MINSIGSTKSZ: from the system.
 *
 * Unknown types (and AT_SYSINFO_EHDR: there is no vDSO) return 0 with
 * errno ENOENT, as glibc does.
 */


#include <sys/auxv.h>

#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>

#include <errno_private.h>

#if defined(__x86_64__) || defined(__i386__)
#	include <cpuid.h>
#endif


#if defined(__x86_64__)
	typedef Elf64_Ehdr ElfHeader;
	typedef Elf64_Phdr ProgramHeader;
	static const char* kPlatform = "x86_64";
#elif defined(__i386__)
	typedef Elf32_Ehdr ElfHeader;
	typedef Elf32_Phdr ProgramHeader;
	static const char* kPlatform = "i686";
#elif defined(__aarch64__)
	typedef Elf64_Ehdr ElfHeader;
	typedef Elf64_Phdr ProgramHeader;
	static const char* kPlatform = "aarch64";
#elif defined(__riscv)
	typedef Elf64_Ehdr ElfHeader;
	typedef Elf64_Phdr ProgramHeader;
	static const char* kPlatform = "riscv64";
#else
#	error "getauxval: unknown architecture"
#endif


namespace {

struct AuxValues {
	unsigned long	phdr;
	unsigned long	phent;
	unsigned long	phnum;
	unsigned long	entry;
	unsigned long	base;
	unsigned long	hwcap;
	unsigned char	random[16];
	char			execfn[B_PATH_NAME_LENGTH];
};

}	// namespace


static AuxValues sValues;
static pthread_once_t sInitOnce = PTHREAD_ONCE_INIT;


static void
init_values()
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE) {
			strlcpy(sValues.execfn, info.name, sizeof(sValues.execfn));

			const ElfHeader* header = (const ElfHeader*)info.text;
			if (memcmp(header->e_ident, ELFMAG, SELFMAG) == 0) {
				const ProgramHeader* programHeaders
					= (const ProgramHeader*)((const char*)header
						+ header->e_phoff);
				// The load delta: where the first PT_LOAD segment ended
				// up, minus where the file says it goes (non-zero for PIE).
				unsigned long delta = 0;
				for (int i = 0; i < header->e_phnum; i++) {
					if (programHeaders[i].p_type == PT_LOAD) {
						delta = (unsigned long)info.text
							- programHeaders[i].p_vaddr;
						break;
					}
				}
				sValues.phdr = (unsigned long)programHeaders;
				sValues.phent = header->e_phentsize;
				sValues.phnum = header->e_phnum;
				sValues.entry = header->e_entry + delta;
			}
		} else if (strstr(info.name, "runtime_loader") != NULL)
			sValues.base = (unsigned long)info.text;
	}

#if defined(__x86_64__) || defined(__i386__)
	unsigned int eax, ebx, ecx, edx;
	if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
		sValues.hwcap = edx;
#endif

	if (getrandom(sValues.random, sizeof(sValues.random), 0)
			!= (ssize_t)sizeof(sValues.random)) {
		memset(sValues.random, 0, sizeof(sValues.random));
	}
}


unsigned long
getauxval(unsigned long type)
{
	pthread_once(&sInitOnce, &init_values);

	switch (type) {
		case AT_PHDR:
			if (sValues.phdr != 0)
				return sValues.phdr;
			break;
		case AT_PHENT:
			if (sValues.phent != 0)
				return sValues.phent;
			break;
		case AT_PHNUM:
			if (sValues.phnum != 0)
				return sValues.phnum;
			break;
		case AT_ENTRY:
			if (sValues.entry != 0)
				return sValues.entry;
			break;
		case AT_BASE:
			if (sValues.base != 0)
				return sValues.base;
			break;
		case AT_PAGESZ:
			return B_PAGE_SIZE;
		case AT_FLAGS:
			return 0;
		case AT_UID:
			return getuid();
		case AT_EUID:
			return geteuid();
		case AT_GID:
			return getgid();
		case AT_EGID:
			return getegid();
		case AT_PLATFORM:
			return (unsigned long)kPlatform;
		case AT_HWCAP:
			return sValues.hwcap;
		case AT_HWCAP2:
			return 0;
		case AT_CLKTCK:
			return sysconf(_SC_CLK_TCK);
		case AT_SECURE:
			return getuid() != geteuid() || getgid() != getegid();
		case AT_RANDOM:
			return (unsigned long)sValues.random;
		case AT_EXECFN:
			if (sValues.execfn[0] != '\0')
				return (unsigned long)sValues.execfn;
			break;
		case AT_MINSIGSTKSZ:
			return MINSIGSTKSZ;
	}

	__set_errno(ENOENT);
	return 0;
}
