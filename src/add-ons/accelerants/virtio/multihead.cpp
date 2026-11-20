/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	<add-ons/graphics/MultiHead.h> version 2 for virtio-gpu (Renga batch 6).

	Every scanout the host offers (QEMU: virtio-gpu-pci,max_outputs=N) is an
	output; output 0 is scanout 0, which the classic hooks drive. virtio-gpu
	has no shared scan-out engines, so there are as many hardware heads as
	outputs. Mirroring points a scanout at the source scanout's resource
	(VIRTIO_GPU_SET_SCANOUT_SOURCE); the host then shows the same pixels and
	nothing is copied. The host draws no cursor plane here, so the cursor
	hooks report B_UNSUPPORTED and app_server draws the cursor itself.
	Hot-plug: the driver does not watch the host's display-change events
	yet, so event_fd() returns -1.

	This is the reference implementation of the table, and the one the
	harness tests app_server's multi-screen code with (ce-test.py
	--displays 2).
*/


#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <add-ons/graphics/MultiHead.h>
#include <edid.h>

#include "accelerant_protos.h"
#include "accelerant.h"


static const uint32 kMaxOutputs = MAX_SCANOUTS;

static struct {
	bool	enabled;
	uint32	source;		// B_NO_SOURCE_OUTPUT, or the scanout mirrored
} sOutputs[kMaxOutputs];
static bool sOutputsInitialized = false;


static void
init_outputs()
{
	if (sOutputsInitialized)
		return;
	for (uint32 i = 0; i < kMaxOutputs; i++) {
		sOutputs[i].enabled = (gInfo->enabled_heads & (1 << i)) != 0;
		sOutputs[i].source = B_NO_SOURCE_OUTPUT;
	}
	sOutputsInitialized = true;
}


//! Outputs are the scanouts the host enabled at start, in order.
static uint32
mh_count_outputs()
{
	uint32 count = 0;
	for (uint32 i = 0; i < kMaxOutputs; i++) {
		if ((gInfo->enabled_heads & (1 << i)) != 0)
			count = i + 1;
	}
	return count;
}


static bool
valid_output(uint32 output)
{
	return output < kMaxOutputs
		&& (gInfo->enabled_heads & (1 << output)) != 0;
}


static uint32
refresh_millihz(const display_timing& timing)
{
	uint64 total = (uint64)timing.h_total * timing.v_total;
	if (total == 0)
		return 0;
	return (uint32)((uint64)timing.pixel_clock * 1000 * 1000 / total);
}


static status_t
mh_get_output_info(uint32 output, multi_head_info* info)
{
	if (!valid_output(output) || info == NULL)
		return B_BAD_VALUE;
	init_outputs();

	memset(info, 0, sizeof(*info));
	const auto& display = gInfo->shared_info->display_info[output];
	info->flags = B_OUTPUT_CONNECTED
		| (sOutputs[output].enabled ? B_OUTPUT_ENABLED : 0);
	info->connector = B_CONNECTOR_VIRTUAL;
	info->connector_index = output + 1;
	info->preferred = display.current_mode;
	if (display.has_edid) {
		edid1_info edid;
		edid_decode(&edid, &display.edid_raw);
		for (int i = 0; i < EDID1_NUM_DETAILED_MONITOR_DESC; i++) {
			if (edid.detailed_monitor[i].monitor_desc_type
					== EDID1_MONITOR_NAME) {
				strlcpy(info->name, edid.detailed_monitor[i].data.monitor_name,
					sizeof(info->name));
				char* end = strchr(info->name, '\n');
				if (end != NULL)
					*end = '\0';
			}
		}
		strlcpy(info->vendor, edid.vendor.manufacturer, sizeof(info->vendor));
		info->serial = edid.vendor.serial;
		info->width_mm = edid.display.h_size * 10;
		info->height_mm = edid.display.v_size * 10;
		// the host's size for this output, with the list's timing
		const display_mode* modes = gInfo->heads[output].mode_list;
		for (uint32 i = 0; modes != NULL && i < display.mode_count; i++) {
			if (modes[i].virtual_width == display.current_mode.virtual_width
				&& modes[i].virtual_height
					== display.current_mode.virtual_height) {
				info->preferred = modes[i];
				break;
			}
		}
	}
	if (info->name[0] == '\0')
		snprintf(info->name, sizeof(info->name), "Virtual display %" B_PRIu32,
			output + 1);
	uint32 refresh = refresh_millihz(info->preferred.timing);
	info->max_refresh_millihz = refresh;
	info->min_refresh_millihz = 0;
	info->max_bpc = 8;
	info->gamma_size = 0;
	return B_OK;
}


