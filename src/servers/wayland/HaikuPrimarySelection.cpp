/*
 * Renga: primary selection (zwp_primary_selection_device_manager_v1).
 * Copyright 2026, Renga contributors. Distributed under the terms of the
 * MIT License.
 *
 * Haiku has no primary selection (select to copy, middle click to paste),
 * so it is kept between Wayland clients only: the latest source is offered
 * to every client's device, and a receive request goes straight to the
 * source with the receiver's descriptor.
 */
#include "RengaProtocols.h"
#include "WpPrimarySelectionUnstableV1.h"

#include <AutoDeleter.h>

#include <algorithm>
#include <new>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

extern const struct wl_interface zwp_primary_selection_device_manager_v1_interface;


class HaikuPrimarySource;
class HaikuPrimaryDevice;

static HaikuPrimarySource *sPrimarySource = NULL;
static std::vector<HaikuPrimaryDevice*> sPrimaryDevices;

static void broadcast_primary();


class HaikuPrimarySource: public ZwpPrimarySelectionSourceV1 {
public:
	std::set<std::string> fMimeTypes;

	virtual ~HaikuPrimarySource()
	{
		if (sPrimarySource == this) {
			sPrimarySource = NULL;
			broadcast_primary();
		}
	}

	void HandleOffer(const char *mimeType) final
	{
		fMimeTypes.emplace(mimeType);
	}
};


class HaikuPrimaryOffer: public ZwpPrimarySelectionOfferV1 {
public:
	HaikuPrimarySource *fSource;

	HaikuPrimaryOffer(HaikuPrimarySource *source): fSource(source) {}

	void HandleReceive(const char *mimeType, int32_t fd) final
	{
		if (fSource != NULL && fSource == sPrimarySource
			&& fSource->ToResource() != NULL) {
			fSource->SendSend(mimeType, fd);
			wl_client_flush(fSource->Client());
		}
		close(fd);
	}
};


class HaikuPrimaryDevice: public ZwpPrimarySelectionDeviceV1 {
public:
	HaikuPrimaryDevice() {sPrimaryDevices.push_back(this);}

	virtual ~HaikuPrimaryDevice()
	{
		sPrimaryDevices.erase(std::remove(sPrimaryDevices.begin(),
			sPrimaryDevices.end(), this), sPrimaryDevices.end());
	}

	void Announce()
	{
		HaikuPrimarySource *source = sPrimarySource;
		if (source == NULL) {
			SendSelection(NULL);
			return;
		}
		HaikuPrimaryOffer *offer = new(std::nothrow) HaikuPrimaryOffer(source);
		if (offer == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		if (!offer->Init(Client(), Version(), 0))
			return;
		SendDataOffer(offer->ToResource());
		for (const std::string &type : source->fMimeTypes)
			offer->SendOffer(type.c_str());
		SendSelection(offer->ToResource());
	}

	void HandleSetSelection(struct wl_resource *sourceResource, uint32_t serial) final
	{
		HaikuPrimarySource *source = sourceResource != NULL
			? (HaikuPrimarySource*)WlResource::FromResource(sourceResource) : NULL;
		if (source == sPrimarySource)
			return;
		HaikuPrimarySource *old = sPrimarySource;
		sPrimarySource = source;
		if (old != NULL)
			old->SendCancelled();
		broadcast_primary();
	}
};


static void
broadcast_primary()
{
	for (HaikuPrimaryDevice *device : sPrimaryDevices)
		device->Announce();
}


class HaikuPrimaryManager: public ZwpPrimarySelectionDeviceManagerV1 {
public:
	void HandleCreateSource(uint32_t id) final
	{
		HaikuPrimarySource *source = new(std::nothrow) HaikuPrimarySource();
		if (source == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		source->Init(Client(), Version(), id);
	}

	void HandleGetDevice(uint32_t id, struct wl_resource *seat) final
	{
		HaikuPrimaryDevice *device = new(std::nothrow) HaikuPrimaryDevice();
		if (device == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		if (!device->Init(Client(), Version(), id))
			return;
		device->Announce();
	}
};


HaikuPrimarySelectionGlobal *HaikuPrimarySelectionGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuPrimarySelectionGlobal> global(new(std::nothrow) HaikuPrimarySelectionGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &zwp_primary_selection_device_manager_v1_interface, 1)) return NULL;
	return global.Detach();
}

void HaikuPrimarySelectionGlobal::Bind(struct wl_client *client, uint32_t version, uint32_t id)
{
	HaikuPrimaryManager *manager = new(std::nothrow) HaikuPrimaryManager();
	if (manager == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	manager->Init(client, version, id);
}
