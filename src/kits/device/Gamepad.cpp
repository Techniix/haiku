/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * BGamepad (roadmap F-42): a game controller with the standard layout, on
 * top of an evdev device (/dev/input/event<N>) and an SDL mapping.
 */


#include <Gamepad.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <Looper.h>
#include <Message.h>
#include <NodeMonitor.h>
#include <ObjectList.h>

#include <GamepadMapping.h>


using namespace BPrivate;


static const char* kInputDirectory = "/dev/input";


struct BGamepad::Data {
	int					fd;
	BString				path;
	GamepadDevice		device;
	GamepadMapping		mapping;
	bool				hasMapping;
	BString				guid;
	gamepad_state		state;
	int16				effectID;
	bool				canRumble;
};


static inline bool
test_bit(const uint8* bits, uint32 bit)
{
	return (bits[bit / 8] & (1 << (bit % 8))) != 0;
}


// Whether an evdev device looks like a game controller: SDL's rule, joystick
// or gamepad buttons, or two absolute axes without being a touch device.
static bool
is_gamepad(int fd)
{
	uint8 keyBits[(KEY_CNT + 7) / 8] = {};
	uint8 absBits[(ABS_CNT + 7) / 8] = {};
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0)
		return false;
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);

	for (uint32 code = BTN_JOYSTICK; code <= BTN_THUMBR; code++) {
		if (test_bit(keyBits, code))
			return true;
	}
	if (test_bit(keyBits, BTN_TRIGGER_HAPPY1))
		return true;
	return test_bit(absBits, ABS_X) && test_bit(absBits, ABS_Y)
		&& !test_bit(keyBits, BTN_TOUCH);
}


static int
event_number(const char* name)
{
	if (strncmp(name, "event", 5) != 0)
		return -1;
	char* end;
	long number = strtol(name + 5, &end, 10);
	return *end == '\0' && end != name + 5 ? (int)number : -1;
}


// The gamepads' device paths, by event number.
static int32
list_gamepads(BString* paths, int32 maxCount)
{
	int numbers[64];
	int32 count = 0;
	DIR* dir = opendir(kInputDirectory);
	if (dir == NULL)
		return 0;
	while (dirent* entry = readdir(dir)) {
		int number = event_number(entry->d_name);
		if (number < 0 || count == 64)
			continue;
		char path[64];
		snprintf(path, sizeof(path), "%s/%s", kInputDirectory, entry->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (is_gamepad(fd))
			numbers[count++] = number;
		close(fd);
	}
	closedir(dir);

	// sort
	for (int32 i = 1; i < count; i++) {
		for (int32 j = i; j > 0 && numbers[j - 1] > numbers[j]; j--) {
			int swap = numbers[j];
			numbers[j] = numbers[j - 1];
			numbers[j - 1] = swap;
		}
	}
	for (int32 i = 0; i < count && i < maxCount; i++)
		paths[i].SetToFormat("%s/event%d", kInputDirectory, numbers[i]);
	return count;
}


// #pragma mark - hot-plug watcher


namespace {

class GamepadWatcher : public BLooper {
public:
	GamepadWatcher()
		:
		BLooper("gamepad watcher"),
		fTargets(4)
	{
	}

	void AddTarget(const BMessenger& target)
	{
		fTargets.AddItem(new BMessenger(target));
	}

	bool RemoveTarget(const BMessenger& target)
	{
		for (int32 i = 0; i < fTargets.CountItems(); i++) {
			if (*fTargets.ItemAt(i) == target) {
				delete fTargets.RemoveItemAt(i);
				break;
			}
		}
		return fTargets.IsEmpty();
	}

	status_t StartNodeMonitor()
	{
		BEntry entry(kInputDirectory);
		node_ref ref;
		status_t status = entry.GetNodeRef(&ref);
		if (status != B_OK)
			return status;
		fDirectory = ref;
		return watch_node(&ref, B_WATCH_DIRECTORY, this);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != B_NODE_MONITOR) {
			BLooper::MessageReceived(message);
			return;
		}
		int32 opcode;
		const char* name;
		if (message->FindInt32("opcode", &opcode) != B_OK
			|| message->FindString("name", &name) != B_OK
			|| event_number(name) < 0)
			return;

		BString path;
		path.SetToFormat("%s/%s", kInputDirectory, name);
		BMessage notice;
		if (opcode == B_ENTRY_CREATED) {
			// only game controllers
			int fd = open(path.String(), O_RDONLY | O_NONBLOCK);
			if (fd < 0)
				return;
			bool gamepad = is_gamepad(fd);
			close(fd);
			if (!gamepad)
				return;
			notice.what = B_GAMEPAD_ADDED;
		} else if (opcode == B_ENTRY_REMOVED)
			notice.what = B_GAMEPAD_REMOVED;
		else
			return;
		notice.AddString("path", path);
		for (int32 i = 0; i < fTargets.CountItems(); i++)
			fTargets.ItemAt(i)->SendMessage(&notice);
	}

private:
	BObjectList<BMessenger, true>	fTargets;
	node_ref						fDirectory;
};

}	// namespace


