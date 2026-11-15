/*
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Distributed under the terms of the MIT License.
 */


#include "runtime_loader_private.h"

#include <runtime_loader.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


/*!	Applies the RELA relocations of \a image. \a irelative selects the
	pass: false for all relocations but R_X86_64_IRELATIVE, true for those
	only. IRELATIVE ones (IFUNCs) call a resolver in the image, which may
	use the image's GOT and data, so they come after everything else, as
	in glibc and FreeBSD, and after remap_images() made the code
	executable (Renga, Chromium R-8: lld output with IFUNCs). \a cache is
	not used in the IRELATIVE pass.
*/
static status_t
relocate_rela(image_t* rootImage, image_t* image, Elf64_Rela* rel,
	size_t relLength, SymbolLookupCache* cache, bool irelative)
{
	for (size_t i = 0; i < relLength / sizeof(Elf64_Rela); i++) {
		int type = ELF64_R_TYPE(rel[i].r_info);
		if ((type == R_X86_64_IRELATIVE) != irelative)
			continue;
		if (type == R_X86_64_IRELATIVE) {
			// the resolver's address is base + addend; its result goes to
			// base + offset
			typedef Elf64_Addr (*ifunc_resolver)(void);
			ifunc_resolver resolver = (ifunc_resolver)(
				image->regions[0].delta + rel[i].r_addend);
			*(Elf64_Addr*)(image->regions[0].delta + rel[i].r_offset)
				= resolver();
			continue;
		}
		int symIndex = ELF64_R_SYM(rel[i].r_info);
		Elf64_Addr symAddr = 0;
		image_t* symbolImage = NULL;

		// Resolve the symbol, if any.
		if (symIndex != 0) {
			Elf64_Sym* sym = SYMBOL(image, symIndex);

			status_t status = resolve_symbol(rootImage, image, sym, cache,
				&symAddr, &symbolImage);
			if (status != B_OK) {
				TRACE(("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status));
				printf("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status);
				return status;
			}
		}

		// Address of the relocation.
		Elf64_Addr relocAddr = image->regions[0].delta + rel[i].r_offset;

		// Calculate the relocation value.
		Elf64_Addr relocValue;
		switch (type) {
			case R_X86_64_NONE:
				continue;
			case R_X86_64_64:
			case R_X86_64_GLOB_DAT:
			case R_X86_64_JUMP_SLOT:
				relocValue = symAddr + rel[i].r_addend;
				break;
			case R_X86_64_PC32:
				relocValue = symAddr + rel[i].r_addend - rel[i].r_offset;
				break;
			case R_X86_64_RELATIVE:
				relocValue = image->regions[0].delta + rel[i].r_addend;
				break;
			case R_X86_64_DTPMOD64:
				relocValue = symbolImage == NULL
							? image->dso_tls_id : symbolImage->dso_tls_id;
				break;
			case R_X86_64_DTPOFF32:
			case R_X86_64_DTPOFF64:
				relocValue = symAddr;
				break;
			default:
				TRACE(("unhandled relocation type %d\n", type));
				return B_BAD_DATA;
		}

		if (type == R_X86_64_PC32 || type == R_X86_64_DTPOFF32)
			*(Elf32_Addr *)relocAddr = relocValue;
		else
			*(Elf64_Addr *)relocAddr = relocValue;
	}

	return B_OK;
}


status_t
arch_relocate_image(image_t* rootImage, image_t* image,
	SymbolLookupCache* cache)
{
	status_t status;

	// No REL on x86_64.

	// Perform RELA relocations, then PLT relocations (they are RELA on
	// x86_64). IFUNC (IRELATIVE) relocations come later
	// (arch_relocate_image_ifuncs()).
	if (image->rela) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache, false);
		if (status != B_OK)
			return status;
	}
	if (image->pltrel) {
		status = relocate_rela(rootImage, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, cache, false);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


status_t
arch_relocate_image_ifuncs(image_t* image)
{
	status_t status;
	if (image->rela) {
		status = relocate_rela(image, image, image->rela, image->rela_len,
			NULL, true);
		if (status != B_OK)
			return status;
	}
	if (image->pltrel) {
		status = relocate_rela(image, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, NULL, true);
		if (status != B_OK)
			return status;
	}
	return B_OK;
}
