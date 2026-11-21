/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Gamepads for Linux programs under Renguin (static glibc program): Renga's
 * gamepad core (F-42) seen through Linux's evdev and uinput interfaces,
 * as SDL, Steam and Proton use them. One line per part that worked, then
 * "ok"; "skip" when this system has no gamepad core.
 *
 *  uinput   /dev/uinput (Linux's other name for /dev/input/uinput) creates
 *           a virtual pad with buttons, axes and rumble (UI_SET_*BIT
 *           with values, UI_ABS_SETUP, UI_DEV_SETUP, UI_DEV_CREATE)
 *  evdev    its /dev/input/event<N>: EVIOCGVERSION, EVIOCGNAME, EVIOCGID,
 *           EVIOCGBIT (keys), EVIOCGABS (range)
 *  events   a button press written to uinput arrives as Linux
 *           input_events (24 bytes) with SYN_REPORT, poll() says readable
 *  grab     EVIOCGRAB takes its value (1, then 0), not a pointer
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf(" (errno %d %s)\n", errno, strerror(errno)); \
			exit(1); \
		} \
	} while (0)


static void
emit(int fd, int type, int code, int value)
{
	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.type = type;
	event.code = code;
	event.value = value;
	CHECK(write(fd, &event, sizeof(event)) == sizeof(event), "uinput write");
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	int pad = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	if (pad < 0 && (errno == ENOENT || errno == ENODEV)) {
		printf("skip\nok\n");
		return 0;
	}
	CHECK(pad >= 0, "open /dev/uinput");
	CHECK(ioctl(pad, UI_SET_EVBIT, EV_KEY) == 0, "UI_SET_EVBIT EV_KEY");
	CHECK(ioctl(pad, UI_SET_EVBIT, EV_ABS) == 0, "UI_SET_EVBIT EV_ABS");
	int buttons[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_START };
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
		CHECK(ioctl(pad, UI_SET_KEYBIT, buttons[i]) == 0, "UI_SET_KEYBIT");
	int axes[] = { ABS_X, ABS_Y };
	for (size_t i = 0; i < 2; i++) {
		CHECK(ioctl(pad, UI_SET_ABSBIT, axes[i]) == 0, "UI_SET_ABSBIT");
		struct uinput_abs_setup setup;
		memset(&setup, 0, sizeof(setup));
		setup.code = axes[i];
		setup.absinfo.minimum = -32768;
		setup.absinfo.maximum = 32767;
		setup.absinfo.flat = 128;
		CHECK(ioctl(pad, UI_ABS_SETUP, &setup) == 0, "UI_ABS_SETUP");
	}
	ioctl(pad, UI_SET_EVBIT, EV_FF);
	ioctl(pad, UI_SET_FFBIT, FF_RUMBLE);

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x2dc8;
	setup.id.product = 0x310a;
	setup.id.version = 0x0114;
	strcpy(setup.name, "Renguin Linux test pad");
	CHECK(ioctl(pad, UI_DEV_SETUP, &setup) == 0, "UI_DEV_SETUP");
	CHECK(ioctl(pad, UI_DEV_CREATE) == 0, "UI_DEV_CREATE");
	char sysname[32] = "";
	CHECK(ioctl(pad, UI_GET_SYSNAME(sizeof(sysname)), sysname) >= 0
		&& strncmp(sysname, "input", 5) == 0, "UI_GET_SYSNAME \"%s\"",
		sysname);
	printf("uinput\n");

	char path[64];
	snprintf(path, sizeof(path), "/dev/input/event%d", atoi(sysname + 5));
	int event = -1;
	for (int i = 0; i < 100 && event < 0; i++) {
		event = open(path, O_RDONLY | O_NONBLOCK);
		if (event < 0)
			usleep(20000);
	}
	CHECK(event >= 0, "open %s", path);
	int version = 0;
	CHECK(ioctl(event, EVIOCGVERSION, &version) == 0 && version >= 0x10000,
		"EVIOCGVERSION %#x", version);
	char name[128] = "";
	CHECK(ioctl(event, EVIOCGNAME(sizeof(name)), name) > 0
		&& strcmp(name, "Renguin Linux test pad") == 0, "EVIOCGNAME \"%s\"",
		name);
	struct input_id id;
	CHECK(ioctl(event, EVIOCGID, &id) == 0 && id.vendor == 0x2dc8
		&& id.product == 0x310a, "EVIOCGID %04x:%04x", id.vendor, id.product);
	unsigned char keys[KEY_MAX / 8 + 1];
	memset(keys, 0, sizeof(keys));
	CHECK(ioctl(event, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) > 0
		&& (keys[BTN_SOUTH / 8] & (1 << (BTN_SOUTH % 8))) != 0,
		"EVIOCGBIT EV_KEY without BTN_SOUTH");
	struct input_absinfo absinfo;
	CHECK(ioctl(event, EVIOCGABS(ABS_X), &absinfo) == 0
		&& absinfo.minimum == -32768 && absinfo.maximum == 32767,
		"EVIOCGABS %d..%d", absinfo.minimum, absinfo.maximum);
	printf("evdev\n");

	emit(pad, EV_KEY, BTN_SOUTH, 1);
	emit(pad, EV_SYN, SYN_REPORT, 0);
	struct pollfd poller = { event, POLLIN, 0 };
	CHECK(poll(&poller, 1, 2000) == 1 && (poller.revents & POLLIN) != 0,
		"poll");
	struct input_event events[8];
	ssize_t bytes = read(event, events, sizeof(events));
	CHECK(bytes > 0 && bytes % sizeof(struct input_event) == 0,
		"read %zd bytes", bytes);
	int count = bytes / sizeof(struct input_event);
	int pressed = 0, synced = 0;
	for (int i = 0; i < count; i++) {
		if (events[i].type == EV_KEY && events[i].code == BTN_SOUTH
			&& events[i].value == 1) {
			pressed = 1;
		}
		if (events[i].type == EV_SYN && events[i].code == SYN_REPORT)
			synced = 1;
	}
	CHECK(pressed && synced, "%d events, press %d, SYN_REPORT %d", count,
		pressed, synced);
	printf("events\n");

	CHECK(ioctl(event, EVIOCGRAB, 1) == 0, "EVIOCGRAB 1");
	CHECK(ioctl(event, EVIOCGRAB, 0) == 0, "EVIOCGRAB 0");
	printf("grab\n");

	close(event);
	ioctl(pad, UI_DEV_DESTROY);
	close(pad);
	printf("ok\n");
	return 0;
}
