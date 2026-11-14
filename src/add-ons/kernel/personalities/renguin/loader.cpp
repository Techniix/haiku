/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux x86-64 executable loader and process entry.
 *
 * This plays the role of FreeBSD's imgact_elf.c with the Linux brand
 * (__elfN(imgact), __elfN(load_file), linux_exec_setregs()): it maps the
 * executable and, for dynamic programs, the Linux ELF interpreter (ld-linux or
 * ld-musl) from the Linux root, builds the Linux initial stack (argc, argv,
 * envp, auxv) and enters userland with Linux register state. Haiku's
 * runtime_loader is never mapped into a Linux team.
 *
 * Mapping mirrors Haiku's elf_load_user_image() (src/system/kernel/elf.cpp):
 * file-backed vm_map_file() areas for each PT_LOAD, zeroing of the partial
 * last page, an anonymous area for the rest of .bss, then the real
 * protection. Unlike Haiku's loader it does no relocation: ET_EXEC images go
 * at their link addresses and ET_DYN images (static-pie executables, the
 * interpreter) at a randomly chosen base; ld.so relocates itself.
 */


#include "renguin.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <AutoDeleter.h>

#include <arch/x86/arch_thread.h>
#include <arch/x86/64/descriptors.h>
#include <arch/x86/arch_cpu.h>
#include <arch/x86/64/iframe.h>
#include <image_defs.h>
#include <kernel.h>
#include <kimage.h>
#include <team.h>
#include <thread.h>
#include <user_thread_defs.h>
#include <util/Random.h>
#include <vfs.h>
#include <vm/vm.h>
#include <vm/vm_types.h>

#include <syscalls.h>
#include <vm_defs.h>

#include <fcntl.h>
#include <sys/stat.h>


//#define TRACE_RENGUIN_LOADER
#ifdef TRACE_RENGUIN_LOADER
#	define TRACE(x...) dprintf("renguin loader: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...) dprintf("renguin loader: " x)


static const uint64 kLinuxPageSize = 4096;
static const uint16 kMaxProgramHeaders = 128;
static const size_t kMaxInterpreterPath = 256;


// Interpreters that mark a program as Linux. Matched by prefix, so versioned
// names and the multiarch locations are covered.
static const char* const kLinuxInterpreterPrefixes[] = {
	"/lib64/ld-linux-x86-64.so",
	"/lib/ld-linux-x86-64.so",
	"/lib/x86_64-linux-gnu/ld-linux-x86-64.so",
	"/usr/lib64/ld-linux-x86-64.so",
	"/lib/ld-musl-x86_64.so",
	"/lib64/ld-linux.so",		// FreeBSD's "short" glibc form
};


struct linux_image_info {
	addr_t	bias;			// load bias (0 for ET_EXEC)
	addr_t	entry;			// biased entry point
	addr_t	phdr;			// biased address of the program headers, or 0
	uint16	phentsize;
	uint16	phnum;
	addr_t	low;			// lowest mapped address
	addr_t	high;			// end of the highest segment (p_vaddr + p_memsz)
	char	interpreter[kMaxInterpreterPath];	// PT_INTERP, or ""
};


static status_t
read_exactly(int fd, off_t pos, void* buffer, size_t size)
{
	ssize_t read = _kern_read(fd, pos, buffer, size);
	if (read < 0)
		return read;
	if ((size_t)read != size)
		return B_BAD_DATA;
	return B_OK;
}


static bool
is_linux_interpreter(const char* path)
{
	for (size_t i = 0; i < B_COUNT_OF(kLinuxInterpreterPrefixes); i++) {
		const char* prefix = kLinuxInterpreterPrefixes[i];
		if (strncmp(path, prefix, strlen(prefix)) == 0)
			return true;
	}
	return false;
}


static bool
is_valid_x86_64_elf(const renguin_ehdr* ehdr)
{
	return memcmp(ehdr->e_ident, "\177ELF", 4) == 0
		&& ehdr->e_ident[RENGUIN_EI_CLASS] == RENGUIN_ELFCLASS64
		&& ehdr->e_ident[RENGUIN_EI_DATA] == RENGUIN_ELFDATA2LSB
		&& ehdr->e_machine == RENGUIN_EM_X86_64
		&& (ehdr->e_type == RENGUIN_ET_EXEC || ehdr->e_type == RENGUIN_ET_DYN);
}


