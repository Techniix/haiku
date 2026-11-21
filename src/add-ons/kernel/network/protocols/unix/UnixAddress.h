/*
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef UNIX_ADDRESS_H
#define UNIX_ADDRESS_H

#include <stddef.h>
#include <string.h>
#include <sys/un.h>

#include <SupportDefs.h>


// NOTE: We support the standard FS address space as well as the alternative
// internal address space Linux features (sun_path[0] is 0, followed by 5 hex
// digits, without null-termination). The latter one is nice to have, because
// the address lookup is quick (hash table lookup, instead of asking the VFS to
// resolve the path), and we don't have to pollute the FS when auto-binding
// sockets (e.g. on connect()).


#define	INTERNAL_UNIX_ADDRESS_LEN	(2 + 1 + 5)
	// sun_len + sun_family + null byte + 5 hex digits

// Renga: any other name after the null byte is a Linux abstract name: the
// bytes up to the address length (sun_len), with no null termination,
// nulls included. Linux allows up to 107 bytes (sun_path is 108 there).
#define	UNIX_ABSTRACT_NAME_MAX		107


struct vnode;


class UnixAddress {
public:
	UnixAddress()
	{
		Unset();
	}

	UnixAddress(const UnixAddress& other)
	{
		*this = other;
	}

	UnixAddress(int32 internalID)
	{
		SetTo(internalID);
	}

	UnixAddress(dev_t volumeID, ino_t nodeID, struct vnode* vnode)
	{
		SetTo(volumeID, nodeID, vnode);
	}

	void SetTo(int32 internalID)
	{
		fInternalID = internalID;
		fVolumeID = -1;
		fNodeID = -1;
		fVnode = NULL;
		fAbstractLength = 0;
	}

	void SetTo(dev_t volumeID, ino_t nodeID, struct vnode* vnode)
	{
		fInternalID = -1;
		fVolumeID = volumeID;
		fNodeID = nodeID;
		fVnode = vnode;
		fAbstractLength = 0;
	}

	//! A Linux abstract name of 1 to UNIX_ABSTRACT_NAME_MAX bytes.
	void SetToAbstract(const char* name, size_t length)
	{
		fInternalID = -1;
		fVolumeID = -1;
		fNodeID = -1;
		fVnode = NULL;
		fAbstractLength = length;
		memcpy(fAbstractName, name, length);
	}

	void Unset()
	{
		fInternalID = -1;
		fVolumeID = -1;
		fNodeID = -1;
		fVnode = NULL;
		fAbstractLength = 0;
	}

	bool IsValid() const
	{
		return fInternalID >= 0 || fVolumeID >= 0 || fAbstractLength > 0;
	}

	bool IsAbstractAddress() const
	{
		return fAbstractLength > 0;
	}

	bool IsInternalAddress() const
	{
		return fInternalID >= 0;
	}

	int32 InternalID() const
	{
		return fInternalID;
	}

	int32 VolumeID() const
	{
		return fVolumeID;
	}

	int32 NodeID() const
	{
		return fNodeID;
	}

	struct vnode* Vnode() const
	{
		return fVnode;
	}

	uint32 HashCode() const
	{
		if (fAbstractLength > 0) {
			// FNV-1a of the name; kept apart from small internal IDs
			uint32 hash = 2166136261u;
			for (uint32 i = 0; i < fAbstractLength; i++)
				hash = (hash ^ (uint8)fAbstractName[i]) * 16777619u;
			return hash | 0x80000000u;
		}
		return fInternalID >= 0
			? fInternalID
			: uint32(fVolumeID) ^ uint32(fNodeID);
	}

	char* ToString(char *buffer, size_t bufferSize) const;

	UnixAddress& operator=(const UnixAddress& other)
	{
		fInternalID = other.fInternalID;
		fVolumeID = other.fVolumeID;
		fNodeID = other.fNodeID;
		fVnode = other.fVnode;
		fAbstractLength = other.fAbstractLength;
		memcpy(fAbstractName, other.fAbstractName, fAbstractLength);
		return *this;
	}

	bool operator==(const UnixAddress& other) const
	{
		if (fAbstractLength > 0 || other.fAbstractLength > 0) {
			return fAbstractLength == other.fAbstractLength
				&& memcmp(fAbstractName, other.fAbstractName,
					fAbstractLength) == 0;
		}
		return fInternalID >= 0
			? fInternalID == other.fInternalID
			: fVolumeID == other.fVolumeID
				&& fNodeID == other.fNodeID;
	}

	bool operator!=(const UnixAddress& other) const
	{
		return !(*this == other);
	}

	static bool IsEmptyAddress(const sockaddr_un& address)
	{
		return address.sun_len == sizeof(sockaddr)
			&& address.sun_path[0] == '\0' && address.sun_path[1] == '\0';
	}

	static int32 InternalID(const sockaddr_un& address)
	{
		if (address.sun_len < INTERNAL_UNIX_ADDRESS_LEN
				|| address.sun_path[0] != '\0') {
			return B_BAD_VALUE;
		}

		// parse the ID
		int32 id = 0;

		for (int32 i = 0; i < 5; i++) {
			char c = address.sun_path[i + 1];
			if (c >= '0' && c <= '9')
				id = (id << 4) + (c - '0');
			else if (c >= 'a' && c <= 'f')
				id = (id << 4) + 10 + (c - 'a');
			else
				return B_BAD_VALUE;
		}

		if (address.sun_len > INTERNAL_UNIX_ADDRESS_LEN) {
			// longer addresses stay internal IDs only if the rest is nulls
			// (native callers passing sizeof(sockaddr_un)); otherwise they
			// are abstract names
			size_t length = address.sun_len;
			if (length > sizeof(sockaddr_un))
				length = sizeof(sockaddr_un);
			for (size_t i = INTERNAL_UNIX_ADDRESS_LEN; i < length; i++) {
				if (((const char*)&address)[i] != '\0')
					return B_BAD_VALUE;
			}
		}

		return id;
	}

	/*!	The address of a name in the internal space (sun_path[0] is a null
		byte, not the empty address): Haiku's 5-hex-digit internal IDs, or
		else a Linux abstract name. B_BAD_VALUE if there is no name.
	*/
	static status_t FromInternalSpace(const sockaddr_un& address,
		UnixAddress& _address)
	{
		int32 internalID = InternalID(address);
		if (internalID >= 0) {
			_address.SetTo(internalID);
			return B_OK;
		}
		size_t offset = offsetof(sockaddr_un, sun_path) + 1;
		size_t end = address.sun_len;
		if (end > sizeof(sockaddr_un))
			end = sizeof(sockaddr_un);
		if (end <= offset || end - offset > UNIX_ABSTRACT_NAME_MAX)
			return B_BAD_VALUE;
		_address.SetToAbstract(address.sun_path + 1, end - offset);
		return B_OK;
	}

private:
	// fat interface: If fInternalID is >= 0, it's an address in the internal
	// namespace, otherwise a FS address.
	int32			fInternalID;
	dev_t			fVolumeID;
	ino_t			fNodeID;
	struct vnode*	fVnode;
	uint32			fAbstractLength;	// > 0: a Linux abstract name
	char			fAbstractName[UNIX_ABSTRACT_NAME_MAX];
};


#endif	// UNIX_ADDRESS_H