static BLocker sWatcherLock("gamepad watcher lock");
static GamepadWatcher* sWatcher = NULL;


// #pragma mark - BGamepad


BGamepad::BGamepad()
	:
	fData(new(std::nothrow) Data)
{
	if (fData != NULL) {
		fData->fd = -1;
		fData->hasMapping = false;
		fData->effectID = -1;
		fData->canRumble = false;
		memset(&fData->state, 0, sizeof(fData->state));
		memset(&fData->device, 0, sizeof(fData->device));
	}
}


BGamepad::~BGamepad()
{
	Close();
	delete fData;
}


status_t
BGamepad::Open(const char* devicePath)
{
	if (fData == NULL)
		return B_NO_MEMORY;
	Close();

	int fd = open(devicePath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fd = open(devicePath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return errno;

	GamepadDevice& device = fData->device;
	memset(&device, 0, sizeof(device));
	if (ioctl(fd, EVIOCGID, &device.id) < 0
		|| ioctl(fd, EVIOCGNAME(sizeof(device.name)), device.name) < 0
		|| ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(device.keyBits)),
			device.keyBits) < 0) {
		close(fd);
		return B_BAD_TYPE;
	}
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(device.absBits)), device.absBits);
	for (uint32 code = 0; code < ABS_CNT; code++) {
		if (test_bit(device.absBits, code))
			ioctl(fd, EVIOCGABS(code), &device.abs[code]);
	}
	ioctl(fd, EVIOCGKEY(sizeof(device.keyState)), device.keyState);
	device.Enumerate();

	uint8 evBits[(EV_CNT + 7) / 8] = {};
	uint8 ffBits[(FF_CNT + 7) / 8] = {};
	ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
	ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits);
	fData->canRumble = test_bit(evBits, EV_FF) && test_bit(ffBits, FF_RUMBLE);

	char guid[33];
	device.GUID(guid);
	fData->guid = guid;

	BString line;
	fData->hasMapping = false;
	if (gamepad_find_mapping(guid, line) == B_OK)
		fData->hasMapping = fData->mapping.Parse(line.String());
	if (!fData->hasMapping)
		fData->hasMapping = fData->mapping.SetDefault(device);

	fData->fd = fd;
	fData->path = devicePath;
	fData->effectID = -1;
	Update();
	return B_OK;
}


status_t
BGamepad::Open(int32 index)
{
	BString path;
	status_t status = GetGamepadPath(index, &path);
	if (status != B_OK)
		return status;
	return Open(path.String());
}


void
BGamepad::Close()
{
	if (fData == NULL || fData->fd < 0)
		return;
	if (fData->effectID >= 0)
		ioctl(fData->fd, EVIOCRMFF, (void*)(addr_t)fData->effectID);
	close(fData->fd);
	fData->fd = -1;
	fData->path = "";
}


status_t
BGamepad::InitCheck() const
{
	if (fData == NULL)
		return B_NO_MEMORY;
	return fData->fd >= 0 ? B_OK : B_NO_INIT;
}


