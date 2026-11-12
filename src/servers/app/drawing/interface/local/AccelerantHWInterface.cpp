/*
 * Copyright 2001-2016 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stephan Aßmus, superstippi@gmx.de
 *		DarkWyrm, bpmagic@columbus.rr.com
 *		Axel Dörfler, axeld@pinc-software.de
 *		Michael Lotz, mmlr@mlotz.ch
 *		John Scipione, jscipione@gmail.com
 */


//!	Accelerant based HWInterface implementation


#include "AccelerantHWInterface.h"

#include <new>

#include <dirent.h>
#include <edid.h>
#include <driver_settings.h>
#include <graphic_driver.h>
#include <image.h>
#include <safemode_defs.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <syscalls.h>
#include <syslog.h>
#include <unistd.h>

#include <Accelerant.h>
#include <HardwareCursor.h>
#include <Cursor.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>
#include <PathFinder.h>
#include <String.h>
#include <StringList.h>

#include "AccelerantBuffer.h"
#include "MallocBuffer.h"
#include "Overlay.h"
#include "RGBColor.h"
#include "ServerConfig.h"
#include "ServerCursor.h"
#include "ServerProtocol.h"
#include "SystemPalette.h"


using std::nothrow;


#ifdef DEBUG_DRIVER_MODULE
#	include <stdio.h>
#	define ATRACE(x) printf x
#else
#	define ATRACE(x) ;
#endif


const int32 kDefaultParamsCount = 64;


bool
operator==(const display_mode& a, const display_mode& b)
{
	return memcmp(&a, &b, sizeof(display_mode)) == 0;
}


bool
use_fail_safe_video_mode()
{
	char buffer[B_FILE_NAME_LENGTH];
	size_t size = sizeof(buffer);

	status_t status = _kern_get_safemode_option(
		B_SAFEMODE_FAIL_SAFE_VIDEO_MODE, buffer, &size);
	if (status == B_OK) {
		if (!strncasecmp(buffer, "true", size)
			|| !strncasecmp(buffer, "yes", size)
			|| !strncasecmp(buffer, "on", size)
			|| !strncasecmp(buffer, "enabled", size)) {
			return true;
		}
	}

	return false;
}


//	#pragma mark - AccelerantHWInterface


AccelerantHWInterface::AccelerantHWInterface()
	:
	HWInterface(),
	fCardFD(-1),
	fAccelerantImage(-1),
	fAccelerantHook(NULL),
	fEngineToken(NULL),
	fSyncToken(),

	// required hooks
	fAccGetModeCount(NULL),
	fAccGetModeList(NULL),
	fAccGetFrameBufferConfig(NULL),
	fAccSetDisplayMode(NULL),
	fAccGetDisplayMode(NULL),
	fAccGetPixelClockLimits(NULL),

	// optional accelerant hooks
	fAccGetTimingConstraints(NULL),
	fAccProposeDisplayMode(NULL),
	fAccSetCursorShape(NULL),
	fAccSetCursorBitmap(NULL),
	fAccMoveCursor(NULL),
	fAccShowCursor(NULL),
	fAccGetCursorMaxSize(NULL),
	fHardwareCursorWidth(0),
	fHardwareCursorHeight(0),
	fDragCursorInHardware(false),
	fSoftwareCursorFallbacks(0),
	fDragBitmapsInHardware(true),
	fTestCursorHooks(false),
	fSavedSetCursorShape(NULL),
	fSavedSetCursorBitmap(NULL),
	fSavedMoveCursor(NULL),
	fSavedShowCursor(NULL),
	fSavedGetCursorMaxSize(NULL),

	// dpms hooks
	fAccDPMSCapabilities(NULL),
	fAccDPMSMode(NULL),
	fAccSetDPMSMode(NULL),

	// brightness hooks
	fAccSetBrightness(NULL),
	fAccGetBrightness(NULL),

	// overlay hooks
	fAccOverlayCount(NULL),
	fAccOverlaySupportedSpaces(NULL),
	fAccOverlaySupportedFeatures(NULL),
	fAccAllocateOverlayBuffer(NULL),
	fAccReleaseOverlayBuffer(NULL),
	fAccGetOverlayConstraints(NULL),
	fAccAllocateOverlay(NULL),
	fAccReleaseOverlay(NULL),
	fAccConfigureOverlay(NULL),

	fModeCount(0),
	fModeList(NULL),

	fBackBuffer(NULL),
	fFrontBuffer(new (nothrow) AccelerantBuffer()),

	fInitialModeSwitch(true),

	fRetraceSemaphore(-1),

	fRectParams(new (nothrow) fill_rect_params[kDefaultParamsCount]),
	fRectParamsCount(kDefaultParamsCount),
	fBlitParams(new (nothrow) blit_params[kDefaultParamsCount]),
	fBlitParamsCount(kDefaultParamsCount)
{
	fDisplayMode.virtual_width = 0;
	fDisplayMode.virtual_height = 0;
	fDisplayMode.space = B_RGB32;

	// NOTE: I have no clue what I'm doing here.
	//fSyncToken.counter = 0;
	//fSyncToken.engine_id = 0;
	memset(&fSyncToken, 0, sizeof(sync_token));
}


AccelerantHWInterface::~AccelerantHWInterface()
{
	delete[] fRectParams;
	delete[] fBlitParams;

	delete[] fModeList;
}


/*!	Opens the first available graphics device and initializes it.

	\return B_OK on success or an appropriate error message on failure.
*/
status_t
AccelerantHWInterface::Initialize()
{
	status_t ret = HWInterface::Initialize();

	if (!fRectParams || !fBlitParams)
		return B_NO_MEMORY;

	if (ret >= B_OK) {
		for (int32 i = 0; fCardFD != B_ENTRY_NOT_FOUND; i++) {
			fCardFD = _OpenGraphicsDevice(i);
			if (fCardFD < 0) {
				ATRACE(("Failed to open graphics device\n"));
				continue;
			}

			if (_OpenAccelerant(fCardFD) == B_OK)
				break;

			close(fCardFD);
			// _OpenAccelerant() failed, try to open next graphics card
		}

		return fCardFD >= 0 ? B_OK : fCardFD;
	}
	return ret;
}


/*!	Proceeds with a recursive scan, avoiding vesa and framebuffer devices.


	\return Whether a device path matching the \a deviceNumber is found.
*/
bool
AccelerantHWInterface::_RecursiveScan(const char* directory, int deviceNumber, int &count,
	char *_path)
{
	ATRACE(("_RecursiveScan directory: %s\n", directory));

	BEntry entry;
	BDirectory dir(directory);
	while (dir.GetNextEntry(&entry) == B_OK) {
		BPath path;
		entry.GetPath(&path);
		if (!strcmp(path.Path(), "/dev/graphics/vesa")
			|| !strcmp(path.Path(), "/dev/graphics/framebuffer")) {
			continue;
		}

		if (entry.IsDirectory()) {
			if (_RecursiveScan(path.Path(), deviceNumber, count, _path))
				return true;
		} else {
			if (count == deviceNumber) {
				strlcpy(_path, path.Path(), PATH_MAX);
				return true;
			}
			count++;
		}
	}
	return false;
}


