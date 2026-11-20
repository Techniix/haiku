/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DBUS_MESSAGE_H
#define DBUS_MESSAGE_H


/*!	D-Bus messages for Renga's session bus (roadmap F-43 phase D): the wire
	format of the D-Bus specification (little-endian when written; both
	byte orders when read), with message bodies as trees of DValue built
	from and written to type signatures.

	Supported types: y b n q i u x t d h s o g, arrays (a), structs ( ),
	dictionary entries { } and variants (v). Unix file descriptors (h) are
	indices into the message's descriptor list, as on the wire.
*/


#include <String.h>
#include <SupportDefs.h>

#include <vector>


namespace DBus {


enum {
	METHOD_CALL		= 1,
	METHOD_RETURN	= 2,
	ERROR			= 3,
	SIGNAL			= 4
};

enum {
	NO_REPLY_EXPECTED	= 0x1,
	NO_AUTO_START		= 0x2
};

static const uint32 kMaxMessageSize = 64 * 1024 * 1024;


struct DValue {
	char				type;		// a type code; 'a' for arrays
	BString				signature;	// arrays: the element's; variants:
									// the contained value's
	int64				i;
	uint64				u;
	double				d;
	BString				s;
	std::vector<DValue>	items;		// array elements, struct and dict
									// entry members, the variant's value

						DValue() : type(0), i(0), u(0), d(0) {}

	static	DValue		Byte(uint8 value);
	static	DValue		Bool(bool value);
	static	DValue		Int32(int32 value);
	static	DValue		UInt32(uint32 value);
	static	DValue		Int64(int64 value);
	static	DValue		UInt64(uint64 value);
	static	DValue		Double(double value);
	static	DValue		String(const char* value);
	static	DValue		ObjectPath(const char* value);
	static	DValue		Signature(const char* value);
	static	DValue		Array(const char* elementSignature);
	static	DValue		Struct();
	static	DValue		DictEntry(const DValue& key, const DValue& value);
	static	DValue		Variant(const DValue& value);

			DValue&		Add(const DValue& item)
							{ items.push_back(item); return *this; }

			// the type signature of this value
			BString		Signature() const;

			// for dictionaries a{s...}: the value for \a key, or NULL
			const DValue* Find(const char* key) const;
			// a variant's content, or the value itself
			const DValue& Unwrapped() const;
};

typedef std::vector<DValue> DValueList;


struct Message {
	uint8				type;
	uint8				flags;
	uint32				serial;
	uint32				replySerial;
	BString				path;
	BString				interface;
	BString				member;
	BString				errorName;
	BString				destination;
	BString				sender;
	BString				signature;
	uint32				unixFDCount;

	std::vector<uint8>	body;		// little-endian, as written by Write()
	bool				bigEndianBody;
	std::vector<int>	fds;		// received or to send

						Message();

			// Parses one whole message from \a data; \a size must be at
			// least what NeededSize() said.
			status_t	Parse(const uint8* data, size_t size);
			// The size of the message in \a data (at least 16 bytes),
			// 0 if not known yet, or an error.
	static	ssize_t		NeededSize(const uint8* data, size_t size);

			// The whole message on the wire.
			void		Serialize(std::vector<uint8>& out) const;

			status_t	GetArguments(DValueList& values) const;
			void		SetArguments(const DValueList& values);

	static	Message		MethodCall(const char* destination, const char* path,
							const char* interface, const char* member);
	static	Message		Signal(const char* path, const char* interface,
							const char* member);
			Message		Reply() const;
			Message		ErrorReply(const char* name,
							const char* text) const;
};


// signature helpers
bool		signature_is_valid(const char* signature);
// the length of the first complete type in \a signature, 0 if invalid
size_t		single_type_length(const char* signature);


}	// namespace DBus


#endif	// DBUS_MESSAGE_H
