/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: absolute pointing devices ("mouse integration" in virtual
	machines). The pointer has to land exactly where the host puts it.

	The harness attaches absolute devices to the VM (ce-test.py
	--pointer-devices; by default a USB tablet on an xHCI controller and a
	virtio tablet) next to QEMU's PS/2 mouse. For every device found, the test
	asks the host to move the pointer ("CE-HOST pointer <device> <x> <y>",
	device coordinates 0..32767, QMP input-send-event) and reads the pointer
	position app_server reports (get_mouse()).

	The device coordinates are the ones QEMU's display (GTK, SDL, VNC) sends
	when the host pointer is over guest pixel px of a screen W pixels wide:
	px * 32767 / W, rounded down (qemu_input_queue_abs() scales 0..W to
	0..32767). The pointer has to land exactly on px again:

	  <dev>.driver           the kernel published the device
	  virtio.abs_range       the virtio driver reports the axis ranges of
	                         QEMU's tablet, 0..32767
	  <dev>.input_server     input_server runs a pointing device for it
	  <dev>.pos.<name>       the pointer lands exactly on the pixel the host
	                         pointer is on, for the corners, the center and
	                         points in between
	  <dev>.device_min/max   device coordinates 0 and 32767 put it on the
	                         first and last pixel
	  <dev>.stays            a second later it is still there
	  <dev>.relative_after   a PS/2 (relative) move afterwards continues from
	                         the tablet's position ("CE-HOST pointer_rel")
	  <dev>.absolute_after   and the next tablet position is exact again
	                         (the harness then saves the screen as
	                         screen-pointer_<dev>.png)
	  <dev>.mode.<name>      after a screen mode change (800x600, if the
	                         graphics driver offers it) the same holds for the
	                         new size

	Without app_server and input_server (no desktop) the test prints an INFO
	line and reports nothing. Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Input.h>
#include <InterfaceDefs.h>
#include <List.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>

#include <virtio_input_driver.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "pointer"

static const int32 kAbsMax = 32767;
	// QEMU's absolute axis range (INPUT_EVENT_ABS_MAX), for every device

static int sFailures = 0;


struct PointerDevice {
	const char*	id;				// as in "CE-HOST pointer <id> ..."
	const char*	directory;		// where the kernel driver publishes it
	const char*	inputName;		// input_server device name prefix
};

static const PointerDevice kDevices[] = {
	{ "usb", "/dev/input/tablet/usb", "USB Tablet" },
	{ "virtio", "/dev/input/virtio", "VirtIO Tablet" },
};

//! A screen position, as a fraction of the way to the last pixel.
struct Position {
	const char*	name;
	float		x, y;
};

// Consecutive positions differ on both axes, so a pointer that does not move
// never passes by accident.
static const Position kPositions[] = {
	{ "top_left", 0, 0 },
	{ "bottom_right", 1, 1 },
	{ "top_right", 1, 0 },
	{ "bottom_left", 0, 1 },
	{ "center", 0.5, 0.5 },
	{ "quarter", 0.25, 0.75 },
	{ "right_half", 0.8, 0.6 },
	{ "near_edge", 0.97, 0.03 },
};


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static bool
has_entries(const char* path)
{
	DIR* dir = opendir(path);
	if (dir == NULL)
		return false;
	bool found = false;
	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
			found = true;
			break;
		}
	}
	closedir(dir);
	return found;
}


//! The running input_server pointing device whose name starts with \a prefix.
static bool
find_running_device(const char* prefix, BString& name)
{
	BList list;
	if (get_input_devices(&list) != B_OK)
		return false;
	bool found = false;
	for (int32 i = 0; i < list.CountItems(); i++) {
		BInputDevice* device = (BInputDevice*)list.ItemAt(i);
		if (!found && device->Type() == B_POINTING_DEVICE
			&& strncmp(device->Name(), prefix, strlen(prefix)) == 0
			&& device->IsRunning()) {
			name = device->Name();
			found = true;
		}
		delete device;
	}
	return found;
}


static BPoint
pointer()
{
	BPoint where(-1, -1);
	uint32 buttons;
	if (get_mouse(&where, &buttons) != B_OK)
		return BPoint(-1, -1);
	return where;
}


//! The pixel of \a frame at \a position.
static BPoint
pixel(const Position& position, BRect frame)
{
	return BPoint(frame.left + roundf(position.x * frame.Width()),
		frame.top + roundf(position.y * frame.Height()));
}


/*!	The device coordinate QEMU's display sends for pixel \a offset of a
	screen \a size pixels wide or high: qemu_input_scale_axis(offset, 0,
	size, 0, 32767).
*/
static int32
device_coordinate(float offset, float size)
{
	return (int64)offset * kAbsMax / (int64)size;
}


static bool
near(BPoint a, BPoint b, float tolerance)
{
	return fabsf(a.x - b.x) <= tolerance && fabsf(a.y - b.y) <= tolerance;
}