/*!	Opens a graphics device for read-write access.

	The \a deviceNumber is relative to the number of graphics devices that can
	be opened. One represents the first card that can be opened (not necessarily
	the first one listed in the directory).

	Graphics drivers must be able to be opened more than once, so we really get
	the first working entry.

	\param deviceNumber Number identifying which graphics card to open
	       (1 for first card).

	\return The file descriptor of the opened graphics device.
*/
int
AccelerantHWInterface::_OpenGraphicsDevice(int deviceNumber)
{
	int device = -1;
	int count = 0;
	if (!use_fail_safe_video_mode()) {
		char path[PATH_MAX];
		if (_RecursiveScan("/dev/graphics/", deviceNumber, count, path))
			device = open(path, B_READ_WRITE);
	}

	// Open VESA or Framebuffer driver if we were not able to get a better one.
	if (device == -1 && count < deviceNumber) {
		device = open("/dev/graphics/vesa", B_READ_WRITE);
		if (device > 0) {
			// store the device, so that we can access the planar blitter
			fVGADevice = device;
		} else {
			device = open("/dev/graphics/framebuffer", B_READ_WRITE);
		}

		if (device < 0)
			return B_ENTRY_NOT_FOUND;
	}

	return device;
}


status_t
AccelerantHWInterface::_OpenAccelerant(int device)
{
	char signature[1024];
	if (ioctl(device, B_GET_ACCELERANT_SIGNATURE,
			&signature, sizeof(signature)) != B_OK) {
		return B_ERROR;
	}

	ATRACE(("accelerant signature is: %s\n", signature));

	fAccelerantImage = -1;

	BString leafPath("/accelerants/");
	leafPath << signature;
	BStringList addOnPaths;
	BPathFinder::FindPaths(B_FIND_PATH_ADD_ONS_DIRECTORY, leafPath.String(),
		addOnPaths);
	int32 count = addOnPaths.CountStrings();
	for (int32 i = 0; i < count; i++) {
		const char* path = addOnPaths.StringAt(i).String();
		struct stat accelerantStat;
		if (stat(path, &accelerantStat) != 0)
			continue;

		ATRACE(("accelerant path is: %s\n", path));

		fAccelerantImage = load_add_on(path);
		if (fAccelerantImage >= 0) {
			if (get_image_symbol(fAccelerantImage, B_ACCELERANT_ENTRY_POINT,
					B_SYMBOL_TYPE_ANY, (void**)(&fAccelerantHook)) != B_OK) {
				ATRACE(("unable to get B_ACCELERANT_ENTRY_POINT\n"));
				unload_add_on(fAccelerantImage);
				fAccelerantImage = -1;
				return B_ERROR;
			}

			init_accelerant initAccelerant;
			initAccelerant = (init_accelerant)fAccelerantHook(
				B_INIT_ACCELERANT, NULL);
			if (!initAccelerant || initAccelerant(device) != B_OK) {
				ATRACE(("InitAccelerant unsuccessful\n"));
				unload_add_on(fAccelerantImage);
				fAccelerantImage = -1;
				return B_ERROR;
			}

			break;
		}
	}

	if (fAccelerantImage < B_OK)
		return B_ERROR;

	if (_SetupDefaultHooks() != B_OK) {
		syslog(LOG_ERR, "Accelerant %s does not export the required hooks.\n",
			signature);

		uninit_accelerant uninitAccelerant = (uninit_accelerant)
			fAccelerantHook(B_UNINIT_ACCELERANT, NULL);
		if (uninitAccelerant != NULL)
			uninitAccelerant();

		unload_add_on(fAccelerantImage);
		return B_ERROR;
	}

	return B_OK;
}


status_t
AccelerantHWInterface::_SetupDefaultHooks()
{
	// required
	fAccGetModeCount
		= (accelerant_mode_count)fAccelerantHook(B_ACCELERANT_MODE_COUNT, NULL);
	fAccGetModeList = (get_mode_list)fAccelerantHook(B_GET_MODE_LIST, NULL);
	fAccGetFrameBufferConfig = (get_frame_buffer_config)fAccelerantHook(
		B_GET_FRAME_BUFFER_CONFIG, NULL);
	fAccSetDisplayMode
		= (set_display_mode)fAccelerantHook(B_SET_DISPLAY_MODE, NULL);
	fAccGetDisplayMode
		= (get_display_mode)fAccelerantHook(B_GET_DISPLAY_MODE, NULL);
	fAccGetPixelClockLimits = (get_pixel_clock_limits)fAccelerantHook(
		B_GET_PIXEL_CLOCK_LIMITS, NULL);

	if (!fAccGetFrameBufferConfig || !fAccGetModeCount || !fAccGetModeList
			|| !fAccSetDisplayMode || !fAccGetDisplayMode || !fAccGetPixelClockLimits) {
		return B_ERROR;
	}

	// optional
	fAccGetTimingConstraints = (get_timing_constraints)fAccelerantHook(
		B_GET_TIMING_CONSTRAINTS, NULL);
	fAccProposeDisplayMode = (propose_display_mode)fAccelerantHook(
		B_PROPOSE_DISPLAY_MODE, NULL);
	fAccGetPreferredDisplayMode = (get_preferred_display_mode)fAccelerantHook(
		B_GET_PREFERRED_DISPLAY_MODE, NULL);
	fAccGetMonitorInfo
		= (get_monitor_info)fAccelerantHook(B_GET_MONITOR_INFO, NULL);
	fAccGetEDIDInfo = (get_edid_info)fAccelerantHook(B_GET_EDID_INFO, NULL);

	// cursor
	fAccSetCursorShape
		= (set_cursor_shape)fAccelerantHook(B_SET_CURSOR_SHAPE, NULL);
	fAccSetCursorBitmap
		= (set_cursor_bitmap)fAccelerantHook(B_SET_CURSOR_BITMAP, NULL);
	fAccMoveCursor = (move_cursor)fAccelerantHook(B_MOVE_CURSOR, NULL);
	fAccShowCursor = (show_cursor)fAccelerantHook(B_SHOW_CURSOR, NULL);
	fAccGetCursorMaxSize = (get_cursor_max_size)fAccelerantHook(
		B_GET_CURSOR_MAX_SIZE, NULL);
		// Renga (F-59), optional
	_LoadCursorSettings();

	// dpms
	fAccDPMSCapabilities
		= (dpms_capabilities)fAccelerantHook(B_DPMS_CAPABILITIES, NULL);
	fAccDPMSMode = (dpms_mode)fAccelerantHook(B_DPMS_MODE, NULL);
	fAccSetDPMSMode = (set_dpms_mode)fAccelerantHook(B_SET_DPMS_MODE, NULL);

	// brightness
	fAccGetBrightness = (get_brightness)fAccelerantHook(B_GET_BRIGHTNESS, NULL);
	fAccSetBrightness = (set_brightness)fAccelerantHook(B_SET_BRIGHTNESS, NULL);

	return B_OK;
}


void
AccelerantHWInterface::_UpdateHooksAfterModeChange()
{
	// overlay
	fAccOverlayCount = (overlay_count)fAccelerantHook(B_OVERLAY_COUNT, NULL);
	fAccOverlaySupportedSpaces = (overlay_supported_spaces)fAccelerantHook(
		B_OVERLAY_SUPPORTED_SPACES, NULL);
	fAccOverlaySupportedFeatures = (overlay_supported_features)fAccelerantHook(
		B_OVERLAY_SUPPORTED_FEATURES, NULL);
	fAccAllocateOverlayBuffer = (allocate_overlay_buffer)fAccelerantHook(
		B_ALLOCATE_OVERLAY_BUFFER, NULL);
	fAccReleaseOverlayBuffer = (release_overlay_buffer)fAccelerantHook(
		B_RELEASE_OVERLAY_BUFFER, NULL);
	fAccGetOverlayConstraints = (get_overlay_constraints)fAccelerantHook(
		B_GET_OVERLAY_CONSTRAINTS, NULL);
	fAccAllocateOverlay
		= (allocate_overlay)fAccelerantHook(B_ALLOCATE_OVERLAY, NULL);
	fAccReleaseOverlay
		= (release_overlay)fAccelerantHook(B_RELEASE_OVERLAY, NULL);
	fAccConfigureOverlay
		= (configure_overlay)fAccelerantHook(B_CONFIGURE_OVERLAY, NULL);
}


