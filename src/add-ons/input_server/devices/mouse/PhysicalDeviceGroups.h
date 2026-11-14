/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef PHYSICAL_DEVICE_GROUPS_H
#define PHYSICAL_DEVICE_GROUPS_H


/*!	Renga: which driver nodes belong to the same physical pointing device.

	Many mice expose more than one HID mouse interface: the maintainer's
	Darmoshark M3 has a boot mouse on interface 0 and another on interface 2
	(next to a keyboard on interface 1), and moves on whichever one its
	firmware uses. usb_hid publishes a node for each (/dev/input/mouse/usb/0
	and /1), and input_server used to show them as two mice ("USB Mouse 1",
	"USB Mouse 2") with separate settings, so a setting changed for the one
	shown first did nothing for the one that moves.

	This class keeps the bookkeeping, apart from the devices so it can be
	tested on its own (3rdparty/ce/tests/pointer/pointer_logic_test.cpp):
	the first node of a physical device is its primary, which input_server
	sees; the others are members that use the primary's name (and therefore
	its settings) and are started and stopped with it. Nodes without a key
	(PS/2, anything that cannot say which device it is) are never joined.
	When the primary goes away while members remain, the first member takes
	its place under the same name.

	Not thread safe; MouseInputDevice uses it under its device list lock.
*/


#include <ObjectList.h>
#include <String.h>


template<typename Device>
class PhysicalDeviceGroups {
public:
	/*!	Adds \a device with the physical device key \a key (NULL or empty:
		none). Returns the primary it joins, or NULL if it is a primary
		itself.
	*/
	Device* Add(Device* device, const char* key)
	{
		Entry* entry = new Entry;
		entry->device = device;
		entry->key = key != NULL ? key : "";
		entry->primary = NULL;
		if (entry->key.Length() > 0) {
			for (int32 i = 0; i < fEntries.CountItems(); i++) {
				Entry* other = fEntries.ItemAt(i);
				if (other->primary == NULL && other->key == entry->key) {
					entry->primary = other->device;
					break;
				}
			}
		}
		fEntries.AddItem(entry);
		return entry->primary;
	}

	/*!	Removes \a device. If it was a primary with members, the first
		member becomes the primary of the others and is returned; otherwise
		NULL.
	*/
	Device* Remove(Device* device)
	{
		Entry* entry = _Find(device);
		if (entry == NULL)
			return NULL;
		fEntries.RemoveItem(entry, false);
		Device* promoted = NULL;
		if (entry->primary == NULL) {
			for (int32 i = 0; i < fEntries.CountItems(); i++) {
				Entry* other = fEntries.ItemAt(i);
				if (other->primary != device)
					continue;
				if (promoted == NULL) {
					promoted = other->device;
					other->primary = NULL;
				} else
					other->primary = promoted;
			}
		}
		delete entry;
		return promoted;
	}

	//! The primary of \a device, or NULL if it is one (or unknown).
	Device* PrimaryOf(Device* device) const
	{
		Entry* entry = _Find(device);
		return entry != NULL ? entry->primary : NULL;
	}

	int32 CountMembers(Device* primary) const
	{
		int32 count = 0;
		for (int32 i = 0; i < fEntries.CountItems(); i++) {
			if (fEntries.ItemAt(i)->primary == primary && primary != NULL)
				count++;
		}
		return count;
	}

	Device* MemberAt(Device* primary, int32 index) const
	{
		for (int32 i = 0; i < fEntries.CountItems(); i++) {
			Entry* entry = fEntries.ItemAt(i);
			if (entry->primary == primary && primary != NULL && index-- == 0)
				return entry->device;
		}
		return NULL;
	}

private:
	struct Entry {
		Device*	device;
		BString	key;
		Device*	primary;
	};

	Entry* _Find(Device* device) const
	{
		for (int32 i = 0; i < fEntries.CountItems(); i++) {
			if (fEntries.ItemAt(i)->device == device)
				return fEntries.ItemAt(i);
		}
		return NULL;
	}

private:
	BObjectList<Entry, true> fEntries;
};


#endif	// PHYSICAL_DEVICE_GROUPS_H