static status_t
read_program_headers(int fd, const renguin_ehdr* ehdr, renguin_phdr** _phdrs)
{
	if (ehdr->e_phnum == 0 || ehdr->e_phnum > kMaxProgramHeaders
		|| ehdr->e_phentsize < sizeof(renguin_phdr)) {
		return B_NOT_AN_EXECUTABLE;
	}

	size_t size = (size_t)ehdr->e_phnum * ehdr->e_phentsize;
	renguin_phdr* phdrs = (renguin_phdr*)malloc(size);
	if (phdrs == NULL)
		return B_NO_MEMORY;

	status_t status = read_exactly(fd, ehdr->e_phoff, phdrs, size);
	if (status != B_OK) {
		free(phdrs);
		return status;
	}

	*_phdrs = phdrs;
	return B_OK;
}


static inline const renguin_phdr*
program_header(const renguin_phdr* phdrs, const renguin_ehdr* ehdr, int index)
{
	return (const renguin_phdr*)((const uint8*)phdrs
		+ (size_t)index * ehdr->e_phentsize);
}


/*!	Looks for an NT_GNU_ABI_TAG note whose OS field is Linux. glibc's crt
	files add one to every program (abi-note.S), including static ones; this is
	how FreeBSD's linux_trans_osrel()/brand note matching recognizes them.
*/
static bool
has_linux_abi_note(int fd, const renguin_phdr* phdr)
{
	if (phdr->p_filesz < 32 || phdr->p_filesz > 4096)
		return false;

	uint8* notes = (uint8*)malloc(phdr->p_filesz);
	if (notes == NULL)
		return false;
	MemoryDeleter notesDeleter(notes);
	if (read_exactly(fd, phdr->p_offset, notes, phdr->p_filesz) != B_OK)
		return false;

	size_t offset = 0;
	while (offset + 12 <= phdr->p_filesz) {
		uint32 nameSize, descSize, type;
		memcpy(&nameSize, notes + offset, 4);
		memcpy(&descSize, notes + offset + 4, 4);
		memcpy(&type, notes + offset + 8, 4);
		size_t nameOffset = offset + 12;
		size_t descOffset = nameOffset + ((nameSize + 3) & ~3u);
		size_t next = descOffset + ((descSize + 3) & ~3u);
		if (next > phdr->p_filesz || next <= offset)
			break;

		if (type == RENGUIN_NT_GNU_ABI_TAG && nameSize == 4
			&& memcmp(notes + nameOffset, "GNU", 4) == 0 && descSize >= 16) {
			uint32 os;
			memcpy(&os, notes + descOffset, 4);
			return os == RENGUIN_GNU_ABI_LINUX;
		}
		offset = next;
	}
	return false;
}