status_t
AccelerantHWInterface::Shutdown()
{
	if (fAccelerantHook != NULL) {
		uninit_accelerant uninitAccelerant
			= (uninit_accelerant)fAccelerantHook(B_UNINIT_ACCELERANT, NULL);
		if (uninitAccelerant != NULL)
			uninitAccelerant();

		fAccelerantHook = NULL;
	}

	if (fAccelerantImage >= 0) {
		unload_add_on(fAccelerantImage);
		fAccelerantImage = -1;
	}

	if (fCardFD >= 0) {
		close(fCardFD);
		fCardFD = -1;
	}

	return B_OK;
}


/*!	Finds the mode in the mode list that is closest to the mode specified.
	As long as the mode list is not empty, this method will always succeed.
*/
status_t
AccelerantHWInterface::_FindBestMode(const display_mode& compareMode,
	float compareAspectRatio, display_mode& modeFound, int32 *_diff) const
{
	int32 bestDiff = 0;
	int32 bestIndex = -1;
	for (int32 i = 0; i < fModeCount; i++) {
		display_mode& mode = fModeList[i];
		float aspectRatio = 0;

		if (compareAspectRatio != 0 && mode.timing.v_display != 0)
			aspectRatio = mode.timing.h_display / mode.timing.v_display;

		// compute some random equality score
		// TODO: check if these scores make sense
		int32 diff
			= 1000 * abs(mode.timing.h_display - compareMode.timing.h_display)
			+ 1000 * abs(mode.timing.v_display - compareMode.timing.v_display)
			+ abs(mode.timing.h_total * mode.timing.v_total
					- compareMode.timing.h_total * compareMode.timing.v_total)
				/ 100
			+ abs((int)(mode.timing.pixel_clock - compareMode.timing.pixel_clock))
				/ 100
			+ (int32)(500 * fabs(aspectRatio - compareAspectRatio))
			+ 100 * abs((int)(mode.space - compareMode.space));

		if (bestIndex == -1 || diff < bestDiff) {
			bestDiff = diff;
			bestIndex = i;
		}
	}

	if (bestIndex < 0)
		return B_ERROR;

	modeFound = fModeList[bestIndex];
	if (_diff != 0)
		*_diff = bestDiff;

	return B_OK;
}


/*!	This method is used for the initial mode set only - because that one
	should really not fail.

	Basically we try to set all modes as found in the mode list the driver
	returned, but we start with the one that best fits the originally
	desired mode.

	The mode list must have been retrieved already.
*/
status_t
AccelerantHWInterface::_SetFallbackMode(display_mode& newMode) const
{
	// At first, we search the closest display mode from the list of
	// supported modes - if that fails, we just take one

	if (_FindBestMode(newMode, 0, newMode) == B_OK
		&& fAccSetDisplayMode(&newMode) == B_OK) {
		return B_OK;
	}

	// That failed as well, this looks like a bug in the graphics
	// driver, but we have to try to be as forgiving as possible
	// here - just take the first mode that works!

	for (int32 i = 0; i < fModeCount; i++) {
		newMode = fModeList[i];
		if (fAccSetDisplayMode(&newMode) == B_OK)
			return B_OK;
	}

	// Well, we tried.
	return B_ERROR;
}


status_t
AccelerantHWInterface::SetMode(const display_mode& mode)
{
	AutoWriteLocker _(this);
	// TODO: There are places this function can fail,
	// maybe it needs to roll back changes in case of an
	// error.

	// prevent from doing the unnecessary
	if (fModeCount > 0 && fFrontBuffer.IsSet() && fDisplayMode == mode) {
		// TODO: better comparison of display modes
		return B_OK;
	}

	// some safety checks
	// TODO: more of those!
	if (!_IsValidMode(mode))
		return B_BAD_VALUE;

	if (!fFrontBuffer.IsSet())
		return B_NO_INIT;

	// just try to set the mode - we let the graphics driver
	// approve or deny the request, as it should know best

	display_mode newMode = mode;

	status_t status = B_ERROR;
	if (!use_fail_safe_video_mode() || !fInitialModeSwitch)
		status = fAccSetDisplayMode(&newMode);
	if (status != B_OK) {
		ATRACE(("setting display mode failed\n"));
		if (!fInitialModeSwitch)
			return status;

		if (fModeList == NULL) {
			status = _UpdateModeList();
			if (status != B_OK)
				return status;
		}

		// If this is the initial mode switch, we try a number of fallback
		// modes first, before we have to fail

		status = use_fail_safe_video_mode()
			? B_ERROR : _SetFallbackMode(newMode);
		if (status != B_OK) {
			// The driver doesn't allow us the mode switch - this usually
			// means we have a driver that doesn't allow mode switches at
			// all.
			// All we can do now is to ask the driver which mode we can
			// use - this is always necessary for VESA mode, for example.
			if (fAccGetDisplayMode(&newMode) != B_OK)
				return B_ERROR;

			// TODO: check if the mode returned is valid!
			if (!_IsValidMode(newMode))
				return B_BAD_DATA;

			// TODO: if the mode switch before fails as well, we must forbid
			//	any uses of this class!
			status = B_OK;
		}
	}

	fDisplayMode = newMode;
	fInitialModeSwitch = false;

	// update frontbuffer
	fFrontBuffer->SetDisplayMode(fDisplayMode);
	if (_UpdateFrameBufferConfig() != B_OK) {
		// TODO: if this fails, we're basically toasted - we need to handle this
		//	differently to crashing later on!
		return B_ERROR;
	}

	// Update the frame buffer used by the on-screen KDL
#ifndef HAIKU_TARGET_PLATFORM_LIBBE_TEST
	uint32 depth = (fFrameBufferConfig.bytes_per_row
		/ fFrontBuffer->Width()) << 3;
	if (fDisplayMode.space == B_RGB15)
		depth = 15;

	_kern_frame_buffer_update((addr_t)fFrameBufferConfig.frame_buffer,
		fFrontBuffer->Width(), fFrontBuffer->Height(),
		depth, fFrameBufferConfig.bytes_per_row);
#endif

	_UpdateHooksAfterModeChange();

	// update backbuffer if necessary
	if (!fBackBuffer.IsSet()
		|| fBackBuffer->Width() != fFrontBuffer->Width()
		|| fBackBuffer->Height() != fFrontBuffer->Height()
		|| (fFrontBuffer->ColorSpace() == B_RGB32 && fBackBuffer.IsSet())) {
		// NOTE: backbuffer is always B_RGBA32, this simplifies the
		// drawing backend implementation tremendously for the time
		// being. The color space conversion is handled in CopyBackToFront()

		fBackBuffer.Unset();

		fBackBuffer.SetTo(new(nothrow) MallocBuffer(
			fFrontBuffer->Width(), fFrontBuffer->Height()));

		status = fBackBuffer.IsSet()
			? fBackBuffer->InitCheck() : B_NO_MEMORY;
		if (status < B_OK) {
			fBackBuffer.Unset();
			return status;
		}
		// clear out backbuffer, alpha is 255 this way
		memset(fBackBuffer->Bits(), 255, fBackBuffer->BitsLength());
	}

	// update color palette configuration if necessary
	if (fDisplayMode.space == B_CMAP8)
		_SetSystemPalette();
	else if (fDisplayMode.space == B_GRAY8)
		_SetGrayscalePalette();

	// notify all listeners about the mode change
	_NotifyFrameBufferChanged();

	return status;
}