static uint32
mh_mode_count(uint32 output)
{
	if (!valid_output(output))
		return 0;
	return gInfo->shared_info->display_info[output].mode_count;
}


static status_t
mh_get_mode_list(uint32 output, display_mode* modes)
{
	if (!valid_output(output) || modes == NULL
		|| gInfo->heads[output].mode_list == NULL)
		return B_BAD_VALUE;
	memcpy(modes, gInfo->heads[output].mode_list,
		gInfo->shared_info->display_info[output].mode_count
			* sizeof(display_mode));
	return B_OK;
}


static status_t
set_source(uint32 output, uint32 source)
{
	virtio_gpu_set_scanout_source_params params;
	params.scanout = output;
	params.source = source == B_NO_SOURCE_OUTPUT ? VIRTIO_GPU_NO_SOURCE
		: source;
	if (ioctl(gInfo->device, VIRTIO_GPU_SET_SCANOUT_SOURCE, &params,
			sizeof(params)) != 0)
		return B_ERROR;
	return B_OK;
}


static status_t
set_mode(uint32 output, const display_mode* mode)
{
	virtio_gpu_set_display_mode_params params;
	params.scanout = output;
	params.mode = *mode;
	if (ioctl(gInfo->device, VIRTIO_GPU_SET_DISPLAY_MODE, &params,
			sizeof(params)) != 0)
		return B_ERROR;
	return B_OK;
}


static status_t
mh_set_output_mode(uint32 output, const display_mode* mode)
{
	if (!valid_output(output))
		return B_BAD_VALUE;
	init_outputs();

	status_t status;
	if (mode == NULL) {
		status = set_source(output, B_NO_SOURCE_OUTPUT);
		if (status == B_OK)
			sOutputs[output].enabled = false;
		return status;
	}
	status = set_mode(output, mode);
	if (status == B_OK) {
		sOutputs[output].enabled = true;
		sOutputs[output].source = B_NO_SOURCE_OUTPUT;
	}
	return status;
}


static status_t
mh_get_output_mode(uint32 output, display_mode* mode)
{
	if (!valid_output(output) || mode == NULL)
		return B_BAD_VALUE;
	init_outputs();
	if (!sOutputs[output].enabled)
		return B_NOT_ALLOWED;
	*mode = gInfo->shared_info->display_info[output].current_mode;
	return B_OK;
}


static status_t
mh_get_frame_buffer_config(uint32 output, frame_buffer_config* config)
{
	if (!valid_output(output) || config == NULL)
		return B_BAD_VALUE;
	const auto& display = gInfo->shared_info->display_info[output];
	memset(config, 0, sizeof(*config));
	config->frame_buffer = display.frame_buffer;
	config->bytes_per_row = display.bytes_per_row;
	return B_OK;
}


static status_t
mh_set_output_dpms(uint32 output, uint32 dpmsFlags)
{
	return valid_output(output) ? B_OK : B_BAD_VALUE;
}


static status_t
mh_move_output_cursor(uint32 output, int32 x, int32 y)
{
	return B_UNSUPPORTED;
}


static status_t
mh_show_output_cursor(uint32 output, bool visible)
{
	return B_UNSUPPORTED;
}


static status_t
mh_get_output_edid(uint32 output, void* data, size_t* _size)
{
	if (!valid_output(output) || _size == NULL)
		return B_BAD_VALUE;
	const auto& display = gInfo->shared_info->display_info[output];
	if (!display.has_edid)
		return B_ENTRY_NOT_FOUND;
	if (data == NULL || *_size < sizeof(edid1_raw)) {
		*_size = sizeof(edid1_raw);
		return B_BUFFER_OVERFLOW;
	}
	memcpy(data, &display.edid_raw, sizeof(edid1_raw));
	*_size = sizeof(edid1_raw);
	return B_OK;
}


static int
mh_event_fd()
{
	return -1;
}


static uint32
mh_count_hardware_heads()
{
	return mh_count_outputs();
}