//! Waits up to \a seconds for the pointer to come within \a tolerance.
static BPoint
wait_for_pointer(BPoint want, float tolerance, float seconds)
{
	BPoint where = pointer();
	for (int i = 0; i < seconds * 10 && !near(where, want, tolerance); i++) {
		snooze(100000);
		where = pointer();
	}
	return where;
}


static void
move_absolute(const PointerDevice& device, int32 x, int32 y)
{
	printf("CE-HOST pointer %s %" B_PRId32 " %" B_PRId32 "\n", device.id, x, y);
	fflush(stdout);
}


static void
move_relative(int32 dx, int32 dy)
{
	printf("CE-HOST pointer_rel %" B_PRId32 " %" B_PRId32 "\n", dx, dy);
	fflush(stdout);
}


//! Sends device coordinates \a x, \a y and checks the pointer lands on \a want.
static bool
check_coordinates(const PointerDevice& device, const char* name, int32 x,
	int32 y, BPoint want, BRect frame)
{
	move_absolute(device, x, y);
	BPoint where = wait_for_pointer(want, 0, 5);
	bool pass = near(where, want, 0);
	BString fullName;
	fullName.SetToFormat("%s.%s", device.id, name);
	report(fullName.String(), pass,
		"device %" B_PRId32 ",%" B_PRId32 " -> pointer %.0f,%.0f (want "
		"%.0f,%.0f, screen %.0fx%.0f)", x, y, where.x, where.y, want.x,
		want.y, frame.Width() + 1, frame.Height() + 1);
	return pass;
}


//! Puts the host pointer on \a position and checks the guest's pointer.
static bool
check_position(const PointerDevice& device, const char* prefix,
	const Position& position, BRect frame)
{
	BPoint want = pixel(position, frame);
	BString name;
	name.SetToFormat("%s.%s", prefix, position.name);
	return check_coordinates(device, name.String(),
		device_coordinate(want.x - frame.left, frame.Width() + 1),
		device_coordinate(want.y - frame.top, frame.Height() + 1), want, frame);
}


//! The ranges the virtio tablet's driver reports (virtioInputGetAbsRange).
static void
test_virtio_range()
{
	DIR* dir = opendir("/dev/input/virtio");
	if (dir == NULL)
		return;
	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		BString path;
		path.SetToFormat("/dev/input/virtio/%s/raw", entry->d_name);
		int fd = open(path.String(), O_RDWR);
		if (fd < 0)
			continue;
		VirtioInputType type = kVirtioInputUnknown;
		VirtioInputAbsRange range = { -1, -1, -1, -1 };
		status_t status = B_ERROR;
		if (ioctl(fd, virtioInputGetType, &type, sizeof(type)) == 0
			&& type == kVirtioInputTablet) {
			status = ioctl(fd, virtioInputGetAbsRange, &range, sizeof(range))
				== 0 ? B_OK : errno;
		}
		close(fd);
		if (type != kVirtioInputTablet)
			continue;
		report("virtio.abs_range", status == B_OK && range.xMin == 0
				&& range.xMax == kAbsMax && range.yMin == 0
				&& range.yMax == kAbsMax,
			"%s: %s, x %" B_PRId32 "..%" B_PRId32 ", y %" B_PRId32 "..%"
			B_PRId32 " (want 0..%" B_PRId32 ")", path.String(),
			strerror(status), range.xMin, range.xMax, range.yMin, range.yMax,
			kAbsMax);
		break;
	}
	closedir(dir);
}


static void
test_device(const PointerDevice& device, BRect frame)
{
	BString name;
	name.SetToFormat("%s.driver", device.id);
	report(name.String(), true, "%s", device.directory);
	if (strcmp(device.id, "virtio") == 0)
		test_virtio_range();

	BString inputName;
	bool running = false;
	for (int i = 0; i < 100 && !running; i++) {
		running = find_running_device(device.inputName, inputName);
		if (!running)
			snooze(100000);
	}
	name.SetToFormat("%s.input_server", device.id);
	report(name.String(), running, "%s", running ? inputName.String()
		: "no running input_server pointing device for it");

	for (size_t i = 0; i < B_COUNT_OF(kPositions); i++)
		check_position(device, "pos", kPositions[i], frame);

	BPoint last = pixel(kPositions[B_COUNT_OF(kPositions) - 1], frame);
	snooze(1000000);
	BPoint where = pointer();
	name.SetToFormat("%s.stays", device.id);
	report(name.String(), near(where, last, 0), "pointer %.0f,%.0f a second "
		"later (want %.0f,%.0f)", where.x, where.y, last.x, last.y);

	// The ends of the device's range are the first and last pixel.
	check_coordinates(device, "device_max", kAbsMax, kAbsMax,
		frame.RightBottom(), frame);
	check_coordinates(device, "device_min", 0, 0, frame.LeftTop(), frame);

	// A relative mouse next to the tablet moves on from where the tablet
	// left the pointer: one pointer, not two positions fighting.
	BPoint start = frame.LeftTop() + BPoint(roundf(frame.Width() / 2),
		roundf(frame.Height() / 2));
	move_absolute(device, device_coordinate(start.x - frame.left,
		frame.Width() + 1), device_coordinate(start.y - frame.top,
		frame.Height() + 1));
	BPoint before = wait_for_pointer(start, 0, 5);
	move_relative(40, 30);
	where = before;
	for (int i = 0; i < 50 && where == before; i++) {
		snooze(100000);
		where = pointer();
	}
	snooze(300000);
	where = pointer();
	bool continues = near(before, start, 0) && where.x > before.x
		&& where.y > before.y && near(where, before, 450)
		&& where.x < frame.right && where.y < frame.bottom;
	name.SetToFormat("%s.relative_after", device.id);
	report(name.String(), continues, "tablet %.0f,%.0f, after PS/2 move "
		"+40,+30: %.0f,%.0f", before.x, before.y, where.x, where.y);

	const Position again = { "again", 0.125, 0.875 };
	BPoint want = pixel(again, frame);
	check_coordinates(device, "absolute_after",
		device_coordinate(want.x - frame.left, frame.Width() + 1),
		device_coordinate(want.y - frame.top, frame.Height() + 1), want, frame);

	// For a look at it: screen-pointer_<device>.png in the results.
	printf("CE-HOST screenshot pointer_%s\n", device.id);
	fflush(stdout);
}


