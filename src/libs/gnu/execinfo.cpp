/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * backtrace(), backtrace_symbols(), backtrace_symbols_fd() (glibc,
 * FreeBSD libexecinfo).
 *
 * Frames come from libgcc's unwinder (_Unwind_Backtrace), which uses the
 * .eh_frame tables, so it works for code built without frame pointers.
 * Symbols come from each image's dynamic symbol table, read in place, and
 * a frame is only named after a function it lies inside (st_value up to
 * st_value + st_size). glibc's dladdr()-based lookup names the nearest
 * exported symbol below the address instead, which is wrong for frames in
 * static functions; here those print as an image offset, which addr2line
 * can resolve. The output format is glibc's:
 *     /path/to/image(symbol+0x1f) [0x7f...]
 *     /path/to/image(+0x1234) [0x7f...]
 * backtrace_symbols_fd() does not allocate, so it can be used from a
 * crash handler, like glibc's.
 */


#include <execinfo.h>

#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

#include <OS.h>
#include <image.h>


namespace {

struct BacktraceState {
	void**	buffer;
	int		size;
	int		count;
	bool	skipped;	// this function's own frame
};

}	// namespace


static _Unwind_Reason_Code
unwind_callback(struct _Unwind_Context* context, void* data)
{
	BacktraceState* state = (BacktraceState*)data;
	if (!state->skipped) {
		state->skipped = true;
		return _URC_NO_REASON;
	}
	if (state->count >= state->size)
		return _URC_END_OF_STACK;

	uintptr_t ip = _Unwind_GetIP(context);
	if (ip == 0)
		return _URC_END_OF_STACK;
	state->buffer[state->count++] = (void*)ip;
	return _URC_NO_REASON;
}


int
backtrace(void** buffer, int size)
{
	if (buffer == NULL || size <= 0)
		return 0;

	BacktraceState state = { buffer, size, 0, false };
	_Unwind_Backtrace(&unwind_callback, &state);
	return state.count;
}


#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv)
	typedef Elf64_Ehdr	ElfHeader;
	typedef Elf64_Phdr	ProgramHeader;
	typedef Elf64_Dyn	DynamicEntry;
	typedef Elf64_Sym	Symbol;
#	define SYMBOL_TYPE(info)	ELF64_ST_TYPE(info)
#else
	typedef Elf32_Ehdr	ElfHeader;
	typedef Elf32_Phdr	ProgramHeader;
	typedef Elf32_Dyn	DynamicEntry;
	typedef Elf32_Sym	Symbol;
#	define SYMBOL_TYPE(info)	ELF32_ST_TYPE(info)
#endif


//!	Number of dynamic symbols, from DT_GNU_HASH (which has no count field).
static uint32
gnu_hash_symbol_count(const uint32* table)
{
	uint32 bucketCount = table[0];
	uint32 symbolOffset = table[1];
	uint32 bloomSize = table[2];
	const uint32* buckets = table + 4 + bloomSize * (sizeof(size_t) / 4);
	const uint32* chains = buckets + bucketCount;

	uint32 last = 0;
	for (uint32 i = 0; i < bucketCount; i++) {
		if (buckets[i] > last)
			last = buckets[i];
	}
	if (last < symbolOffset)
		return symbolOffset;
	// Walk the last chain to its end (the entry with the low bit set).
	while ((chains[last - symbolOffset] & 1) == 0)
		last++;
	return last + 1;
}