void
AccelerantHWInterface::GetMode(display_mode* mode)
{
	if (mode && LockParallelAccess()) {
		*mode = fDisplayMode;
		UnlockParallelAccess();
	}
}


status_t
AccelerantHWInterface::_UpdateModeList()
{
	fModeCount = fAccGetModeCount();
	if (fModeCount <= 0)
		return B_ERROR;

	delete[] fModeList;
	fModeList = new(nothrow) display_mode[fModeCount];
	if (!fModeList)
		return B_NO_MEMORY;

	if (fAccGetModeList(fModeList) != B_OK) {
		ATRACE(("unable to get mode list\n"));
		return B_ERROR;
	}

	return B_OK;
}


status_t
AccelerantHWInterface::_UpdateFrameBufferConfig()
{
	if (fAccGetFrameBufferConfig(&fFrameBufferConfig) != B_OK) {
		ATRACE(("unable to get frame buffer config\n"));
		return B_ERROR;
	}

	fFrontBuffer->SetFrameBufferConfig(fFrameBufferConfig);

	return B_OK;
}


status_t
AccelerantHWInterface::GetDeviceInfo(accelerant_device_info* info)
{
	get_accelerant_device_info GetAccelerantDeviceInfo
		= (get_accelerant_device_info)fAccelerantHook(
			B_GET_ACCELERANT_DEVICE_INFO, NULL);
	if (!GetAccelerantDeviceInfo) {
		ATRACE(("No B_GET_ACCELERANT_DEVICE_INFO hook found\n"));
		return B_UNSUPPORTED;
	}

	return GetAccelerantDeviceInfo(info);
}


status_t
AccelerantHWInterface::GetModeList(display_mode** _modes, uint32* _count)
{
	AutoReadLocker _(this);

	if (_count == NULL || _modes == NULL)
		return B_BAD_VALUE;

	status_t status = B_OK;

	if (fModeList == NULL)
		status = _UpdateModeList();

	if (status >= B_OK) {
		*_modes = new(nothrow) display_mode[fModeCount];
		if (*_modes) {
			*_count = fModeCount;
			memcpy(*_modes, fModeList, sizeof(display_mode) * fModeCount);
		} else {
			*_count = 0;
			status = B_NO_MEMORY;
		}
	}
	return status;
}


status_t
AccelerantHWInterface::GetPixelClockLimits(display_mode *mode, uint32* _low,
	uint32* _high)
{
	if (mode == NULL || _low == NULL || _high == NULL)
		return B_BAD_VALUE;

	AutoReadLocker _(this);
	return fAccGetPixelClockLimits(mode, _low, _high);
}


status_t
AccelerantHWInterface::GetTimingConstraints(
	display_timing_constraints* constraints)
{
	if (constraints == NULL)
		return B_BAD_VALUE;

	AutoReadLocker _(this);

	if (fAccGetTimingConstraints)
		return fAccGetTimingConstraints(constraints);

	return B_UNSUPPORTED;
}


status_t
AccelerantHWInterface::ProposeMode(display_mode* candidate,
	const display_mode* _low, const display_mode* _high)
{
	if (candidate == NULL || _low == NULL || _high == NULL)
		return B_BAD_VALUE;

	AutoReadLocker _(this);

	if (fAccProposeDisplayMode == NULL)
		return B_UNSUPPORTED;

	// avoid const issues
	display_mode high, low;
	high = *_high;
	low = *_low;

	return fAccProposeDisplayMode(candidate, &low, &high);
}


status_t
AccelerantHWInterface::GetPreferredMode(display_mode* preferredMode)
{
	status_t status = B_NOT_SUPPORTED;

	if (fAccGetPreferredDisplayMode != NULL) {
		status = fAccGetPreferredDisplayMode(preferredMode);
		if (status == B_OK)
			return B_OK;
	}

	if (fAccGetEDIDInfo != NULL) {
		edid1_info info;
		uint32 version;
		status = fAccGetEDIDInfo(&info, sizeof(info), &version);
		if (status < B_OK)
			return status;
		if (version != EDID_VERSION_1)
			return B_NOT_SUPPORTED;

		if (fModeList == NULL) {
			status = _UpdateModeList();
			if (status != B_OK)
				return status;
		}

		status = B_NOT_SUPPORTED;
		display_mode bestMode;
		int32 bestDiff = INT_MAX;

		// find preferred mode from EDID info
		for (uint32 i = 0; i < EDID1_NUM_DETAILED_MONITOR_DESC; ++i) {
			if (info.detailed_monitor[i].monitor_desc_type
					!= EDID1_IS_DETAILED_TIMING)
				continue;

			// construct basic mode and find it in the mode list
			const edid1_detailed_timing& timing
				= info.detailed_monitor[i].data.detailed_timing;
			if (timing.h_active < 640 || timing.v_active < 350)
				continue;

			float aspectRatio = 0.0f;
			if (timing.h_size > 0 && timing.v_size > 0)
				aspectRatio = 1.0f * timing.h_size / timing.v_size;

			display_mode modeFound;
			display_mode mode;

			mode.timing.pixel_clock = timing.pixel_clock * 10;
			mode.timing.h_display = timing.h_active;
			mode.timing.h_sync_start = timing.h_active + timing.h_sync_off;
			mode.timing.h_sync_end = mode.timing.h_sync_start
				+ timing.h_sync_width;
			mode.timing.h_total = timing.h_active + timing.h_blank;
			mode.timing.v_display = timing.v_active;
			mode.timing.v_sync_start = timing.v_active + timing.v_sync_off;
			mode.timing.v_sync_end = mode.timing.v_sync_start
				+ timing.v_sync_width;
			mode.timing.v_total = timing.v_active + timing.v_blank;

			mode.space = B_RGB32;
			mode.virtual_width = mode.timing.h_display;
			mode.virtual_height = mode.timing.v_display;

			// TODO: eventually ignore detailed modes for the preferred one
			// if there are more than one usable?
			int32 diff;
			if (_FindBestMode(mode, aspectRatio, modeFound, &diff) == B_OK) {
				status = B_OK;
				if (diff < bestDiff) {
					bestMode = modeFound;
					bestDiff = diff;
				}
			}
		}

		if (status == B_OK)
			*preferredMode = bestMode;
	}

	return status;
}


status_t
AccelerantHWInterface::GetMonitorInfo(monitor_info* info)
{
	status_t status = B_NOT_SUPPORTED;

	if (fAccGetMonitorInfo != NULL) {
		status = fAccGetMonitorInfo(info);
		if (status == B_OK)
			return B_OK;
	}

	if (fAccGetEDIDInfo == NULL)
		return status;

	edid1_info edid;
	uint32 version;
	status = fAccGetEDIDInfo(&edid, sizeof(edid), &version);
	if (status < B_OK)
		return status;
	if (version != EDID_VERSION_1)
		return B_NOT_SUPPORTED;

	memset(info, 0, sizeof(monitor_info));
	strlcpy(info->vendor, edid.vendor.manufacturer, sizeof(info->vendor));
	if (edid.vendor.serial != 0) {
		snprintf(info->serial_number, sizeof(info->serial_number), "%" B_PRIu32,
			edid.vendor.serial);
	}
	info->product_id = edid.vendor.prod_id;
	info->produced.week = edid.vendor.week;
	info->produced.year = edid.vendor.year;
	info->width = edid.display.h_size;
	info->height = edid.display.v_size;

	for (uint32 i = 0; i < EDID1_NUM_DETAILED_MONITOR_DESC; ++i) {
		edid1_detailed_monitor *monitor = &edid.detailed_monitor[i];

		switch (monitor->monitor_desc_type) {
			case EDID1_SERIAL_NUMBER:
				strlcpy(info->serial_number, monitor->data.serial_number,
					sizeof(info->serial_number));
				break;

			case EDID1_MONITOR_NAME:
				// There can be several of these; in this case we'll just
				// overwrite the previous entries
				// TODO: we could append them as well
				strlcpy(info->name, monitor->data.monitor_name,
					sizeof(info->name));
				break;

			case EDID1_MONITOR_RANGES:
			{
				edid1_monitor_range& range = monitor->data.monitor_range;

				info->min_horizontal_frequency = range.min_h;
				info->max_horizontal_frequency = range.max_h;
				info->min_vertical_frequency = range.min_v;
				info->max_vertical_frequency = range.max_v;
				info->max_pixel_clock = range.max_clock * 10000;
				break;
			}

			case EDID1_IS_DETAILED_TIMING:
			{
				edid1_detailed_timing& timing = monitor->data.detailed_timing;
				info->width = timing.h_size / 10.0;
				info->height = timing.v_size / 10.0;
			}

			default:
				break;
		}
	}

	return B_OK;
}