/*!	Checks the whole configuration first, then applies it: modes of the
	outputs with their own frame buffer, then the mirrors, then turns the
	rest off. If a step fails, what was changed is put back as far as the
	host allows.
*/
static status_t
mh_set_configuration(const multi_head_config* configs, uint32 count,
	uint32 flags)
{
	init_outputs();
	uint32 outputs = mh_count_outputs();
	if (count > outputs || (count > 0 && configs == NULL))
		return B_BAD_VALUE;

	const multi_head_config* wanted[kMaxOutputs] = {};
	uint32 enabled = 0;
	for (uint32 i = 0; i < count; i++) {
		const multi_head_config& config = configs[i];
		if (!valid_output(config.output) || wanted[config.output] != NULL)
			return B_BAD_VALUE;
		wanted[config.output] = &config;
		if ((config.flags & B_OUTPUT_CONFIG_ENABLED) != 0)
			enabled++;
	}
	if (enabled > mh_count_hardware_heads())
		return B_BUSY;
	if (enabled == 0)
		return B_BAD_VALUE;
		// app_server always needs one output

	for (uint32 i = 0; i < count; i++) {
		const multi_head_config& config = configs[i];
		if ((config.flags & B_OUTPUT_CONFIG_ENABLED) == 0)
			continue;
		if (config.source_output == B_NO_SOURCE_OUTPUT
			|| config.source_output == config.output) {
			if (config.mode.space != B_RGB32 && config.mode.space != B_RGBA32)
				return B_BAD_VALUE;
			if (config.mode.virtual_width == 0
				|| config.mode.virtual_height == 0)
				return B_BAD_VALUE;
			continue;
		}
		// a mirror: its source must have a buffer of its own
		const multi_head_config* source = config.source_output < kMaxOutputs
			? wanted[config.source_output] : NULL;
		if (source == NULL
			|| (source->flags & B_OUTPUT_CONFIG_ENABLED) == 0
			|| (source->source_output != B_NO_SOURCE_OUTPUT
				&& source->source_output != source->output))
			return B_BAD_VALUE;
	}

	if ((flags & B_MULTI_HEAD_TEST_ONLY) != 0)
		return B_OK;

	// remember the current state to put it back on failure
	struct { bool enabled; uint32 source; display_mode mode; } saved[kMaxOutputs];
	for (uint32 i = 0; i < outputs; i++) {
		saved[i].enabled = sOutputs[i].enabled;
		saved[i].source = sOutputs[i].source;
		if (valid_output(i))
			saved[i].mode = gInfo->shared_info->display_info[i].current_mode;
	}

	status_t status = B_OK;
	for (uint32 i = 0; i < outputs && status == B_OK; i++) {
		const multi_head_config* config = wanted[i];
		if (config == NULL || (config->flags & B_OUTPUT_CONFIG_ENABLED) == 0
			|| (config->source_output != B_NO_SOURCE_OUTPUT
				&& config->source_output != i))
			continue;
		status = set_mode(i, &config->mode);
		if (status == B_OK) {
			sOutputs[i].enabled = true;
			sOutputs[i].source = B_NO_SOURCE_OUTPUT;
		}
	}
	for (uint32 i = 0; i < outputs && status == B_OK; i++) {
		const multi_head_config* config = wanted[i];
		if (config == NULL || (config->flags & B_OUTPUT_CONFIG_ENABLED) == 0
			|| config->source_output == B_NO_SOURCE_OUTPUT
			|| config->source_output == i)
			continue;
		status = set_source(i, config->source_output);
		if (status == B_OK) {
			sOutputs[i].enabled = true;
			sOutputs[i].source = config->source_output;
		}
	}
	for (uint32 i = 0; i < outputs && status == B_OK; i++) {
		const multi_head_config* config = wanted[i];
		if (!valid_output(i) || (config != NULL
				&& (config->flags & B_OUTPUT_CONFIG_ENABLED) != 0)
			|| !sOutputs[i].enabled)
			continue;
		status = set_source(i, B_NO_SOURCE_OUTPUT);
		if (status == B_OK)
			sOutputs[i].enabled = false;
	}

	if (status != B_OK) {
		for (uint32 i = 0; i < outputs; i++) {
			if (!valid_output(i))
				continue;
			if (!saved[i].enabled)
				set_source(i, B_NO_SOURCE_OUTPUT);
			else if (saved[i].source != B_NO_SOURCE_OUTPUT)
				set_source(i, saved[i].source);
			else
				set_mode(i, &saved[i].mode);
			sOutputs[i].enabled = saved[i].enabled;
			sOutputs[i].source = saved[i].source;
		}
	}
	return status;
}


static multi_head_hooks sMultiHeadHooks = {
	B_MULTI_HEAD_VERSION,
	mh_count_outputs,
	mh_get_output_info,
	mh_mode_count,
	mh_get_mode_list,
	mh_set_output_mode,
	mh_get_output_mode,
	mh_get_frame_buffer_config,
	mh_set_output_dpms,
	mh_move_output_cursor,
	mh_show_output_cursor,
	mh_get_output_edid,
	mh_event_fd,
	mh_count_hardware_heads,
	mh_set_configuration
};


extern "C" void*
virtio_gpu_multi_head_hooks()
{
	return &sMultiHeadHooks;
}
