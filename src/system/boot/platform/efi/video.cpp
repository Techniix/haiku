/*
 * Copyright 2016, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "video.h"

#include <stdlib.h>

#include <boot/kernel_args.h>
#include <boot/menu.h>
#include <boot/platform.h>
#include <boot/platform/generic/video.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <drivers/driver_settings.h>
#include <edid.h>
#include <util/list.h>

#include "efi_platform.h"
#include <efi/protocol/edid.h>
#include <efi/protocol/graphics-output.h>


//#define TRACE_VIDEO
#ifdef TRACE_VIDEO
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


struct video_mode {
	list_link	link;
	size_t		mode;
	size_t		width, height, bits_per_pixel, bytes_per_row;
};


static efi_guid sGraphicsOutputGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static efi_guid sEdidActiveGuid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
static efi_guid sEdidDiscoveredGuid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
static efi_graphics_output_protocol *sGraphicsOutput;
static efi_edid_protocol *sEdidActiveProtocol;
static size_t sGraphicsMode;
// Renga (batch 7): the largest mode (the old default) and the monitor's
// native one, from EDID or the mode the firmware set; "native_mode false"
// in the vesa driver settings goes back to the largest.
static size_t sLargestMode;
static ssize_t sNativeMode = -1;
static const char* sNativeSource = NULL;
static struct list sModeList;
static uint32 sModeCount;
static bool sModeChosen;
static bool sSettingsLoaded;


static int
compare_video_modes(video_mode *a, video_mode *b)
{
	int compare = a->width - b->width;
	if (compare != 0)
		return compare;

	compare = a->height - b->height;
	if (compare != 0)
		return compare;

	return a->bits_per_pixel - b->bits_per_pixel;
}


static void
add_video_mode(video_mode *videoMode)
{
	video_mode *mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode))
			!= NULL) {
		int compare = compare_video_modes(videoMode, mode);
		if (compare == 0) {
			// mode already exists
			return;
		}

		if (compare > 0)
			break;
	}

	list_insert_item_before(&sModeList, mode, videoMode);
	sModeCount++;
}


static video_mode*
closest_video_mode(uint32 width, uint32 height, uint32 depth)
{
	video_mode *bestMode = NULL;
	int64 bestDiff = 0;

	video_mode *mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode)) != NULL) {
		if (mode->width > width) {
			// Only choose modes with a width less or equal than the searched
			// one; or else it might well be that the monitor cannot keep up.
			continue;
		}

		int64 diff = 2 * abs((int64)mode->width - width)
			+ abs((int64)mode->height - height)
			+ abs((int64)mode->bits_per_pixel - depth);

		if (bestMode == NULL || bestDiff > diff) {
			bestMode = mode;
			bestDiff = diff;
		}
	}

	return bestMode;
}


static void
get_mode_from_settings(void)
{
	if (sSettingsLoaded)
		return;

	void *handle = load_driver_settings("vesa");
	if (handle == NULL)
		return;

	const driver_settings *settings = get_driver_settings(handle);
	if (settings == NULL)
		goto out;

	sSettingsLoaded = true;

	// Renga (batch 7): "native_mode false" restores the largest mode
	for (int32 i = 0; i < settings->parameter_count; i++) {
		driver_parameter &parameter = settings->parameters[i];
		if (strcmp(parameter.name, "native_mode") == 0
			&& parameter.value_count >= 1
			&& (strcmp(parameter.values[0], "false") == 0
				|| strcmp(parameter.values[0], "off") == 0)
			&& sNativeMode >= 0 && sGraphicsMode == (size_t)sNativeMode) {
			sGraphicsMode = sLargestMode;
			dprintf("video: native mode off in the settings, largest mode\n");
		}
	}

	for (int32 i = 0; i < settings->parameter_count; i++) {
		driver_parameter &parameter = settings->parameters[i];

		if (parameter.value_count < 3 || strcmp(parameter.name, "mode") != 0) continue;
		uint32 width = strtoul(parameter.values[0], NULL, 0);
		uint32 height = strtoul(parameter.values[1], NULL, 0);
		uint32 depth = strtoul(parameter.values[2], NULL, 0);

		// search mode that fits
		video_mode *mode = closest_video_mode(width, height, depth);
		if (mode != NULL) {
			sGraphicsMode = mode->mode;
			break;
		}
	}

out:
	unload_driver_settings(handle);
}


static int
depth_from_mode_info(efi_graphics_output_mode_information* info)
{
	if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
		return 32;
	} else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
		// seen this in the wild, but acts like RGB, go figure...
		return 32;
	} else if (info->PixelFormat == PixelBitMask
		&& info->PixelInformation.RedMask == 0x3FF00000
		&& info->PixelInformation.GreenMask == 0x000FFC00
		&& info->PixelInformation.BlueMask == 0x000003FF
		&& info->PixelInformation.ReservedMask == 0xC0000000) {
		return 30;
	} else if (info->PixelFormat == PixelBitMask
		&& info->PixelInformation.RedMask == 0xFF0000
		&& info->PixelInformation.GreenMask == 0x00FF00
		&& info->PixelInformation.BlueMask == 0x0000FF
		&& info->PixelInformation.ReservedMask == 0) {
		return 24;
	} else if (info->PixelFormat == PixelBitMask
		&& info->PixelInformation.RedMask == 0xF800
		&& info->PixelInformation.GreenMask == 0x07E0
		&& info->PixelInformation.BlueMask == 0x001F
		&& info->PixelInformation.ReservedMask == 0) {
		return 16;
	}

	return -1;
}


static video_mode*
find_mode(size_t width, size_t height)
{
	video_mode* best = NULL;
	video_mode* mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode)) != NULL) {
		if (mode->width == width && mode->height == height
			&& (best == NULL || mode->bits_per_pixel > best->bits_per_pixel))
			best = mode;
	}
	return best;
}


/*!	Renga (batch 7): the monitor's native mode. EDID's first detailed
	timing is the preferred mode (EDID 1.3 and later); if the GOP lists
	it, that is the native mode. Without EDID, or when the GOP does not
	list it, the mode the firmware set before starting us is used: the
	UEFI specification has the firmware start in the native mode. Upstream
	took the largest mode, which on many machines and in QEMU's OVMF
	(2048x2048 there) is larger than the monitor and slow.
*/
static void
_choose_native_mode()
{
	sNativeMode = -1;
	sNativeSource = NULL;
	edid1_info* info = (edid1_info*)gKernelArgs.edid_info.Pointer();
	if (info != NULL) {
		for (int32 i = 0; i < EDID1_NUM_DETAILED_MONITOR_DESC; i++) {
			edid1_detailed_monitor& monitor = info->detailed_monitor[i];
			if (monitor.monitor_desc_type != EDID1_IS_DETAILED_TIMING)
				continue;
			video_mode* mode = find_mode(
				monitor.data.detailed_timing.h_active,
				monitor.data.detailed_timing.v_active);
			if (mode != NULL) {
				sNativeMode = mode->mode;
				sNativeSource = "EDID";
			}
			break;
		}
	}
	if (sNativeMode < 0 && sGraphicsOutput->Mode != NULL
		&& sGraphicsOutput->Mode->Info != NULL) {
		size_t width = sGraphicsOutput->Mode->Info->HorizontalResolution;
		size_t height = sGraphicsOutput->Mode->Info->VerticalResolution;
		video_mode* mode = find_mode(width, height);
		if (mode != NULL && width >= 640 && height >= 480) {
			sNativeMode = mode->mode;
			sNativeSource = "firmware";
		}
	}
	if (sNativeMode >= 0) {
		video_mode* mode = NULL;
		while ((mode = (video_mode*)list_get_next_item(&sModeList, mode))
				!= NULL) {
			if (mode->mode == (size_t)sNativeMode) {
				dprintf("video: native mode %lux%lu from %s\n", mode->width,
					mode->height, sNativeSource);
				break;
			}
		}
	} else
		dprintf("video: no native mode found, largest mode\n");
}