/*!	Finds the function containing "target" in the image's dynamic symbol
	table. Returns false if none does (static function, stripped image).
*/
static bool
find_function(const image_info& info, uintptr_t target, const char*& _name,
	uintptr_t& _start)
{
	const ElfHeader* header = (const ElfHeader*)info.text;
	if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0)
		return false;
	const ProgramHeader* programHeaders = (const ProgramHeader*)
		((const char*)header + header->e_phoff);

	// The dynamic section's addresses are unrelocated: add the load delta,
	// as the runtime loader does (region 0 is the first PT_LOAD).
	uintptr_t delta = 0;
	bool haveDelta = false;
	const DynamicEntry* dynamic = NULL;
	for (int i = 0; i < header->e_phnum; i++) {
		if (programHeaders[i].p_type == PT_LOAD && !haveDelta) {
			delta = (uintptr_t)info.text - programHeaders[i].p_vaddr;
			haveDelta = true;
		} else if (programHeaders[i].p_type == PT_DYNAMIC)
			dynamic = (const DynamicEntry*)programHeaders[i].p_vaddr;
	}
	if (dynamic == NULL || !haveDelta)
		return false;
	dynamic = (const DynamicEntry*)((uintptr_t)dynamic + delta);

	const Symbol* symbols = NULL;
	const char* strings = NULL;
	uint32 count = 0;
	for (; dynamic->d_tag != DT_NULL; dynamic++) {
		uintptr_t pointer = dynamic->d_un.d_ptr + delta;
		switch (dynamic->d_tag) {
			case DT_SYMTAB:
				symbols = (const Symbol*)pointer;
				break;
			case DT_STRTAB:
				strings = (const char*)pointer;
				break;
			case DT_HASH:
				count = ((const uint32*)pointer)[1];	// nchain
				break;
			case DT_GNU_HASH:
				if (count == 0)
					count = gnu_hash_symbol_count((const uint32*)pointer);
				break;
		}
	}
	if (symbols == NULL || strings == NULL)
		return false;

	for (uint32 i = 0; i < count; i++) {
		const Symbol& symbol = symbols[i];
		if (SYMBOL_TYPE(symbol.st_info) != STT_FUNC || symbol.st_value == 0)
			continue;
		uintptr_t start = symbol.st_value + delta;
		if (target >= start && target < start + symbol.st_size) {
			_name = strings + symbol.st_name;
			_start = start;
			return true;
		}
	}
	return false;
}


/*!	Formats one frame into "line" (no allocation). Return addresses point
	after the call instruction, so the lookup uses address - 1.
*/
static void
format_frame(void* address, char* line, size_t size)
{
	uintptr_t target = (uintptr_t)address - 1;

	image_info info;
	int32 cookie = 0;
	bool found = false;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (target >= (uintptr_t)info.text
			&& target < (uintptr_t)info.text + info.text_size) {
			found = true;
			break;
		}
	}
	if (!found) {
		snprintf(line, size, "[%p]", address);
		return;
	}

	const char* name;
	uintptr_t start;
	if (find_function(info, target, name, start)) {
		snprintf(line, size, "%s(%s+0x%lx) [%p]", info.name, name,
			(unsigned long)((uintptr_t)address - start), address);
	} else {
		snprintf(line, size, "%s(+0x%lx) [%p]", info.name,
			(unsigned long)((uintptr_t)address - (uintptr_t)info.text),
			address);
	}
}


char**
backtrace_symbols(void* const* buffer, int size)
{
	if (buffer == NULL || size <= 0)
		return NULL;

	// One allocation: the pointer array followed by the strings, so the
	// caller frees everything with a single free(), as with glibc.
	char lines[size][B_PATH_NAME_LENGTH + 320];
	size_t total = size * sizeof(char*);
	for (int i = 0; i < size; i++) {
		format_frame(buffer[i], lines[i], sizeof(lines[i]));
		total += strlen(lines[i]) + 1;
	}

	char** result = (char**)malloc(total);
	if (result == NULL)
		return NULL;

	char* strings = (char*)(result + size);
	for (int i = 0; i < size; i++) {
		size_t length = strlen(lines[i]) + 1;
		memcpy(strings, lines[i], length);
		result[i] = strings;
		strings += length;
	}
	return result;
}


void
backtrace_symbols_fd(void* const* buffer, int size, int fd)
{
	for (int i = 0; buffer != NULL && i < size; i++) {
		char line[B_PATH_NAME_LENGTH + 320];
		format_frame(buffer[i], line, sizeof(line) - 1);
		size_t length = strlen(line);
		line[length++] = '\n';
		write(fd, line, length);
	}
}