sem_id
AccelerantHWInterface::RetraceSemaphore()
{
	AutoWriteLocker _(this);

	if (fRetraceSemaphore != -1)
		return fRetraceSemaphore;

	accelerant_retrace_semaphore AccelerantRetraceSemaphore =
		(accelerant_retrace_semaphore)fAccelerantHook(
			B_ACCELERANT_RETRACE_SEMAPHORE, NULL);
	if (!AccelerantRetraceSemaphore)
		fRetraceSemaphore = B_UNSUPPORTED;
	else
		fRetraceSemaphore = AccelerantRetraceSemaphore();

	return fRetraceSemaphore;
}


status_t
AccelerantHWInterface::WaitForRetrace(bigtime_t timeout)
{
	sem_id sem = RetraceSemaphore();
	if (sem < 0)
		return sem;

	return acquire_sem_etc(sem, 1, B_RELATIVE_TIMEOUT, timeout);
}


status_t
AccelerantHWInterface::SetDPMSMode(uint32 state)
{
	AutoWriteLocker _(this);

	if (!fAccSetDPMSMode)
		return B_UNSUPPORTED;

	return fAccSetDPMSMode(state);
}


uint32
AccelerantHWInterface::DPMSMode()
{
	AutoReadLocker _(this);

	if (!fAccDPMSMode)
		return B_UNSUPPORTED;

	return fAccDPMSMode();
}


uint32
AccelerantHWInterface::DPMSCapabilities()
{
	AutoReadLocker _(this);

	if (!fAccDPMSCapabilities)
		return B_UNSUPPORTED;

	return fAccDPMSCapabilities();
}


status_t
AccelerantHWInterface::SetBrightness(float brightness)
{
	AutoReadLocker _(this);

	if (!fAccSetBrightness)
		return B_UNSUPPORTED;

	return fAccSetBrightness(brightness);
}


status_t
AccelerantHWInterface::GetBrightness(float* brightness)
{
	AutoReadLocker _(this);

	if (!fAccGetBrightness)
		return B_UNSUPPORTED;

	return fAccGetBrightness(brightness);
}


status_t
AccelerantHWInterface::GetAccelerantPath(BString& string)
{
	image_info info;
	status_t status = get_image_info(fAccelerantImage, &info);
	if (status == B_OK)
		string = info.name;
	return status;
}


status_t
AccelerantHWInterface::GetDriverPath(BString& string)
{
	// TODO: this currently assumes that the accelerant's clone info
	//	is always the path name of its driver (that's the case for
	//	all of our drivers)
	char path[B_PATH_NAME_LENGTH];
	get_accelerant_clone_info getCloneInfo;
	getCloneInfo = (get_accelerant_clone_info)fAccelerantHook(
		B_GET_ACCELERANT_CLONE_INFO, NULL);

	if (getCloneInfo == NULL)
		return B_NOT_SUPPORTED;

	getCloneInfo((void*)path);
	string.SetTo(path);
	return B_OK;
}


// #pragma mark - overlays


overlay_token
AccelerantHWInterface::AcquireOverlayChannel()
{
	if (fAccAllocateOverlay == NULL
		|| fAccReleaseOverlay == NULL)
		return NULL;

	// The current display mode only matters at the time we're planning on
	// showing the overlay channel on screen - that's why we can't use
	// the B_OVERLAY_COUNT hook.
	// TODO: remove fAccOverlayCount if we're not going to need it at all.

	return fAccAllocateOverlay();
}


void
AccelerantHWInterface::ReleaseOverlayChannel(overlay_token token)
{
	if (token == NULL)
		return;

	fAccReleaseOverlay(token);
}


status_t
AccelerantHWInterface::GetOverlayRestrictions(const Overlay* overlay,
	overlay_restrictions* restrictions)
{
	if (overlay == NULL || restrictions == NULL)
		return B_BAD_VALUE;
	if (fAccGetOverlayConstraints == NULL)
		return B_NOT_SUPPORTED;

	overlay_constraints constraints;
	status_t status = fAccGetOverlayConstraints(&fDisplayMode,
		overlay->OverlayBuffer(), &constraints);
	if (status < B_OK)
		return status;

	memset(restrictions, 0, sizeof(overlay_restrictions));
	memcpy(&restrictions->source, &constraints.view, sizeof(overlay_limits));
	memcpy(&restrictions->destination, &constraints.window,
		sizeof(overlay_limits));
	restrictions->min_width_scale = constraints.h_scale.min;
	restrictions->max_width_scale = constraints.h_scale.max;
	restrictions->min_height_scale = constraints.v_scale.min;
	restrictions->max_height_scale = constraints.v_scale.max;

	return B_OK;
}


bool
AccelerantHWInterface::CheckOverlayRestrictions(int32 width, int32 height,
	color_space colorSpace)
{
	if (fAccOverlaySupportedSpaces == NULL
		|| fAccGetOverlayConstraints == NULL
		|| fAccAllocateOverlayBuffer == NULL
		|| fAccReleaseOverlayBuffer == NULL)
		return false;

	// Note: we can't really check the size of the overlay upfront - we
	// must assume fAccAllocateOverlayBuffer() will fail in that case.
	if (width < 0 || width > 65535 || height < 0 || height > 65535)
		return false;

	// check color space

	const uint32* spaces = fAccOverlaySupportedSpaces(&fDisplayMode);
	if (spaces == NULL)
		return false;

	for (int32 i = 0; spaces[i] != 0; i++) {
		if (spaces[i] == (uint32)colorSpace)
			return true;
	}

	return false;
}


const overlay_buffer*
AccelerantHWInterface::AllocateOverlayBuffer(int32 width, int32 height,
	color_space space)
{
	if (fAccAllocateOverlayBuffer == NULL)
		return NULL;

	return fAccAllocateOverlayBuffer(space, width, height);
}


void
AccelerantHWInterface::FreeOverlayBuffer(const overlay_buffer* buffer)
{
	if (buffer == NULL || fAccReleaseOverlayBuffer == NULL)
		return;

	fAccReleaseOverlayBuffer(buffer);
}


void
AccelerantHWInterface::ConfigureOverlay(Overlay* overlay)
{
	// TODO: this only needs to be done on mode changes!
	overlay->SetColorSpace(fDisplayMode.space);

	fAccConfigureOverlay(overlay->OverlayToken(), overlay->OverlayBuffer(),
		overlay->OverlayWindow(), overlay->OverlayView());
}


void
AccelerantHWInterface::HideOverlay(Overlay* overlay)
{
	fAccConfigureOverlay(overlay->OverlayToken(), overlay->OverlayBuffer(),
		NULL, NULL);
}


// #pragma mark - cursor