extern "C" status_t
platform_init_video(void)
{
	list_init(&sModeList);

	// we don't support VESA modes
	gKernelArgs.vesa_modes = NULL;
	gKernelArgs.vesa_modes_size = 0;

	gKernelArgs.edid_info = NULL;

	// make a guess at the best video mode to use, and save the mode ID for switching to graphics
	// mode
	efi_status status = kBootServices->LocateProtocol(&sGraphicsOutputGuid, NULL,
		(void **)&sGraphicsOutput);
	if (sGraphicsOutput == NULL || status != EFI_SUCCESS) {
		dprintf("GOP protocol not found\n");
		gKernelArgs.frame_buffer.enabled = false;
		sGraphicsOutput = NULL;
		return B_ERROR;
	}

	size_t bestArea = 0;
	size_t bestDepth = 0;

	TRACE(("looking for best graphics mode...\n"));
	for (size_t mode = 0; mode < sGraphicsOutput->Mode->MaxMode; ++mode) {
		efi_graphics_output_mode_information *info = NULL;
		size_t size;
		sGraphicsOutput->QueryMode(sGraphicsOutput, mode, &size, &info);
		size_t area = info->HorizontalResolution * info->VerticalResolution;
		TRACE(("  mode: %lu\n", mode));
		TRACE(("  width: %u\n", info->HorizontalResolution));
		TRACE(("  height: %u\n", info->VerticalResolution));
		TRACE(("  area: %lu\n", area));

		int depth = depth_from_mode_info(info);
		if (depth == -1) {
			TRACE(("  pixel format: %x unsupported\n",
				info->PixelFormat));
			continue;
		}
		TRACE(("  depth: %lu\n", depth));

		video_mode *videoMode = (video_mode*)malloc(sizeof(struct video_mode));
		if (videoMode != NULL) {
			videoMode->mode = mode;
			videoMode->width = info->HorizontalResolution;
			videoMode->height = info->VerticalResolution;
			videoMode->bits_per_pixel = depth;
			videoMode->bytes_per_row = info->PixelsPerScanLine * ((depth + 7) / 8);
			add_video_mode(videoMode);
		}

		area *= depth;
		TRACE(("  area (w/depth): %lu\n", area));
		if (area >= bestArea) {
			TRACE(("selected new best mode: %lu\n", mode));
			bestArea = area;
			bestDepth = depth;
			sGraphicsMode = mode;
		}
	}

	if (bestArea == 0 || bestDepth == 0) {
		sGraphicsOutput = NULL;
		gKernelArgs.frame_buffer.enabled = false;
		return B_ERROR;
	}

	gKernelArgs.frame_buffer.enabled = true;
	sModeChosen = false;
	sSettingsLoaded = false;
	sLargestMode = sGraphicsMode;

	status = kBootServices->LocateProtocol(&sEdidActiveGuid, NULL, (void **)&sEdidActiveProtocol);
	if (sEdidActiveProtocol == NULL || status != EFI_SUCCESS
		|| sEdidActiveProtocol->SizeOfEdid == 0) {
		// Renga (batch 7): some firmware only has the discovered EDID
		sEdidActiveProtocol = NULL;
		status = kBootServices->LocateProtocol(&sEdidDiscoveredGuid, NULL,
			(void **)&sEdidActiveProtocol);
	}
	if ((sEdidActiveProtocol != NULL) && (status == EFI_SUCCESS)
		&& (sEdidActiveProtocol->SizeOfEdid) >= sizeof(edid1_raw)) {
		edid1_info* edid_info = (edid1_info*)kernel_args_malloc(sizeof(edid1_info));
		if (edid_info != NULL) {
			edid_decode(edid_info, (edid1_raw*)sEdidActiveProtocol->Edid);
			gKernelArgs.edid_info = edid_info;
		}
	}

	_choose_native_mode();
	if (sNativeMode >= 0)
		sGraphicsMode = sNativeMode;

	return B_OK;
}


