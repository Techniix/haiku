/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "MultiOutput.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <driver_settings.h>
#include <OS.h>


static const char* const kSettingsPath
	= "/boot/home/config/settings/system/app_server/displays";


const char*
MultiOutput::SettingsPath()
{
	return kSettingsPath;
}


/*!	Returns the helper if the accelerant offers <MultiHead.h> version 2 or
	later with more than one connected output and the settings do not turn
	multi-screen off; NULL otherwise (app_server then drives one screen
	through the classic hooks, as always).
*/
MultiOutput*
MultiOutput::Create(GetAccelerantHook hook)
{
	if (hook == NULL)
		return NULL;
	multi_head_hooks* hooks
		= (multi_head_hooks*)hook(B_MULTI_HEAD_HOOKS, NULL);
	if (hooks == NULL || hooks->version < 2
		|| hooks->count_outputs == NULL || hooks->set_configuration == NULL)
		return NULL;

	void* settings = load_driver_settings(kSettingsPath);
	bool enabled = settings == NULL || get_driver_boolean_parameter(settings,
		"multi_screen", true, true);
	unload_driver_settings(settings);
	if (!enabled) {
		syslog(LOG_INFO, "app_server: multi-screen is off in %s",
			kSettingsPath);
		return NULL;
	}

	MultiOutput* multi = new(std::nothrow) MultiOutput(hooks);
	if (multi == NULL)
		return NULL;
	int32 connected = 0;
	for (int32 i = 0; i < multi->fCount; i++) {
		if ((multi->fOutputs[i].info.flags & B_OUTPUT_CONNECTED) != 0)
			connected++;
	}
	if (connected < 2) {
		delete multi;
		return NULL;
	}
	return multi;
}


MultiOutput::MultiOutput(multi_head_hooks* hooks)
	:
	fHooks(hooks),
	fCount(0),
	fDesktop(NULL),
	fDesktopArea(-1),
	fDesktopBytesPerRow(0),
	fCommitted(false)
{
	_Load();
}


MultiOutput::~MultiOutput()
{
	if (fDesktopArea >= 0)
		delete_area(fDesktopArea);
}


/*!	Reads the displays settings:

		multi_screen true
		output 1 {
			enabled true		# false: off
			primary false		# the output Screen preferences' mode is for
			position 1024 0		# top left in the arrangement
			mode 1280 1024		# from the output's mode list
			mirror 0			# show output 0's picture instead
		}

	Missing values keep the defaults: connected outputs on, preferred mode,
	left to right.
*/
void
MultiOutput::_Load()
{
	// what the accelerant reports now (also after a hot-plug)
	fCount = 0;
	uint32 count = fHooks->count_outputs();
	if (count > 16)
		count = 16;
	for (uint32 i = 0; i < count; i++) {
		Output& output = fOutputs[fCount];
		frame_buffer_config buffer = output.buffer;
		output = Output();
		output.buffer = buffer;
		output.index = i;
		output.mirrorOf = B_NO_SOURCE_OUTPUT;
		if (fHooks->get_output_info(i, &output.info) != B_OK)
			continue;
		output.enabled = (output.info.flags & B_OUTPUT_CONNECTED) != 0;
		output.mode = output.info.preferred;
		output.frame = BRect(-1, -1, -2, -2);
			// "no position": _Arrange() places it
		fCount++;
	}
	if (fCount > 0)
		fOutputs[0].primary = true;

	void* handle = load_driver_settings(kSettingsPath);
	const driver_settings* settings = handle != NULL
		? get_driver_settings(handle) : NULL;
	for (int32 p = 0; settings != NULL && p < settings->parameter_count;
			p++) {
		const driver_parameter& parameter = settings->parameters[p];
		if (strcmp(parameter.name, "output") != 0 || parameter.value_count < 1)
			continue;
		uint32 index = strtoul(parameter.values[0], NULL, 10);
		Output* output = NULL;
		for (int32 i = 0; i < fCount; i++) {
			if (fOutputs[i].index == index)
				output = &fOutputs[i];
		}
		if (output == NULL)
			continue;
		for (int32 j = 0; j < parameter.parameter_count; j++) {
			const driver_parameter& item = parameter.parameters[j];
			const char* value = item.value_count > 0 ? item.values[0] : "";
			if (strcmp(item.name, "enabled") == 0) {
				output->enabled = strcmp(value, "false") != 0
					&& strcmp(value, "off") != 0;
			} else if (strcmp(item.name, "primary") == 0) {
				if (strcmp(value, "true") == 0 || strcmp(value, "on") == 0) {
					for (int32 i = 0; i < fCount; i++)
						fOutputs[i].primary = false;
					output->primary = true;
				}
			} else if (strcmp(item.name, "position") == 0
				&& item.value_count >= 2) {
				float x = atoi(item.values[0]);
				float y = atoi(item.values[1]);
				output->frame = BRect(x, y, x, y);
			} else if (strcmp(item.name, "mirror") == 0) {
				output->mirrorOf = strtoul(value, NULL, 10);
			} else if (strcmp(item.name, "mode") == 0
				&& item.value_count >= 2) {
				uint32 width = strtoul(item.values[0], NULL, 10);
				uint32 height = strtoul(item.values[1], NULL, 10);
				uint32 count = fHooks->mode_count(output->index);
				display_mode* modes = new(std::nothrow) display_mode[count];
				if (modes != NULL
					&& fHooks->get_mode_list(output->index, modes) == B_OK) {
					for (uint32 m = 0; m < count; m++) {
						if (modes[m].virtual_width == width
							&& modes[m].virtual_height == height) {
							output->mode = modes[m];
							break;
						}
					}
				}
				delete[] modes;
			}
		}
	}
	unload_driver_settings(handle);

	// the primary must be an enabled output with a picture of its own
	bool havePrimary = false;
	for (int32 i = 0; i < fCount; i++) {
		Output& output = fOutputs[i];
		if (output.mirrorOf == output.index)
			output.mirrorOf = B_NO_SOURCE_OUTPUT;
		if (output.primary && (!output.enabled
				|| output.mirrorOf != B_NO_SOURCE_OUTPUT))
			output.primary = false;
		havePrimary |= output.primary;
	}
	for (int32 i = 0; !havePrimary && i < fCount; i++) {
		if (fOutputs[i].enabled
			&& fOutputs[i].mirrorOf == B_NO_SOURCE_OUTPUT) {
			fOutputs[i].primary = true;
			havePrimary = true;
		}
	}
}