/*!	Decides whether an executable is a Linux program. The order follows
	FreeBSD's __elfN(get_brandinfo)(): the interpreter path, then the OS/ABI
	byte, then the ABI note.

	Haiku's own executables carry EI_OSABI 0, no GNU ABI note, and Haiku's
	runtime_loader as interpreter (or none), so they are never claimed. A
	static musl program has none of the markers either; it needs the
	renguin-brand tool, like FreeBSD's brandelf.
*/
status_t
renguin_probe(const foreign_exec_args* args)
{
	if (args->header_size < sizeof(renguin_ehdr))
		return B_NOT_SUPPORTED;

	const renguin_ehdr* ehdr = (const renguin_ehdr*)args->header;
	if (!is_valid_x86_64_elf(ehdr) || ehdr->e_entry == 0)
		return B_NOT_SUPPORTED;

	renguin_phdr* phdrs;
	if (read_program_headers(args->fd, ehdr, &phdrs) != B_OK)
		return B_NOT_SUPPORTED;
	MemoryDeleter phdrsDeleter(phdrs);

	// 1. A known Linux interpreter. Any other interpreter (Haiku's
	// runtime_loader, or an unusual Linux one such as Nix's) falls through to
	// the other checks.
	for (int i = 0; i < ehdr->e_phnum; i++) {
		const renguin_phdr* phdr = program_header(phdrs, ehdr, i);
		if (phdr->p_type != RENGUIN_PT_INTERP)
			continue;
		char interpreter[kMaxInterpreterPath];
		if (phdr->p_filesz == 0 || phdr->p_filesz >= sizeof(interpreter)
			|| read_exactly(args->fd, phdr->p_offset, interpreter,
				phdr->p_filesz) != B_OK) {
			return B_NOT_SUPPORTED;
		}
		interpreter[phdr->p_filesz] = '\0';
		if (is_linux_interpreter(interpreter))
			return B_OK;
		break;
	}

	// 2. Branded Linux/GNU (glibc's static programs with IFUNCs, or
	// renguin-brand).
	if (ehdr->e_ident[RENGUIN_EI_OSABI] == RENGUIN_ELFOSABI_LINUX)
		return B_OK;

	// 3. A GNU ABI note saying Linux.
	bool isDynamic = false;
	for (int i = 0; i < ehdr->e_phnum; i++) {
		const renguin_phdr* phdr = program_header(phdrs, ehdr, i);
		if (phdr->p_type == RENGUIN_PT_NOTE && has_linux_abi_note(args->fd, phdr))
			return B_OK;
		if (phdr->p_type == RENGUIN_PT_INTERP
			|| phdr->p_type == RENGUIN_PT_DYNAMIC) {
			isDynamic = true;
		}
	}

	// 4. Fallback brand, like FreeBSD's kern.elf64.fallback_brand, but
	// scoped: a fully static, unmarked program (no PT_INTERP, no
	// PT_DYNAMIC) that a Linux program executes is taken to be Linux too;
	// static musl programs carry no marker. Haiku's own programs never
	// match: Haiku links executables with -shared (buildtools
	// gcc/config/i386/haiku64.h LINK_SPEC), so they have no PT_INTERP, but
	// they always have PT_DYNAMIC (every one needs libroot.so). Programs
	// started from Haiku are never affected.
	if (!isDynamic && ehdr->e_type == RENGUIN_ET_EXEC
		&& ehdr->e_ident[RENGUIN_EI_OSABI] == RENGUIN_ELFOSABI_SYSV
		&& renguin_is_linux_team(args->team)) {
		return B_OK;
	}

	return B_NOT_SUPPORTED;
}


static uint32
segment_protection(uint32 flags)
{
	uint32 protection = 0;
	if ((flags & RENGUIN_PF_R) != 0)
		protection |= B_READ_AREA;
	if ((flags & RENGUIN_PF_W) != 0)
		protection |= B_WRITE_AREA;
	if ((flags & RENGUIN_PF_X) != 0)
		protection |= B_EXECUTE_AREA;
	return protection;
}