extern "C" void
platform_switch_to_logo(void)
{
	if (sGraphicsOutput == NULL || !gKernelArgs.frame_buffer.enabled)
		return;

	if (!sModeChosen)
		get_mode_from_settings();

	sGraphicsOutput->SetMode(sGraphicsOutput, sGraphicsMode);
	gKernelArgs.frame_buffer.physical_buffer.start =
		sGraphicsOutput->Mode->FrameBufferBase;
	gKernelArgs.frame_buffer.physical_buffer.size =
		sGraphicsOutput->Mode->FrameBufferSize;
	gKernelArgs.frame_buffer.width = sGraphicsOutput->Mode->Info->HorizontalResolution;
	gKernelArgs.frame_buffer.height = sGraphicsOutput->Mode->Info->VerticalResolution;
	int depth = depth_from_mode_info(sGraphicsOutput->Mode->Info);
	gKernelArgs.frame_buffer.depth = depth;
	dprintf("video: using %" B_PRIu32 "x%" B_PRIu32 "\n",
		gKernelArgs.frame_buffer.width, gKernelArgs.frame_buffer.height);
	gKernelArgs.frame_buffer.bytes_per_row = sGraphicsOutput->Mode->Info->PixelsPerScanLine
			* ((depth + 7) / 8);

	video_display_splash(gKernelArgs.frame_buffer.physical_buffer.start);
}


bool
video_mode_hook(Menu *menu, MenuItem *item)
{
	Menu* submenu = item->Submenu();
	MenuItem* subitem = submenu->FindMarked();
	if (subitem != NULL) {
		sGraphicsMode = (size_t)subitem->Data();
		sModeChosen = true;
	}

	return true;
}


Menu*
video_mode_menu()
{
	Menu *menu = new(std::nothrow)Menu(CHOICE_MENU, "Select Video Mode");
	MenuItem *item;

	video_mode *mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode)) != NULL) {
		char label[64];
		snprintf(label, sizeof(label), "%lux%lu %lu bit", mode->width,
			mode->height, mode->bits_per_pixel);

		menu->AddItem(item = new (std::nothrow)MenuItem(label));
		item->SetData((const void*)mode->mode);
		if (mode->mode == sGraphicsMode) {
			item->SetMarked(true);
			item->Select(true);
		}
	}

	menu->AddSeparatorItem();
	menu->AddItem(item = new(std::nothrow)MenuItem("Return to main menu"));
	item->SetType(MENU_ITEM_NO_CHOICE);

	return menu;
}


extern "C" void
platform_blit4(addr_t frameBuffer, const uint8 *data,
	uint16 width, uint16 height, uint16 imageWidth,
	uint16 left, uint16 top)
{
	panic("platform_blit4 unsupported");
	return;
}


extern "C" void
platform_set_palette(const uint8 *palette)
{
	panic("platform_set_palette unsupported");
	return;
}