int32
BGamepad::CountGamepads()
{
	BString paths[64];
	return list_gamepads(paths, 64);
}


status_t
BGamepad::GetGamepadPath(int32 index, BString* path)
{
	if (path == NULL || index < 0)
		return B_BAD_VALUE;
	BString paths[64];
	int32 count = list_gamepads(paths, 64);
	if (index >= count || index >= 64)
		return B_BAD_INDEX;
	*path = paths[index];
	return B_OK;
}


status_t
BGamepad::StartWatching(const BMessenger& target)
{
	BAutolock locker(sWatcherLock);
	if (sWatcher == NULL) {
		sWatcher = new(std::nothrow) GamepadWatcher;
		if (sWatcher == NULL)
			return B_NO_MEMORY;
		sWatcher->Run();
		BAutolock looperLocker(sWatcher);
		status_t status = sWatcher->StartNodeMonitor();
		if (status != B_OK) {
			sWatcher->Quit();
			sWatcher = NULL;
			return status;
		}
	}
	BAutolock looperLocker(sWatcher);
	sWatcher->AddTarget(target);
	return B_OK;
}


void
BGamepad::StopWatching(const BMessenger& target)
{
	BAutolock locker(sWatcherLock);
	if (sWatcher == NULL)
		return;
	sWatcher->Lock();
	if (sWatcher->RemoveTarget(target)) {
		stop_watching(sWatcher);
		sWatcher->Quit();
		sWatcher = NULL;
	} else
		sWatcher->Unlock();
}


const char*
BGamepad::Name() const
{
	return fData != NULL ? fData->device.name : NULL;
}


const char*
BGamepad::Path() const
{
	return fData != NULL ? fData->path.String() : NULL;
}


uint16
BGamepad::VendorID() const
{
	return fData != NULL ? fData->device.id.vendor : 0;
}


uint16
BGamepad::ProductID() const
{
	return fData != NULL ? fData->device.id.product : 0;
}


const char*
BGamepad::GUID() const
{
	return fData != NULL ? fData->guid.String() : NULL;
}


bool
BGamepad::HasMapping() const
{
	return fData != NULL && fData->hasMapping;
}


const char*
BGamepad::Mapping() const
{
	return HasMapping() ? fData->mapping.Line() : NULL;
}


int
BGamepad::FileDescriptor() const
{
	return fData != NULL ? fData->fd : -1;
}


status_t
BGamepad::Update()
{
	if (InitCheck() != B_OK)
		return B_NO_INIT;

	GamepadDevice& device = fData->device;
	struct input_event events[64];
	bool resync = false;
	while (true) {
		ssize_t bytes = read(fData->fd, events, sizeof(events));
		if (bytes < 0) {
			if (errno == B_WOULD_BLOCK || errno == EAGAIN
				|| errno == EINTR)
				break;
			return errno;
		}
		if (bytes == 0)
			break;
		int32 count = bytes / sizeof(struct input_event);
		for (int32 i = 0; i < count; i++) {
			const struct input_event& event = events[i];
			if (event.type == EV_KEY && event.code < KEY_CNT) {
				if (event.value != 0)
					device.keyState[event.code / 8] |= 1 << (event.code % 8);
				else {
					device.keyState[event.code / 8]
						&= ~(1 << (event.code % 8));
				}
			} else if (event.type == EV_ABS && event.code < ABS_CNT)
				device.abs[event.code].value = event.value;
			else if (event.type == EV_SYN && event.code == SYN_DROPPED)
				resync = true;
		}
		if (count < (int32)(sizeof(events) / sizeof(events[0])))
			break;
	}

	if (resync) {
		ioctl(fData->fd, EVIOCGKEY(sizeof(device.keyState)), device.keyState);
		for (uint32 code = 0; code < ABS_CNT; code++) {
			if (test_bit(device.absBits, code))
				ioctl(fData->fd, EVIOCGABS(code), &device.abs[code]);
		}
	}

	memset(&fData->state, 0, sizeof(fData->state));
	fData->state.time = system_time();
	if (fData->hasMapping)
		fData->mapping.Apply(device, fData->state);
	return B_OK;
}