void
AccelerantHWInterface::SetCursor(ServerCursor* cursor)
{
	// cursor should never be NULL, but let us be safe!!
	if (cursor == NULL || LockExclusiveAccess() == false)
		return;

	// Renga (F-59): HWInterface::SetCursor() first, so that fCursor and
	// the combined cursor and drag bitmap are up to date when the
	// hardware is given its image. It draws the software cursor only
	// while the hardware cursor is off. The exclusive lock is held until
	// the hardware has its image (the lock is recursive), so a
	// SetDragBitmap() in between cannot hand the hardware an older
	// cursor.
	HWInterface::SetCursor(cursor);
		// HWInterface claims ownership of cursor.

	_UpdateHardwareCursor();

	UnlockExclusiveAccess();
}


/*!	Renga (F-59): a drag bitmap (an icon dragged in Tracker, for example)
	is shown by the hardware cursor too. HWInterface::SetDragBitmap()
	combines it with the cursor (_AdoptDragBitmap()); the hardware gets
	that same B_RGBA32 image, the pointer composited over the drag bitmap. Only if the accelerant refuses that image (too large,
	or it has only a monochrome B_SET_CURSOR_SHAPE cursor) is the
	software cursor used, for as long as the drag lasts.
*/
void
AccelerantHWInterface::SetDragBitmap(const ServerBitmap* bitmap,
	const BPoint& offsetFromCursor)
{
	// HWInterface::SetDragBitmap() draws into the frame buffer when the
	// software cursor is in use; it did so without the exclusive lock.
	if (!LockExclusiveAccess())
		return;

	HWInterface::SetDragBitmap(bitmap, offsetFromCursor);
	_UpdateHardwareCursor();

	UnlockExclusiveAccess();
}


void
AccelerantHWInterface::SetCursorVisible(bool visible)
{
	HWInterface::SetCursorVisible(visible);

	if (fHardwareCursorEnabled && LockExclusiveAccess()) {
		if (fAccShowCursor != NULL)
				fAccShowCursor(visible);
		else
			fHardwareCursorEnabled = false;

		UnlockExclusiveAccess();
	}
}


void
AccelerantHWInterface::MoveCursorTo(float x, float y)
{
	HWInterface::MoveCursorTo(x, y);

	if (fHardwareCursorEnabled && LockExclusiveAccess()) {
		if (fAccMoveCursor != NULL)
				fAccMoveCursor((uint16)x, (uint16)y);
		else {
			fHardwareCursorEnabled = false;
			if (fAccShowCursor != NULL)
				fAccShowCursor(false);
		}

		UnlockExclusiveAccess();
	}
}


/*!	Gives the accelerant the image the cursor should show now (the cursor,
	or the cursor and the drag bitmap composited) and switches between
	the hardware and the software cursor as needed. The exclusive access
	lock must be held.
*/
void
AccelerantHWInterface::_UpdateHardwareCursor()
{
	ServerCursorReference cursor = Cursor();
	if (!cursor.IsSet())
		return;

	bool dragShown = _DragBitmapShown();
	bool cursorSet = false;
	bool dragInHardware = false;

	if (dragShown && !fDragBitmapsInHardware) {
		// The setting is off: the software cursor shows drags, as
		// before F-59.
		fSoftwareCursorFallbacks++;
	} else if (fAccSetCursorBitmap != NULL) {
		ServerCursorReference image = cursor;
		if (dragShown) {
			// HWInterface's combined image: the cursor composited over the
			// drag bitmap, premultiplied like every cursor app_server
			// hands the accelerant (CursorManager premultiplies the system
			// cursors), so the hardware shows exactly what the software
			// cursor would.
			image = CursorAndDragBitmap();
			if (image.Get() == cursor.Get()) {
				// _AdoptDragBitmap() could not allocate the combined image
				image.Unset();
			}
			if (image.IsSet() && !_HardwareCursorFits(image.Get())) {
				// The accelerant reported a smaller limit; don't ask.
				image.Unset();
			}
		}

		if (image.IsSet()) {
			// TODO are x and y switched for this, too?
			uint16 xHotSpot = (uint16)image->GetHotSpot().x;
			uint16 yHotSpot = (uint16)image->GetHotSpot().y;

			uint16 width = (uint16)image->Width();
			uint16 height = (uint16)image->Height();

			// Time to talk to the accelerant!
			cursorSet = fAccSetCursorBitmap(width, height, xHotSpot,
				yHotSpot, image->ColorSpace(), (uint16)image->BytesPerRow(),
				image->Bits()) == B_OK;
			dragInHardware = cursorSet && dragShown;
			if (cursorSet) {
				fHardwareCursorWidth = width;
				fHardwareCursorHeight = height;
				fHardwareCursorHotSpot = image->GetHotSpot();
				fHardwareCursorImage = image;
			}
		}
		if (dragShown && !cursorSet)
			fSoftwareCursorFallbacks++;
	}

	if (!cursorSet && !dragShown && cursor->CursorData() != NULL
		&& fAccSetCursorShape != NULL) {
		// BeOS BCursor, 16x16 monochrome; it cannot show a drag bitmap,
		// so it is not used while one is shown.
		uint8 size = cursor->CursorData()[0];
		// CursorData()[1] is color depth (always monochrome)
		// x and y are switched
		uint8 xHotSpot = cursor->CursorData()[3];
		uint8 yHotSpot = cursor->CursorData()[2];

		// Create pointers to the cursor and/xor bit arrays
		// for the BeOS BCursor there are two 32 byte, 16x16 bit arrays
		// in the first:  1 is black,  0 is white
		// in the second: 1 is opaque, 0 is transparent
		// 1st	2nd
		//  0	 0	 transparent
		//  0	 1	 white
		//  1	 0	 transparent
		//  1	 1	 black
		// for the HW cursor the first is ANDed and the second is XORed
		// AND	XOR
		//  0	 0	 white
		//  0	 1	 black
		//  1	 0	 transparent
		//  1	 1	 reverse
		// so, the first 32 bytes are the XOR mask
		const uint8* xorMask = cursor->CursorData() + 4;
		// the second 32 bytes *NOTed* are the AND mask
		// TODO maybe this should be NOTed when copied to the ServerCursor
		uint8 andMask[32];
		const uint8* transMask = cursor->CursorData() + 36;
		for (int32 i = 0; i < 32; i++)
			andMask[i] = ~transMask[i];

		// Time to talk to the accelerant!
		cursorSet = fAccSetCursorShape(size, size, xHotSpot,
			yHotSpot, andMask, xorMask) == B_OK;
		if (cursorSet) {
			fHardwareCursorWidth = size;
			fHardwareCursorHeight = size;
			fHardwareCursorHotSpot = BPoint(xHotSpot, yHotSpot);
			fHardwareCursorImage.Unset();
		}
	}

	if (cursorSet && !fHardwareCursorEnabled) {
		// we switched from SW to HW, so we need to erase the SW cursor
		if (fCursorVisible && fFloatingOverlaysLock.Lock()) {
			IntRect r = _CursorFrame();
			fCursorVisible = false;
				// so the Invalidate doesn't draw it again
			_RestoreCursorArea();
			Invalidate(r);
			fCursorVisible = true;
			fFloatingOverlaysLock.Unlock();
		}
		// and we need to update our position
		if (fAccMoveCursor != NULL)
			fAccMoveCursor((uint16)fCursorLocation.x,
				(uint16)fCursorLocation.y);
	}

	// Keep a hidden cursor hidden (SetCursorVisible() shows it again). It
	// was shown after every cursor change before, so a cursor hidden with
	// SetCursorVisible(false) came back with the next shape.
	if (fAccShowCursor != NULL)
		fAccShowCursor(cursorSet && IsCursorVisible());

	bool switchedToSoftware = fHardwareCursorEnabled && !cursorSet;
	fHardwareCursorEnabled = cursorSet;
	fDragCursorInHardware = dragInHardware;
	if (!cursorSet)
		fHardwareCursorImage.Unset();

	if (switchedToSoftware) {
		// HWInterface only draws the software cursor when the cursor or
		// the drag bitmap changes; it changed before the switch.
		_ShowSoftwareCursor();
	}
}