//!	Gives every output its frame; the desktop starts at 0, 0.
void
MultiOutput::_Arrange()
{
	float nextX = 0;
	for (int32 i = 0; i < fCount; i++) {
		Output& output = fOutputs[i];
		if (!output.enabled || output.mirrorOf != B_NO_SOURCE_OUTPUT)
			continue;
		float width = output.mode.virtual_width;
		float height = output.mode.virtual_height;
		if (!output.frame.IsValid() && output.frame.left == -1)
			output.frame.Set(nextX, 0, nextX, 0);
		output.frame.right = output.frame.left + width - 1;
		output.frame.bottom = output.frame.top + height - 1;
		if (output.frame.right + 1 > nextX)
			nextX = output.frame.right + 1;
	}

	BRect bounds;
	bool first = true;
	for (int32 i = 0; i < fCount; i++) {
		const Output& output = fOutputs[i];
		if (!output.enabled || output.mirrorOf != B_NO_SOURCE_OUTPUT)
			continue;
		bounds = first ? output.frame : (bounds | output.frame);
		first = false;
	}
	BPoint offset = bounds.LeftTop();
	for (int32 i = 0; i < fCount; i++) {
		Output& output = fOutputs[i];
		if (output.enabled && output.mirrorOf == B_NO_SOURCE_OUTPUT)
			output.frame.OffsetBy(-offset);
	}
	// mirrors show their source's place
	for (int32 i = 0; i < fCount; i++) {
		Output& output = fOutputs[i];
		if (!output.enabled || output.mirrorOf == B_NO_SOURCE_OUTPUT)
			continue;
		for (int32 j = 0; j < fCount; j++) {
			if (fOutputs[j].index == output.mirrorOf)
				output.frame = fOutputs[j].frame;
		}
	}
	fDesktopFrame = bounds.OffsetToCopy(0, 0);
}


