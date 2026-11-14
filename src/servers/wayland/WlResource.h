#pragma once
#include <wayland-server-core.h>

class WlResource {
private:
	struct wl_resource *fResource = NULL;

	static void Destructor(struct wl_resource *resource);
	static int Dispatcher(const void *impl, void *resource, uint32_t opcode, const struct wl_message *message, union wl_argument *args);

protected:
	// Renga: called when libwayland destroys the resource (the client asked
	// for it, or the client went away and all its resources go). The
	// default deletes the object; reference-counted objects that may
	// outlive their resource (buffers still shown by a surface) override
	// it and call ForgetResource(): ToResource() is NULL from then on.
	// (Destructors of other objects still use their resource.)
	virtual void ResourceDestroyed() {delete this;}
	void ForgetResource() {fResource = NULL;}

public:
	virtual ~WlResource() = default;
	bool Init(struct wl_client *wl_client, uint32_t version, uint32_t id);
	void Destroy();
	virtual const struct wl_interface *Interface() const = 0;
	virtual int Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args);
	static WlResource *FromResource(struct wl_resource *resource);
	struct wl_resource *ToResource() const {return fResource;}
	struct wl_client *Client() const {return wl_resource_get_client(fResource);}
	uint32_t Id() const {return wl_resource_get_id(fResource);}
	uint32_t Version() const {return wl_resource_get_version(fResource);}
};