/*!	Maps one PT_LOAD segment at \a bias + p_vaddr: the file-backed part, the
	zeroed tail of its last page, and an anonymous area for the remaining
	.bss. Every area gets the segment's protection at the end.
*/
static status_t
map_segment(team_id teamID, int fd, const renguin_phdr* phdr, addr_t bias,
	const char* baseName, int index)
{
	char name[B_OS_NAME_LENGTH];
	addr_t vaddr = phdr->p_vaddr + bias;
	addr_t pageOffset = vaddr % B_PAGE_SIZE;
	addr_t pageStart = vaddr - pageOffset;

	if (phdr->p_memsz < phdr->p_filesz
		|| (phdr->p_offset % B_PAGE_SIZE) != pageOffset) {
		ERROR("segment %d is misaligned\n", index);
		return B_NOT_AN_EXECUTABLE;
	}

	size_t fileUpperBound = 0;
	if (phdr->p_filesz > 0)
		fileUpperBound = ROUNDUP(pageOffset + phdr->p_filesz, B_PAGE_SIZE);
	size_t memUpperBound = ROUNDUP(pageOffset + phdr->p_memsz, B_PAGE_SIZE);

	const uint32 loadProtection = B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA;
	area_id fileArea = -1;
	area_id bssArea = -1;

	if (fileUpperBound > 0) {
		snprintf(name, sizeof(name), "%s_seg%d", baseName, index);
		void* address = (void*)pageStart;
		fileArea = vm_map_file(teamID, name, &address, B_EXACT_ADDRESS,
			fileUpperBound, loadProtection, REGION_PRIVATE_MAP, false, fd,
			phdr->p_offset - pageOffset);
		if (fileArea < 0) {
			ERROR("mapping segment %d at %#" B_PRIxADDR " failed: %s\n", index,
				pageStart, strerror(fileArea));
			return fileArea;
		}

		// Zero the part of the last file page past p_filesz (partial .bss).
		if (phdr->p_memsz > phdr->p_filesz) {
			addr_t zeroStart = vaddr + phdr->p_filesz;
			addr_t zeroEnd = ROUNDUP(zeroStart, B_PAGE_SIZE);
			if (zeroEnd > zeroStart
				&& user_memset((void*)zeroStart, 0, zeroEnd - zeroStart)
					!= B_OK) {
				return B_BAD_ADDRESS;
			}
		}
	}

	if (memUpperBound > fileUpperBound) {
		snprintf(name, sizeof(name), "%s_bss%d", baseName, index);
		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = (void*)(pageStart + fileUpperBound);
		virtualRestrictions.address_specification = B_EXACT_ADDRESS;
		physical_address_restrictions physicalRestrictions = {};
		void* address;
		bssArea = create_area_etc(teamID, name, memUpperBound - fileUpperBound,
			B_NO_LOCK, loadProtection, 0, 0, &virtualRestrictions,
			&physicalRestrictions, &address);
		if (bssArea < 0) {
			ERROR("allocating bss %d failed: %s\n", index, strerror(bssArea));
			return bssArea;
		}
	}

	uint32 protection = segment_protection(phdr->p_flags);
	if (fileArea >= 0) {
		status_t status = vm_set_area_protection(fileArea, protection, true);
		if (status != B_OK)
			return status;
	}
	if (bssArea >= 0) {
		status_t status = vm_set_area_protection(bssArea, protection, true);
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


/*!	Maps the ELF image open as kernel FD \a fd into \a team. Fills in \a info.
	\a isInterpreter selects the area names and forbids a nested PT_INTERP.
*/
static status_t
load_elf_image(Team* team, int fd, bool isInterpreter, linux_image_info* info)
{
	memset(info, 0, sizeof(*info));

	renguin_ehdr ehdr;
	status_t status = read_exactly(fd, 0, &ehdr, sizeof(ehdr));
	if (status != B_OK)
		return status;
	if (!is_valid_x86_64_elf(&ehdr)) {
		ERROR("not an x86-64 ELF image\n");
		return B_NOT_AN_EXECUTABLE;
	}

	renguin_phdr* phdrs;
	status = read_program_headers(fd, &ehdr, &phdrs);
	if (status != B_OK)
		return status;
	MemoryDeleter phdrsDeleter(phdrs);

	// Find the address span of the PT_LOAD segments.
	addr_t low = ~(addr_t)0;
	addr_t high = 0;
	for (int i = 0; i < ehdr.e_phnum; i++) {
		const renguin_phdr* phdr = program_header(phdrs, &ehdr, i);
		if (phdr->p_type == RENGUIN_PT_LOAD) {
			addr_t start = ROUNDDOWN(phdr->p_vaddr, B_PAGE_SIZE);
			addr_t end = phdr->p_vaddr + phdr->p_memsz;
			if (end < phdr->p_vaddr) {
				ERROR("segment %d wraps around\n", i);
				return B_NOT_AN_EXECUTABLE;
			}
			if (start < low)
				low = start;
			if (end > high)
				high = end;
		} else if (phdr->p_type == RENGUIN_PT_INTERP) {
			if (isInterpreter) {
				ERROR("interpreter has an interpreter\n");
				return B_NOT_AN_EXECUTABLE;
			}
			if (phdr->p_filesz == 0
				|| phdr->p_filesz >= sizeof(info->interpreter)) {
				return B_NOT_AN_EXECUTABLE;
			}
			status = read_exactly(fd, phdr->p_offset, info->interpreter,
				phdr->p_filesz);
			if (status != B_OK)
				return status;
			info->interpreter[phdr->p_filesz] = '\0';
		}
	}
	if (high == 0) {
		ERROR("no loadable segments\n");
		return B_NOT_AN_EXECUTABLE;
	}
	high = ROUNDUP(high, B_PAGE_SIZE);

	// ET_EXEC goes at its link address. ET_DYN goes wherever the address
	// space has room: reserve the whole span to find a base, then release
	// the reservation and map at exact addresses inside it. Nothing else runs
	// in the new team yet, so nobody can take the range in between.
	addr_t bias = 0;
	if (ehdr.e_type == RENGUIN_ET_DYN) {
		void* base = NULL;
		status = vm_reserve_address_range(team->id, &base,
			B_RANDOMIZED_ANY_ADDRESS, high - low, 0);
		if (status != B_OK) {
			ERROR("no room for a %#" B_PRIxADDR "-byte image\n", high - low);
			return status;
		}
		vm_unreserve_address_range(team->id, base, high - low);
		bias = (addr_t)base - low;
	} else if (!IS_USER_ADDRESS(low) || !IS_USER_ADDRESS(high - 1)) {
		ERROR("image at %#" B_PRIxADDR " is outside user space\n", low);
		return B_NOT_AN_EXECUTABLE;
	}

	const char* baseName = isInterpreter ? "linux_interp" : "linux_exe";
	for (int i = 0; i < ehdr.e_phnum; i++) {
		const renguin_phdr* phdr = program_header(phdrs, &ehdr, i);
		if (phdr->p_type != RENGUIN_PT_LOAD || phdr->p_memsz == 0)
			continue;
		status = map_segment(team->id, fd, phdr, bias, baseName, i);
		if (status != B_OK)
			return status;
	}

	// AT_PHDR: PT_PHDR if present, otherwise the PT_LOAD that contains the
	// program header table in the file.
	addr_t phdrAddress = 0;
	for (int i = 0; i < ehdr.e_phnum; i++) {
		const renguin_phdr* phdr = program_header(phdrs, &ehdr, i);
		if (phdr->p_type == RENGUIN_PT_PHDR) {
			phdrAddress = phdr->p_vaddr + bias;
			break;
		}
	}
	if (phdrAddress == 0) {
		for (int i = 0; i < ehdr.e_phnum; i++) {
			const renguin_phdr* phdr = program_header(phdrs, &ehdr, i);
			if (phdr->p_type == RENGUIN_PT_LOAD
				&& ehdr.e_phoff >= phdr->p_offset
				&& ehdr.e_phoff + (uint64)ehdr.e_phnum * ehdr.e_phentsize
					<= phdr->p_offset + phdr->p_filesz) {
				phdrAddress = phdr->p_vaddr + (ehdr.e_phoff - phdr->p_offset)
					+ bias;
				break;
			}
		}
	}

	info->bias = bias;
	info->entry = ehdr.e_entry + bias;
	info->phdr = phdrAddress;
	info->phentsize = ehdr.e_phentsize;
	info->phnum = ehdr.e_phnum;
	info->low = low + bias;
	info->high = high + bias;
	return B_OK;
}


static image_id
register_linux_image(Team* team, int fd, const char* path, image_type type,
	const linux_image_info* info)
{
	extended_image_info imageInfo;
	memset(&imageInfo, 0, sizeof(imageInfo));
	imageInfo.basic_info.type = type;
	strlcpy(imageInfo.basic_info.name, path, sizeof(imageInfo.basic_info.name));
	imageInfo.basic_info.text = (void*)info->low;
	size_t size = info->high - info->low;
	imageInfo.basic_info.text_size = size > INT32_MAX ? INT32_MAX : (int32)size;
	imageInfo.text_delta = (ssize_t)info->bias;

	struct stat st;
	if (_kern_read_stat(fd, NULL, false, &st, sizeof(st)) == B_OK) {
		imageInfo.basic_info.device = st.st_dev;
		imageInfo.basic_info.node = st.st_ino;
	}

	return register_image(team, &imageInfo, sizeof(imageInfo));
}


//! Push a block of bytes onto the descending user stack, keeping sp valid.
static status_t
push_bytes(addr_t& sp, const void* data, size_t size)
{
	sp -= size;
	if (user_memcpy((void*)sp, data, size) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
push_value(addr_t& sp, uint64 value)
{
	return push_bytes(sp, &value, sizeof(value));
}


/*!	Builds the Linux initial process stack in the main thread's existing user
	stack area and returns the final (16-byte aligned) stack pointer in \a _sp.

	Layout, from high to low addresses: the argument and environment strings,
	the AT_EXECFN and AT_PLATFORM strings, 16 random bytes, then the aligned
	auxiliary vector, envp array, argv array and finally argc at the lowest
	address, which is where rsp points on entry. This is the layout of
	FreeBSD's linux_copyout_strings() / linux_fixup_elf() and Linux's
	create_elf_tables().
*/
static status_t
build_stack(Thread* thread, const foreign_exec_args* args,
	renguin_team* data, addr_t* _sp)
{
	Team* team = thread->team;
	const uint32 argCount = args->arg_count;
	const uint32 envCount = args->env_count;
	char* const* flatArgs = args->flat_args;
	char* const* argv = flatArgs;
	char* const* envp = flatArgs + argCount + 1;

	addr_t sp = thread->user_stack_base + thread->user_stack_size;

	// Reserve room so the strings never reach the very top.
	sp -= 16;

	// Copy strings, recording their user addresses. argCount and envCount
	// may be large, so the address arrays go on the heap, not the kernel
	// stack.
	addr_t* argvAddr = (addr_t*)malloc((argCount + 1) * sizeof(addr_t));
	addr_t* envpAddr = (addr_t*)malloc((envCount + 1) * sizeof(addr_t));
	if (argvAddr == NULL || envpAddr == NULL) {
		free(argvAddr);
		free(envpAddr);
		return B_NO_MEMORY;
	}
	MemoryDeleter argvDeleter(argvAddr);
	MemoryDeleter envpDeleter(envpAddr);

	// Pushed last to first, so that both lists read front to back in memory,
	// arguments below the environment, as on Linux (/proc/<pid>/cmdline and
	// environ are these ranges).
	addr_t envEnd = sp;
	for (uint32 i = envCount; i-- > 0;) {
		size_t len = strlen(envp[i]) + 1;
		if (push_bytes(sp, envp[i], len) != B_OK)
			return B_BAD_ADDRESS;
		envpAddr[i] = sp;
	}
	addr_t envStart = sp;
	for (uint32 i = argCount; i-- > 0;) {
		size_t len = strlen(argv[i]) + 1;
		if (push_bytes(sp, argv[i], len) != B_OK)
			return B_BAD_ADDRESS;
		argvAddr[i] = sp;
	}
	data->arg_start = sp;
	data->arg_end = envStart;
	data->env_start = envStart;
	data->env_end = envEnd;

	// The program's path as the Linux program sees it, for AT_EXECFN.
	char* execfn = (char*)malloc(B_PATH_NAME_LENGTH);
	if (execfn == NULL)
		return B_NO_MEMORY;
	MemoryDeleter execfnDeleter(execfn);
	strlcpy(execfn, args->path, B_PATH_NAME_LENGTH);
	renguin_host_to_linux_path(execfn);
	if (push_bytes(sp, execfn, strlen(execfn) + 1) != B_OK)
		return B_BAD_ADDRESS;
	addr_t execfnAddr = sp;

	static const char kPlatform[] = "x86_64";
	if (push_bytes(sp, kPlatform, sizeof(kPlatform)) != B_OK)
		return B_BAD_ADDRESS;
	addr_t platformAddr = sp;

	// 16 random bytes for AT_RANDOM (libc's stack guard and pointer guard).
	sp &= ~(addr_t)15;
	uint8 random[RENGUIN_AT_RANDOM_LEN];
	for (size_t i = 0; i < sizeof(random); i++)
		random[i] = (uint8)secure_random_value();
	if (push_bytes(sp, random, sizeof(random)) != B_OK)
		return B_BAD_ADDRESS;
	addr_t randomAddr = sp;

	// The auxiliary vector. AT_SYSINFO_EHDR (the vDSO) is deliberately
	// omitted; glibc and musl fall back to real syscalls without it.
	struct { uint64 type; uint64 value; } aux[] = {
		{ RENGUIN_AT_PHDR,		data->phdr },
		{ RENGUIN_AT_PHENT,		data->phentsize },
		{ RENGUIN_AT_PHNUM,		data->phnum },
		{ RENGUIN_AT_PAGESZ,	kLinuxPageSize },
		{ RENGUIN_AT_BASE,		data->interpreter_base },
		{ RENGUIN_AT_FLAGS,		0 },
		{ RENGUIN_AT_ENTRY,		data->entry },
		{ RENGUIN_AT_UID,		(uint64)team->real_uid },
		{ RENGUIN_AT_EUID,		(uint64)team->effective_uid },
		{ RENGUIN_AT_GID,		(uint64)team->real_gid },
		{ RENGUIN_AT_EGID,		(uint64)team->effective_gid },
		{ RENGUIN_AT_SECURE,	0 },
		{ RENGUIN_AT_CLKTCK,	100 },
		{ RENGUIN_AT_HWCAP,		0 },
		{ RENGUIN_AT_HWCAP2,	0 },
		{ RENGUIN_AT_RANDOM,	randomAddr },
		{ RENGUIN_AT_PLATFORM,	platformAddr },
		{ RENGUIN_AT_EXECFN,	execfnAddr },
		{ RENGUIN_AT_NULL,		0 },
	};
	const int auxCount = sizeof(aux) / sizeof(aux[0]);

	// The final rsp must be 16-byte aligned. The vector below it holds:
	// argc(1) + argv(argCount) + NULL(1) + envp(envCount) + NULL(1)
	//   + auxv(auxCount * 2).
	size_t items = 1 + argCount + 1 + envCount + 1 + (size_t)auxCount * 2;
	addr_t vectorsStart = sp - items * sizeof(uint64);
	vectorsStart &= ~(addr_t)15;
	sp = vectorsStart + items * sizeof(uint64);

	// Push from the top of the vector region downward (reverse order).
	for (int i = auxCount - 1; i >= 0; i--) {
		if (push_value(sp, aux[i].value) != B_OK
			|| push_value(sp, aux[i].type) != B_OK) {
			return B_BAD_ADDRESS;
		}
	}
	if (push_value(sp, 0) != B_OK)					// envp terminator
		return B_BAD_ADDRESS;
	for (int i = envCount - 1; i >= 0; i--) {
		if (push_value(sp, envpAddr[i]) != B_OK)
			return B_BAD_ADDRESS;
	}
	if (push_value(sp, 0) != B_OK)					// argv terminator
		return B_BAD_ADDRESS;
	for (int i = argCount - 1; i >= 0; i--) {
		if (push_value(sp, argvAddr[i]) != B_OK)
			return B_BAD_ADDRESS;
	}
	if (push_value(sp, argCount) != B_OK)			// argc
		return B_BAD_ADDRESS;

	if ((sp & 15) != 0)
		ERROR("stack pointer %#" B_PRIxADDR " not 16-byte aligned\n", sp);

	*_sp = sp;
	return B_OK;
}


/*!	Opens the program's ELF interpreter. A Linux interpreter path is resolved
	inside the Linux root first (following symlinks there), then as a plain
	Haiku path. Returns a kernel FD.
*/
static int
open_interpreter(const char* interpreter, bool haveLinuxRoot, char* hostPath,
	size_t hostPathSize)
{
	if (haveLinuxRoot
		&& renguin_resolve_in_root(interpreter, RENGUIN_PATH_FOLLOW_LEAF,
			hostPath, hostPathSize) == B_OK) {
		int fd = _kern_open(-1, hostPath, O_RDONLY, 0);
		if (fd >= 0)
			return fd;
	}

	strlcpy(hostPath, interpreter, hostPathSize);
	return _kern_open(-1, hostPath, O_RDONLY, 0);
}


status_t
renguin_load(foreign_exec_args* args)
{
	Team* team = args->team;
	Thread* thread = args->thread;

	renguin_team* data = (renguin_team*)malloc(sizeof(renguin_team));
	if (data == NULL)
		return B_NO_MEMORY;
	memset(data, 0, sizeof(*data));
	MemoryDeleter dataDeleter(data);
	data->brk_area = -1;
	data->app_image = -1;
	strlcpy(data->exe_path, args->path, sizeof(data->exe_path));
	data->umask = args->umask & 0777;
	renguin_proc_init_team(data);

	struct stat rootStat;
	data->have_linux_root = vfs_read_stat(-1, RENGUIN_LINUX_ROOT, true,
		&rootStat, true) == B_OK && S_ISDIR(rootStat.st_mode);

	linux_image_info executable;
	status_t status = load_elf_image(team, args->fd, false, &executable);
	if (status != B_OK)
		return status;

	data->executable_base = executable.bias;
	data->entry = executable.entry;
	data->phdr = executable.phdr;
	data->phentsize = executable.phentsize;
	data->phnum = executable.phnum;

	addr_t entry = executable.entry;

	if (executable.interpreter[0] != '\0') {
		char* hostPath = (char*)malloc(B_PATH_NAME_LENGTH);
		if (hostPath == NULL)
			return B_NO_MEMORY;
		MemoryDeleter hostPathDeleter(hostPath);

		int fd = open_interpreter(executable.interpreter,
			data->have_linux_root, hostPath, B_PATH_NAME_LENGTH);
		if (fd < 0) {
			ERROR("\"%s\" needs the Linux interpreter %s, which is missing. "
				"Install a Linux userland in " RENGUIN_LINUX_ROOT "\n",
				args->path, executable.interpreter);
			return B_MISSING_LIBRARY;
		}
		FileDescriptorCloser fdCloser(fd);

		linux_image_info interpreter;
		status = load_elf_image(team, fd, true, &interpreter);
		if (status != B_OK) {
			ERROR("loading interpreter %s failed: %s\n", hostPath,
				strerror(status));
			return status;
		}
		data->interpreter_base = interpreter.bias;
		data->interpreter_entry = interpreter.entry;
		entry = interpreter.entry;

		register_linux_image(team, fd, hostPath, B_LIBRARY_IMAGE,
			&interpreter);
	}

	// The program break starts on the page after the executable.
	data->brk_start = executable.high;
	data->brk_current = data->brk_start;
	data->brk_mapped_end = data->brk_start;

	status = build_stack(thread, args, data, &args->stack_pointer);
	if (status != B_OK)
		return status;
	args->entry = entry;

	// Register the program as the team's app image. Besides making it
	// visible to the debugger and to Haiku's process tools, this lets
	// renguin_enter_userspace() complete a load_image(B_WAIT_TILL_LOADED),
	// which runtime_loader normally does.
	data->app_image = register_linux_image(team, args->fd, args->path,
		B_APP_IMAGE, &executable);

	renguin_team_threads_init(data);
	team->personality_data = dataDeleter.Detach();
	TRACE("loaded \"%s\": entry %#" B_PRIxADDR ", sp %#" B_PRIxADDR
		", interpreter base %#" B_PRIxADDR "\n", args->path, args->entry,
		args->stack_pointer, data->interpreter_base);
	return B_OK;
}


status_t
renguin_enter_userspace(Thread* thread, addr_t entry, addr_t stackPointer)
{
	renguin_team* data = (renguin_team*)thread->team->personality_data;

	// If a parent started us with load_image(), it is waiting until the
	// program has been loaded; runtime_loader reports that through
	// _kern_image_relocated() on the app image. Do the same. This also
	// suspends the thread until the parent resumes it, which is what
	// load_image() callers expect.
	if (data != NULL && data->app_image >= 0)
		_user_image_relocated(data->app_image);

	// Initialize the thread's user_thread block, as Haiku's enter_userspace()
	// does for native threads (thread.cpp). The kernel's signal code reads
	// it, e.g. defer_signals. Linux programs never look at it.
	if (thread->user_thread != NULL
		&& user_memset(thread->user_thread, 0, sizeof(user_thread)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	// Linux entry state (linux_exec_setregs()): rsp -> argc, rdx = 0 (no
	// dynamic-linker exit hook), all other general registers 0, FS/GS base 0,
	// clean flags. The FPU state is the initial one, loaded on the way out
	// (x86_return_to_userland(), see renguin_reset_fpu_state() below).
	iframe frame = {};
	frame.type = IFRAME_TYPE_SYSCALL;
	frame.ip = entry;
	frame.cs = USER_CODE_SELECTOR;
	frame.flags = X86_EFLAGS_RESERVED1 | X86_EFLAGS_INTERRUPT;
	frame.sp = stackPointer;
	frame.ss = USER_DATA_SELECTOR;

	// Linux clears FS/GS base; libc sets FS via arch_prctl() during startup.
	thread->user_local_storage = 0;
	thread->arch_info.user_gs_base = 0;
	// A new program starts with the initial FPU state, not whatever the
	// thread last saved (it may have run a native program before).
	renguin_reset_fpu_state(thread);

	x86_initial_return_to_userland(thread, &frame);
	return B_OK;
}


void
renguin_team_released(Team* team)
{
	renguin_team* data = (renguin_team*)team->personality_data;
	if (data != NULL) {
		renguin_team_threads_free(data);
		free(data);
		team->personality_data = NULL;
	}
}