/*!	Draws the software cursor (with the drag bitmap, if any) where it is
	now. The exclusive access lock must be held.
*/
void
AccelerantHWInterface::_ShowSoftwareCursor()
{
	if (fHardwareCursorEnabled || !fFloatingOverlaysLock.Lock())
		return;

	if (IsDoubleBuffered())
		Invalidate(_CursorFrame());
	else
		_DrawCursor(_CursorFrame());
	fFloatingOverlaysLock.Unlock();
}


/*!	Whether a drag bitmap is shown with the cursor: the same tests as in
	HWInterface::_AdoptDragBitmap(), which ignores drag bitmaps in other
	color spaces.
*/
bool
AccelerantHWInterface::_DragBitmapShown() const
{
	bool shown = false;
	if (fFloatingOverlaysLock.Lock()) {
		shown = fDragBitmap != NULL
			&& (fDragBitmap->ColorSpace() == B_RGB32
				|| fDragBitmap->ColorSpace() == B_RGBA32)
			&& fDragBitmap->Bounds().Width() > 0
			&& fDragBitmap->Bounds().Height() > 0;
		fFloatingOverlaysLock.Unlock();
	}
	return shown;
}


/*!	Whether \a image is within the limit the accelerant reports
	(B_GET_CURSOR_MAX_SIZE); true if it reports none.
*/
bool
AccelerantHWInterface::_HardwareCursorFits(const ServerCursor* image) const
{
	if (fAccGetCursorMaxSize == NULL)
		return true;

	uint16 maxWidth = 0;
	uint16 maxHeight = 0;
	if (fAccGetCursorMaxSize(image->ColorSpace(), &maxWidth, &maxHeight)
			!= B_OK)
		return false;
	return image->Width() <= maxWidth && image->Height() <= maxHeight;
}


// #pragma mark - Renga (F-59): hardware cursor state and test hooks


namespace {

/*!	A stand-in for an accelerant's hardware cursor (AS_TEST_HARDWARE_CURSOR,
	harness images only): it keeps the last image and position it was given
	and draws nothing, the way app_server sees a real one. Its state is
	only touched under the HWInterface's exclusive lock.
*/
struct TestHardwareCursor {
	uint16		maxWidth;
	uint16		maxHeight;
	uint16		width;
	uint16		height;
	uint16		hotX;
	uint16		hotY;
	color_space	space;
	uint8*		bits;
	uint16		x;
	uint16		y;
	bool		shown;
	int32		setCalls;
	int32		refused;
	int32		sizeQueries;
};

TestHardwareCursor sTestCursor;


status_t
test_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	color_space space, uint16 bytesPerRow, const uint8* bits)
{
	sTestCursor.setCalls++;
	if (width > sTestCursor.maxWidth || height > sTestCursor.maxHeight
		|| (space != B_RGBA32 && space != B_RGB32) || bits == NULL
		|| bytesPerRow < width * 4) {
		sTestCursor.refused++;
		return B_BAD_VALUE;
	}

	uint8* copy = (uint8*)malloc((size_t)width * height * 4);
	if (copy == NULL) {
		sTestCursor.refused++;
		return B_NO_MEMORY;
	}
	for (uint32 y = 0; y < height; y++)
		memcpy(copy + y * width * 4, bits + y * bytesPerRow, width * 4);

	free(sTestCursor.bits);
	sTestCursor.bits = copy;
	sTestCursor.width = width;
	sTestCursor.height = height;
	sTestCursor.hotX = hotX;
	sTestCursor.hotY = hotY;
	sTestCursor.space = space;
	return B_OK;
}


void
test_move_cursor(uint16 x, uint16 y)
{
	sTestCursor.x = x;
	sTestCursor.y = y;
}


void
test_show_cursor(bool show)
{
	sTestCursor.shown = show;
}


status_t
test_get_cursor_max_size(color_space space, uint16* maxWidth,
	uint16* maxHeight)
{
	sTestCursor.sizeQueries++;
	if (space != B_RGBA32 && space != B_RGB32)
		return B_BAD_VALUE;
	*maxWidth = sTestCursor.maxWidth;
	*maxHeight = sTestCursor.maxHeight;
	return B_OK;
}

}	// namespace


status_t
AccelerantHWInterface::SetTestHardwareCursor(bool enable, uint16 maxWidth,
	uint16 maxHeight, bool reportMaxSize)
{
	if (access("/boot/system/non-packaged/data/ce-harness", F_OK) != 0)
		return B_NOT_ALLOWED;
	if (!LockExclusiveAccess())
		return B_ERROR;

	if (enable && !fTestCursorHooks) {
		fSavedSetCursorShape = fAccSetCursorShape;
		fSavedSetCursorBitmap = fAccSetCursorBitmap;
		fSavedMoveCursor = fAccMoveCursor;
		fSavedShowCursor = fAccShowCursor;
		fSavedGetCursorMaxSize = fAccGetCursorMaxSize;
		if (fHardwareCursorEnabled && fAccShowCursor != NULL)
			fAccShowCursor(false);
		fHardwareCursorEnabled = false;
			// a real hardware cursor, if any, is off; the software
			// cursor is erased when the stand-in takes over below
	} else if (!enable && fTestCursorHooks) {
		fAccSetCursorShape = fSavedSetCursorShape;
		fAccSetCursorBitmap = fSavedSetCursorBitmap;
		fAccMoveCursor = fSavedMoveCursor;
		fAccShowCursor = fSavedShowCursor;
		fAccGetCursorMaxSize = fSavedGetCursorMaxSize;
		fTestCursorHooks = false;
		fHardwareCursorEnabled = false;
		free(sTestCursor.bits);
		sTestCursor = TestHardwareCursor();
	}

	if (enable) {
		free(sTestCursor.bits);
		sTestCursor = TestHardwareCursor();
		sTestCursor.maxWidth = maxWidth;
		sTestCursor.maxHeight = maxHeight;
		fAccSetCursorShape = NULL;
		fAccSetCursorBitmap = test_set_cursor_bitmap;
		fAccMoveCursor = test_move_cursor;
		fAccShowCursor = test_show_cursor;
		fAccGetCursorMaxSize = reportMaxSize ? test_get_cursor_max_size : NULL;
		fTestCursorHooks = true;
	}
	fSoftwareCursorFallbacks = 0;

	_UpdateHardwareCursor();
	_ShowSoftwareCursor();
		// if the hardware (or the stand-in) did not take it

	UnlockExclusiveAccess();
	return B_OK;
}


void
AccelerantHWInterface::SetDragBitmapsInHardware(bool enabled)
{
	if (!LockExclusiveAccess())
		return;
	if (fDragBitmapsInHardware != enabled) {
		fDragBitmapsInHardware = enabled;
		_SaveCursorSettings();
		_UpdateHardwareCursor();
		_ShowSoftwareCursor();
	}
	UnlockExclusiveAccess();
}


bool
AccelerantHWInterface::DragBitmapsInHardware() const
{
	return fDragBitmapsInHardware;
}


static status_t
cursor_settings_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("system/app_server/hardware_cursor");
	return status;
}