status_t
BGamepad::GetState(gamepad_state* state) const
{
	if (state == NULL)
		return B_BAD_VALUE;
	if (InitCheck() != B_OK)
		return B_NO_INIT;
	*state = fData->state;
	return B_OK;
}


bool
BGamepad::IsPressed(gamepad_button button) const
{
	if (fData == NULL || button >= B_GAMEPAD_BUTTON_COUNT)
		return false;
	return (fData->state.buttons & (1UL << button)) != 0;
}


int16
BGamepad::AxisValue(gamepad_axis axis) const
{
	if (fData == NULL || axis >= B_GAMEPAD_AXIS_COUNT)
		return 0;
	return fData->state.axes[axis];
}


bool
BGamepad::CanRumble() const
{
	return fData != NULL && fData->canRumble;
}


status_t
BGamepad::Rumble(uint16 strong, uint16 weak, bigtime_t duration)
{
	if (InitCheck() != B_OK)
		return B_NO_INIT;
	if (!fData->canRumble)
		return B_NOT_SUPPORTED;

	struct ff_effect effect;
	memset(&effect, 0, sizeof(effect));
	effect.type = FF_RUMBLE;
	effect.id = fData->effectID;
	effect.u.rumble.strong_magnitude = strong;
	effect.u.rumble.weak_magnitude = weak;
	bigtime_t milliseconds = duration / 1000;
	effect.replay.length = milliseconds > 0xffff ? 0xffff
		: milliseconds <= 0 ? 0 : milliseconds;
	if (ioctl(fData->fd, EVIOCSFF, &effect) < 0)
		return errno;
	fData->effectID = effect.id;

	struct input_event play;
	memset(&play, 0, sizeof(play));
	play.type = EV_FF;
	play.code = effect.id;
	play.value = 1;
	if (write(fData->fd, &play, sizeof(play)) != (ssize_t)sizeof(play))
		return errno;
	return B_OK;
}


status_t
BGamepad::StopRumble()
{
	if (InitCheck() != B_OK)
		return B_NO_INIT;
	if (fData->effectID < 0)
		return B_OK;
	struct input_event stop;
	memset(&stop, 0, sizeof(stop));
	stop.type = EV_FF;
	stop.code = fData->effectID;
	stop.value = 0;
	if (write(fData->fd, &stop, sizeof(stop)) != (ssize_t)sizeof(stop))
		return errno;
	return B_OK;
}


int32
BGamepad::Player() const
{
	int player = 0;
	if (InitCheck() != B_OK
		|| ioctl(fData->fd, EVIOC_RENGA_GPLAYER, &player) < 0)
		return 0;
	return player;
}


status_t
BGamepad::SetPlayer(int32 player)
{
	if (InitCheck() != B_OK)
		return B_NO_INIT;
	int value = player;
	if (ioctl(fData->fd, EVIOC_RENGA_SPLAYER, &value) < 0)
		return errno;
	return B_OK;
}


status_t
BGamepad::GetBattery(int32* level, bool* charging, bool* wired) const
{
	if (InitCheck() != B_OK)
		return B_NO_INIT;
	struct renga_input_battery battery;
	if (ioctl(fData->fd, EVIOC_RENGA_GBATTERY, &battery) < 0)
		return errno;
	if (level != NULL)
		*level = battery.level;
	if (charging != NULL)
		*charging = battery.charging != 0;
	if (wired != NULL)
		*wired = battery.wired != 0;
	return B_OK;
}


status_t
BGamepad::AddMapping(const char* sdlMapping)
{
	return gamepad_add_mapping(sdlMapping);
}


status_t
BGamepad::FindMapping(const char* guid, BString* mapping)
{
	if (mapping == NULL)
		return B_BAD_VALUE;
	return gamepad_find_mapping(guid, *mapping);
}


void BGamepad::_ReservedGamepad1() {}
void BGamepad::_ReservedGamepad2() {}
void BGamepad::_ReservedGamepad3() {}
void BGamepad::_ReservedGamepad4() {}