//! Switches to a \a width x \a height mode of the current depth, if there.
static bool
find_mode(BScreen& screen, uint16 width, uint16 height, display_mode& mode)
{
	display_mode current;
	display_mode* modes;
	uint32 count;
	if (screen.GetMode(&current) != B_OK
		|| screen.GetModeList(&modes, &count) != B_OK)
		return false;
	bool found = false;
	for (uint32 i = 0; i < count && !found; i++) {
		if (modes[i].virtual_width == width
			&& modes[i].virtual_height == height
			&& modes[i].space == current.space) {
			mode = modes[i];
			found = true;
		}
	}
	free(modes);
	return found;
}


static void
test_mode_change(const PointerDevice& device)
{
	BScreen screen;
	display_mode original;
	display_mode small;
	if (screen.GetMode(&original) != B_OK
		|| !find_mode(screen, 800, 600, small)) {
		printf("CE-INFO pointer: no 800x600 mode, mode change not tested\n");
		return;
	}
	if (original.virtual_width == 800 && original.virtual_height == 600) {
		printf("CE-INFO pointer: already 800x600, mode change not tested\n");
		return;
	}
	status_t status = screen.SetMode(&small);
	if (status != B_OK) {
		printf("CE-INFO pointer: setting 800x600 failed (%s), mode change "
			"not tested\n", strerror(status));
		return;
	}
	// input_server learns about the new frame from app_server.
	snooze(2000000);
	BRect frame = screen.Frame();
	for (size_t i = 0; i < 4; i++)
		check_position(device, "mode", kPositions[i], frame);
	screen.SetMode(&original);
	snooze(2000000);
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	// Pointer positions need app_server and input_server. An image without
	// them skips the suite; otherwise wait for input_server, which can come
	// up a minute or more after the runner starts on a full image without
	// KVM. Tracker is not needed (a fresh Renga image shows FirstBootPrompt
	// instead).
	if (access("/boot/system/servers/app_server", X_OK) != 0
		|| access("/boot/system/servers/input_server", X_OK) != 0) {
		printf("CE-INFO pointer: no desktop (app_server/input_server), "
			"skipped\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}
	bool desktop = false;
	for (int i = 0; i < 300 && !desktop; i++) {
		desktop = be_roster->IsRunning("application/x-vnd.Be-input_server");
		if (!desktop) {
			// Some output now and then, or the harness takes the silence
			// for a hang (--stall).
			if (i > 0 && i % 30 == 0) {
				printf("CE-INFO pointer: waiting for input_server (%d s)\n",
					i);
				fflush(stdout);
			}
			snooze(1000000);
		}
	}
	if (!desktop) {
		report("desktop", false, "input_server not running after 300 s");
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	status_t status = B_ERROR;
	BApplication* app = new BApplication(
		"application/x-vnd.Renga-ce-pointer", &status);
	if (status != B_OK) {
		report("desktop", false, "no BApplication: %s", strerror(status));
	} else {
		BRect frame = BScreen().Frame();
		const PointerDevice* first = NULL;
		int tested = 0;
		for (size_t i = 0; i < B_COUNT_OF(kDevices); i++) {
			if (!has_entries(kDevices[i].directory)) {
				printf("CE-INFO pointer: no %s device (%s empty)\n",
					kDevices[i].id, kDevices[i].directory);
				continue;
			}
			test_device(kDevices[i], frame);
			if (first == NULL)
				first = &kDevices[i];
			tested++;
		}
		if (first != NULL)
			test_mode_change(*first);
		printf("CE-INFO pointer: %d absolute device(s) tested\n", tested);
	}
	delete app;

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
