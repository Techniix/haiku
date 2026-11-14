#pragma once

#include <optional>

#include "WlGlobal.h"
#include "Wayland.h"

#include <Bitmap.h>

#include <Referenceable.h>
#include <AutoDeleter.h>
#include <AutoDeleterOS.h>


class HaikuShmGlobal: public WlGlocal {
public:
	static HaikuShmGlobal *Create(struct wl_display *display);
	virtual ~HaikuShmGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) final;
};

class HaikuShm: public WlShm {
public:
	virtual ~HaikuShm() = default;
	void HandleCreatePool(uint32_t id, int32_t fd, int32_t size) final;
};

class HaikuShmAreaRef: public BReferenceable {
private:
	AreaDeleter fArea;

public:
	HaikuShmAreaRef(area_id area): fArea(area) {}
	area_id Area() const {return fArea.Get();}
};

class HaikuShmPool: public WlShmPool {
private:
	friend class HaikuShm;
	friend class HaikuShmBuffer;

	FileDescriptorCloser fFd;
	BReference<HaikuShmAreaRef> fAreaRef;
	void *fAddress {};
	size_t fSize {};

	void Remap(int32_t size);

public:
	virtual ~HaikuShmPool() = default;
	void HandleCreateBuffer(uint32_t id, int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format) final;
	void HandleResize(int32_t size) final;
};

class HaikuShmBuffer: public WlBuffer, public BReferenceable {
private:
	friend class HaikuShmPool;

	BReference<HaikuShmAreaRef> fAreaRef;
	std::optional<BBitmap> fBitmap;

	// Renga: the resource holds one reference, released when the client
	// destroys the buffer; surfaces showing it hold theirs. The object is
	// deleted when both the resource and the last reference are gone, in
	// whichever order (a client that goes away destroys its resources in
	// any order, surfaces after buffers or before).
	bool fResourceReferenceReleased = false;
	bool fResourceGone = false;

protected:
	void ResourceDestroyed() final
	{
		ForgetResource();
		fResourceGone = true;
		if (!fResourceReferenceReleased) {
			fResourceReferenceReleased = true;
			ReleaseReference();
				// may delete this (LastReferenceReleased)
		} else if (CountReferences() == 0)
			delete this;
	}

public:
	static HaikuShmBuffer *FromResource(struct wl_resource *resource) {return (HaikuShmBuffer*)WlResource::FromResource(resource);}
	virtual ~HaikuShmBuffer() = default;

	void HandleDestroy() final
	{
		if (!fResourceReferenceReleased) {
			fResourceReferenceReleased = true;
			ReleaseReference();
		}
	}

	void LastReferenceReleased() final
	{
		if (fResourceGone)
			delete this;
		else
			Destroy();
				// ResourceDestroyed() deletes this
	}

	void SendReleaseIfAlive() {if (ToResource() != NULL) SendRelease();}

	inline BBitmap &Bitmap() {return fBitmap.value();}
};