/*!	~/config/settings/system/app_server/hardware_cursor, a driver settings
	file: "drag_bitmaps false" turns compositing drags into the hardware
	cursor off. Written by SetDragBitmapsInHardware() (the Input
	preferences); absent means the defaults.
*/
void
AccelerantHWInterface::_LoadCursorSettings()
{
	BPath path;
	if (cursor_settings_path(path) != B_OK)
		return;
	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return;
	fDragBitmapsInHardware = get_driver_boolean_parameter(handle,
		"drag_bitmaps", true, true);
	unload_driver_settings(handle);
}


void
AccelerantHWInterface::_SaveCursorSettings() const
{
	BPath path;
	if (cursor_settings_path(path) != B_OK)
		return;
	if (fDragBitmapsInHardware) {
		// the default: no file
		unlink(path.Path());
		return;
	}
	BPath parent;
	if (path.GetParent(&parent) == B_OK)
		create_directory(parent.Path(), 0755);
	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return;
	fprintf(file, "# Written by app_server (the Input preferences)\n"
		"drag_bitmaps false\n");
	fclose(file);
}


status_t
AccelerantHWInterface::GetHardwareCursorInfo(hardware_cursor_info& info,
	int32 sampleX, int32 sampleY)
{
	memset(&info, 0, sizeof(info));
	if (!LockExclusiveAccess())
		return B_ERROR;

	info.hardware = fHardwareCursorEnabled;
	info.test_hooks = fTestCursorHooks;
	info.drag_bitmap = _DragBitmapShown();
	info.drag_in_hardware = fHardwareCursorEnabled && fDragCursorInHardware;
	info.visible = IsCursorVisible();
	info.software_fallbacks = fSoftwareCursorFallbacks;
	info.drag_compositing = fDragBitmapsInHardware;
	if (fHardwareCursorEnabled) {
		info.width = fHardwareCursorWidth;
		info.height = fHardwareCursorHeight;
		info.hot_x = (uint16)fHardwareCursorHotSpot.x;
		info.hot_y = (uint16)fHardwareCursorHotSpot.y;
	}

	if (fTestCursorHooks) {
		info.set_calls = sTestCursor.setCalls;
		info.refused = sTestCursor.refused;
		info.size_queries = sTestCursor.sizeQueries;
		info.position_x = sTestCursor.x;
		info.position_y = sTestCursor.y;
		info.visible = sTestCursor.shown;
		info.color_space = sTestCursor.space;
		// what the stand-in "hardware" holds, not what app_server thinks
		info.width = sTestCursor.width;
		info.height = sTestCursor.height;
		info.hot_x = sTestCursor.hotX;
		info.hot_y = sTestCursor.hotY;
		if (sTestCursor.bits != NULL && sampleX >= 0 && sampleY >= 0
			&& sampleX < sTestCursor.width && sampleY < sTestCursor.height) {
			memcpy(&info.sample, sTestCursor.bits
				+ (sampleY * sTestCursor.width + sampleX) * 4, 4);
		}
	} else if (fHardwareCursorImage.IsSet() && sampleX >= 0 && sampleY >= 0
		&& sampleX < (int32)fHardwareCursorImage->Width()
		&& sampleY < (int32)fHardwareCursorImage->Height()) {
		info.color_space = fHardwareCursorImage->ColorSpace();
		memcpy(&info.sample, fHardwareCursorImage->Bits()
			+ sampleY * fHardwareCursorImage->BytesPerRow() + sampleX * 4, 4);
	}

	UnlockExclusiveAccess();
	return B_OK;
}


// #pragma mark - buffer access


RenderingBuffer*
AccelerantHWInterface::FrontBuffer() const
{
	return fFrontBuffer.Get();
}


RenderingBuffer*
AccelerantHWInterface::BackBuffer() const
{
	return fBackBuffer.Get();
}


bool
AccelerantHWInterface::IsDoubleBuffered() const
{
	return fBackBuffer.IsSet();
}


void
AccelerantHWInterface::_CopyBackToFront(/*const*/ BRegion& region)
{
	return HWInterface::_CopyBackToFront(region);
}


// #pragma mark -


void
AccelerantHWInterface::_DrawCursor(IntRect area) const
{
	if (!fHardwareCursorEnabled)
		HWInterface::_DrawCursor(area);
}


void
AccelerantHWInterface::_RegionToRectParams(/*const*/ BRegion* region,
	uint32* count) const
{
	*count = region->CountRects();
	// TODO: locking!!
	if (fRectParamsCount < *count) {
		fRectParamsCount = (*count / kDefaultParamsCount + 1)
			* kDefaultParamsCount;
		// NOTE: realloc() could be used instead...
		fill_rect_params* params
			= new (nothrow) fill_rect_params[fRectParamsCount];
		if (params) {
			delete[] fRectParams;
			fRectParams = params;
		} else {
			*count = fRectParamsCount;
		}
	}

	for (uint32 i = 0; i < *count; i++) {
		clipping_rect r = region->RectAtInt(i);
		fRectParams[i].left = (uint16)r.left;
		fRectParams[i].top = (uint16)r.top;
		fRectParams[i].right = (uint16)r.right;
		fRectParams[i].bottom = (uint16)r.bottom;
	}
}


uint32
AccelerantHWInterface::_NativeColor(const rgb_color& color) const
{
	// NOTE: This functions looks somehow suspicious to me.
	// It assumes that all graphics cards have the same native endianness, no?
	switch (fDisplayMode.space) {
		case B_CMAP8:
		case B_GRAY8:
			return RGBColor(color).GetColor8();

		case B_RGB15_BIG:
		case B_RGBA15_BIG:
		case B_RGB15_LITTLE:
		case B_RGBA15_LITTLE:
			return RGBColor(color).GetColor15();

		case B_RGB16_BIG:
		case B_RGB16_LITTLE:
			return RGBColor(color).GetColor16();

		case B_RGB32_BIG:
		case B_RGBA32_BIG:
		case B_RGB32_LITTLE:
		case B_RGBA32_LITTLE: {
			return (uint32)((color.alpha << 24) | (color.red << 16)
				| (color.green << 8) | color.blue);
		}
	}
	return 0;
}


void
AccelerantHWInterface::_SetSystemPalette()
{
	set_indexed_colors setIndexedColors = (set_indexed_colors)fAccelerantHook(
		B_SET_INDEXED_COLORS, NULL);
	if (setIndexedColors == NULL)
		return;

	const rgb_color* palette = SystemPalette();
	uint8 colors[3 * 256];
		// the color table is an array with 3 bytes per color
	uint32 j = 0;

	for (int32 i = 0; i < 256; i++) {
		colors[j++] = palette[i].red;
		colors[j++] = palette[i].green;
		colors[j++] = palette[i].blue;
	}

	setIndexedColors(256, 0, colors, 0);
}


void
AccelerantHWInterface::_SetGrayscalePalette()
{
	set_indexed_colors setIndexedColors = (set_indexed_colors)fAccelerantHook(
		B_SET_INDEXED_COLORS, NULL);
	if (setIndexedColors == NULL)
		return;

	uint8 colors[3 * 256];
		// the color table is an array with 3 bytes per color
	uint32 j = 0;

	if (fFrontBuffer->Width() > fFrontBuffer->BytesPerRow()) {
		// VGA 16 color grayscale planar mode
		for (int32 i = 0; i < 256; i++) {
			colors[j++] = (i & 0xf) * 17;
			colors[j++] = (i & 0xf) * 17;
			colors[j++] = (i & 0xf) * 17;
		}

		setIndexedColors(256, 0, colors, 0);
	} else {
		for (int32 i = 0; i < 256; i++) {
			colors[j++] = i;
			colors[j++] = i;
			colors[j++] = i;
		}

		setIndexedColors(256, 0, colors, 0);
	}
}