status_t
MultiOutput::Apply(const display_mode* primaryMode, display_mode& desktopMode,
	frame_buffer_config& desktopBuffer)
{
	_Load();
	if (primaryMode != NULL) {
		for (int32 i = 0; i < fCount; i++) {
			Output& output = fOutputs[i];
			if (!output.primary)
				continue;
			uint32 count = fHooks->mode_count(output.index);
			display_mode* modes = new(std::nothrow) display_mode[count];
			if (modes != NULL
				&& fHooks->get_mode_list(output.index, modes) == B_OK) {
				for (uint32 m = 0; m < count; m++) {
					if (modes[m].virtual_width == primaryMode->virtual_width
						&& modes[m].virtual_height
							== primaryMode->virtual_height) {
						output.mode = modes[m];
						break;
					}
				}
			}
			delete[] modes;
		}
	}

	_Arrange();

	multi_head_config configs[16];
	int32 configCount = 0;
	for (int32 i = 0; i < fCount; i++) {
		const Output& output = fOutputs[i];
		multi_head_config& config = configs[configCount++];
		memset(&config, 0, sizeof(config));
		config.output = output.index;
		config.flags = output.enabled ? B_OUTPUT_CONFIG_ENABLED : 0;
		config.source_output = output.mirrorOf;
		config.mode = output.mode;
	}
	fCommitted = false;
	status_t status = fHooks->set_configuration(configs, configCount,
		B_MULTI_HEAD_TEST_ONLY);
	if (status == B_OK)
		status = fHooks->set_configuration(configs, configCount, 0);
	if (status != B_OK) {
		syslog(LOG_ERR, "app_server: the displays arrangement was refused "
			"(%s)", strerror(status));
		return status;
	}

	// Committed: every output has a new frame buffer now (Quasar's answer
	// to R-19, 2026-11-20), so the old addresses must not be written to
	// again, even if the rest of this fails.
	fCommitted = true;
	for (int32 i = 0; i < fCount; i++)
		fOutputs[i].buffer.frame_buffer = NULL;

	for (int32 i = 0; i < fCount; i++) {
		Output& output = fOutputs[i];
		if (!output.enabled || output.mirrorOf != B_NO_SOURCE_OUTPUT)
			continue;
		fHooks->get_output_mode(output.index, &output.mode);
		if (fHooks->get_frame_buffer_config(output.index, &output.buffer)
				!= B_OK)
			return B_ERROR;
	}

	uint32 width = fDesktopFrame.IntegerWidth() + 1;
	uint32 height = fDesktopFrame.IntegerHeight() + 1;
	size_t size = ((size_t)width * height * 4 + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	if (fDesktopArea >= 0)
		delete_area(fDesktopArea);
	fDesktopArea = create_area("desktop of all outputs", (void**)&fDesktop,
		B_ANY_ADDRESS, size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA
			| B_CLONEABLE_AREA);
	if (fDesktopArea < 0) {
		fDesktop = NULL;
		return fDesktopArea;
	}
	fDesktopBytesPerRow = width * 4;
	memset(fDesktop, 0, size);

	desktopMode = fOutputs[0].mode;
	for (int32 i = 0; i < fCount; i++) {
		if (fOutputs[i].primary)
			desktopMode = fOutputs[i].mode;
	}
	desktopMode.virtual_width = width;
	desktopMode.virtual_height = height;
	desktopMode.timing.h_display = width;
	desktopMode.timing.v_display = height;
	desktopMode.h_display_start = 0;
	desktopMode.v_display_start = 0;
	desktopMode.space = B_RGB32;

	memset(&desktopBuffer, 0, sizeof(desktopBuffer));
	desktopBuffer.frame_buffer = fDesktop;
	desktopBuffer.bytes_per_row = fDesktopBytesPerRow;

	for (int32 i = 0; i < fCount; i++) {
		const Output& output = fOutputs[i];
		syslog(LOG_INFO, "app_server: output %" B_PRIu32 " (%s): %s%s at "
			"%d,%d, %dx%d%s", output.index, output.info.name,
			output.enabled ? "on" : "off",
			output.mirrorOf != B_NO_SOURCE_OUTPUT ? ", mirroring" : "",
			(int)output.frame.left, (int)output.frame.top,
			output.mode.virtual_width, output.mode.virtual_height,
			output.primary ? ", primary" : "");
	}
	return B_OK;
}


void
MultiOutput::Push(const BRect& rect) const
{
	if (fDesktop == NULL)
		return;
	for (int32 i = 0; i < fCount; i++) {
		const Output& output = fOutputs[i];
		if (!output.enabled || output.mirrorOf != B_NO_SOURCE_OUTPUT
			|| output.buffer.frame_buffer == NULL)
			continue;
		BRect area = rect & output.frame & fDesktopFrame;
		if (!area.IsValid())
			continue;
		int32 left = (int32)area.left;
		int32 top = (int32)area.top;
		int32 bytes = (area.IntegerWidth() + 1) * 4;
		int32 localX = left - (int32)output.frame.left;
		int32 localY = top - (int32)output.frame.top;
		const uint8* source = fDesktop + top * fDesktopBytesPerRow + left * 4;
		uint8* target = (uint8*)output.buffer.frame_buffer
			+ localY * output.buffer.bytes_per_row + localX * 4;
		for (int32 y = top; y <= (int32)area.bottom; y++) {
			memcpy(target, source, bytes);
			source += fDesktopBytesPerRow;
			target += output.buffer.bytes_per_row;
		}
	}
}


void
MultiOutput::Push(const BRegion& region) const
{
	for (int32 i = 0; i < region.CountRects(); i++)
		Push(region.RectAt(i));
}


BRect
MultiOutput::PrimaryFrame() const
{
	for (int32 i = 0; i < fCount; i++) {
		if (fOutputs[i].primary)
			return fOutputs[i].frame;
	}
	return fDesktopFrame;
}


//!	The frame of the output with \a point, or the primary output's.
BRect
MultiOutput::OutputFrameAt(BPoint point) const
{
	for (int32 i = 0; i < fCount; i++) {
		const Output& output = fOutputs[i];
		if (output.enabled && output.mirrorOf == B_NO_SOURCE_OUTPUT
			&& output.frame.Contains(point))
			return output.frame;
	}
	return PrimaryFrame();
}
