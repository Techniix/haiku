/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

//!	D-Bus wire format; see DBusMessage.h.


#include "DBusMessage.h"

#include <string.h>


namespace DBus {


// #pragma mark - signatures


static size_t
alignment_of(char type)
{
	switch (type) {
		case 'y': case 'g': case 'v':
			return 1;
		case 'n': case 'q':
			return 2;
		case 'b': case 'i': case 'u': case 'h': case 's': case 'o':
		case 'a':
			return 4;
		case 'x': case 't': case 'd': case '(': case '{':
			return 8;
	}
	return 1;
}


static size_t
single_type_length_depth(const char* signature, int depth)
{
	if (depth > 32)
		return 0;
	switch (signature[0]) {
		case 'y': case 'b': case 'n': case 'q': case 'i': case 'u':
		case 'x': case 't': case 'd': case 'h': case 's': case 'o':
		case 'g': case 'v':
			return 1;
		case 'a':
		{
			size_t length = single_type_length_depth(signature + 1,
				depth + 1);
			return length == 0 ? 0 : length + 1;
		}
		case '(':
		{
			size_t at = 1;
			while (signature[at] != ')') {
				size_t length = single_type_length_depth(signature + at,
					depth + 1);
				if (length == 0)
					return 0;
				at += length;
			}
			return at == 1 ? 0 : at + 1;
		}
		case '{':
		{
			// a basic key type, then one complete type
			if (strchr("ybnqiuxtdhsog", signature[1]) == NULL
				|| signature[1] == '\0')
				return 0;
			size_t length = single_type_length_depth(signature + 2,
				depth + 1);
			if (length == 0 || signature[2 + length] != '}')
				return 0;
			return length + 3;
		}
	}
	return 0;
}


size_t
single_type_length(const char* signature)
{
	return single_type_length_depth(signature, 0);
}


bool
signature_is_valid(const char* signature)
{
	size_t length = strlen(signature);
	if (length > 255)
		return false;
	size_t at = 0;
	while (at < length) {
		size_t one = single_type_length(signature + at);
		if (one == 0 || signature[at] == '{')
			return false;
		at += one;
	}
	return true;
}


// #pragma mark - DValue


DValue DValue::Byte(uint8 value)
	{ DValue v; v.type = 'y'; v.u = value; return v; }
DValue DValue::Bool(bool value)
	{ DValue v; v.type = 'b'; v.u = value ? 1 : 0; return v; }
DValue DValue::Int32(int32 value)
	{ DValue v; v.type = 'i'; v.i = value; return v; }
DValue DValue::UInt32(uint32 value)
	{ DValue v; v.type = 'u'; v.u = value; return v; }
DValue DValue::Int64(int64 value)
	{ DValue v; v.type = 'x'; v.i = value; return v; }
DValue DValue::UInt64(uint64 value)
	{ DValue v; v.type = 't'; v.u = value; return v; }
DValue DValue::Double(double value)
	{ DValue v; v.type = 'd'; v.d = value; return v; }
DValue DValue::String(const char* value)
	{ DValue v; v.type = 's'; v.s = value; return v; }
DValue DValue::ObjectPath(const char* value)
	{ DValue v; v.type = 'o'; v.s = value; return v; }
DValue DValue::Signature(const char* value)
	{ DValue v; v.type = 'g'; v.s = value; return v; }
DValue DValue::Array(const char* elementSignature)
	{ DValue v; v.type = 'a'; v.signature = elementSignature; return v; }
DValue DValue::Struct()
	{ DValue v; v.type = '('; return v; }


DValue
DValue::DictEntry(const DValue& key, const DValue& value)
{
	DValue v;
	v.type = '{';
	v.items.push_back(key);
	v.items.push_back(value);
	return v;
}


DValue
DValue::Variant(const DValue& value)
{
	DValue v;
	v.type = 'v';
	v.signature = value.Signature();
	v.items.push_back(value);
	return v;
}


BString
DValue::Signature() const
{
	BString result;
	switch (type) {
		case 'a':
			result << "a" << signature;
			break;
		case '(':
			result << "(";
			for (size_t i = 0; i < items.size(); i++)
				result << items[i].Signature();
			result << ")";
			break;
		case '{':
			result << "{";
			for (size_t i = 0; i < items.size(); i++)
				result << items[i].Signature();
			result << "}";
			break;
		default:
			result << type;
	}
	return result;
}


const DValue*
DValue::Find(const char* key) const
{
	if (type != 'a')
		return NULL;
	for (size_t i = 0; i < items.size(); i++) {
		const DValue& entry = items[i];
		if (entry.type == '{' && entry.items.size() == 2
			&& entry.items[0].s == key)
			return &entry.items[1];
	}
	return NULL;
}


const DValue&
DValue::Unwrapped() const
{
	if (type == 'v' && items.size() == 1)
		return items[0].Unwrapped();
	return *this;
}


// #pragma mark - writer


class Writer {
public:
	Writer(std::vector<uint8>& out)
		:
		fOut(out)
	{
	}

	void Align(size_t alignment)
	{
		while (fOut.size() % alignment != 0)
			fOut.push_back(0);
	}

	void Raw(const void* data, size_t size)
	{
		const uint8* bytes = (const uint8*)data;
		fOut.insert(fOut.end(), bytes, bytes + size);
	}

	void UInt8(uint8 value) { fOut.push_back(value); }
	void UInt16(uint16 value) { Align(2); Raw(&value, 2); }
	void UInt32(uint32 value) { Align(4); Raw(&value, 4); }
	void UInt64(uint64 value) { Align(8); Raw(&value, 8); }

	void String(const BString& value)
	{
		UInt32(value.Length());
		Raw(value.String(), value.Length() + 1);
	}

	void Signature(const BString& value)
	{
		UInt8(value.Length());
		Raw(value.String(), value.Length() + 1);
	}

	bool Value(const char* signature, const DValue& value)
	{
		char type = signature[0];
		switch (type) {
			case 'y': UInt8(value.u); return true;
			case 'b': UInt32(value.u != 0); return true;
			case 'n': UInt16((uint16)value.i); return true;
			case 'q': UInt16((uint16)value.u); return true;
			case 'i': UInt32((uint32)value.i); return true;
			case 'u': case 'h': UInt32((uint32)value.u); return true;
			case 'x': UInt64((uint64)value.i); return true;
			case 't': UInt64(value.u); return true;
			case 'd':
			{
				Align(8);
				Raw(&value.d, 8);
				return true;
			}
			case 's': case 'o': String(value.s); return true;
			case 'g': Signature(value.s); return true;
			case 'v':
			{
				if (value.items.size() != 1)
					return false;
				BString inner = value.items[0].Signature();
				Signature(inner);
				return Value(inner.String(), value.items[0]);
			}
			case 'a':
			{
				const char* element = signature + 1;
				size_t elementLength = single_type_length(element);
				BString elementSignature(element, elementLength);
				UInt32(0);
				size_t lengthAt = fOut.size() - 4;
				Align(alignment_of(element[0]));
				size_t start = fOut.size();
				for (size_t i = 0; i < value.items.size(); i++) {
					if (!Value(elementSignature.String(), value.items[i]))
						return false;
				}
				uint32 length = fOut.size() - start;
				memcpy(&fOut[lengthAt], &length, 4);
				return true;
			}
			case '(':
			case '{':
			{
				Align(8);
				const char* member = signature + 1;
				for (size_t i = 0; i < value.items.size(); i++) {
					size_t length = single_type_length(member);
					if (length == 0)
						return false;
					BString memberSignature(member, length);
					if (!Value(memberSignature.String(), value.items[i]))
						return false;
					member += length;
				}
				return true;
			}
		}
		return false;
	}

private:
	std::vector<uint8>&	fOut;
};


// #pragma mark - reader


class Reader {
public:
	Reader(const uint8* data, size_t size, size_t offset, bool bigEndian,
		size_t base)
		:
		fData(data),
		fSize(size),
		fPosition(offset),
		fBase(base),
		fBigEndian(bigEndian),
		fError(false)
	{
	}

	bool Error() const { return fError; }
	size_t Position() const { return fPosition; }

	void Align(size_t alignment)
	{
		size_t relative = fPosition - fBase;
		size_t padded = (relative + alignment - 1) / alignment * alignment;
		if (fBase + padded > fSize)
			fError = true;
		else
			fPosition = fBase + padded;
	}

	bool Need(size_t size)
	{
		if (fError || fPosition + size > fSize) {
			fError = true;
			return false;
		}
		return true;
	}

	uint8 UInt8()
	{
		if (!Need(1))
			return 0;
		return fData[fPosition++];
	}

	uint16 UInt16()
	{
		Align(2);
		if (!Need(2))
			return 0;
		uint16 value;
		memcpy(&value, fData + fPosition, 2);
		fPosition += 2;
		return fBigEndian ? __builtin_bswap16(value) : value;
	}

	uint32 UInt32()
	{
		Align(4);
		if (!Need(4))
			return 0;
		uint32 value;
		memcpy(&value, fData + fPosition, 4);
		fPosition += 4;
		return fBigEndian ? __builtin_bswap32(value) : value;
	}

	uint64 UInt64()
	{
		Align(8);
		if (!Need(8))
			return 0;
		uint64 value;
		memcpy(&value, fData + fPosition, 8);
		fPosition += 8;
		return fBigEndian ? __builtin_bswap64(value) : value;
	}

	BString String()
	{
		uint32 length = UInt32();
		if (!Need((size_t)length + 1) || fData[fPosition + length] != '\0') {
			fError = true;
			return BString();
		}
		BString value((const char*)fData + fPosition, length);
		fPosition += length + 1;
		return value;
	}

	BString Signature()
	{
		uint8 length = UInt8();
		if (!Need((size_t)length + 1) || fData[fPosition + length] != '\0') {
			fError = true;
			return BString();
		}
		BString value((const char*)fData + fPosition, length);
		fPosition += length + 1;
		return value;
	}

	bool Value(const char* signature, DValue& value, int depth = 0)
	{
		if (depth > 64) {
			fError = true;
			return false;
		}
		char type = signature[0];
		value = DValue();
		value.type = type;
		switch (type) {
			case 'y': value.u = UInt8(); break;
			case 'b': value.u = UInt32() != 0; break;
			case 'n': value.i = (int16)UInt16(); break;
			case 'q': value.u = UInt16(); break;
			case 'i': value.i = (int32)UInt32(); break;
			case 'u': case 'h': value.u = UInt32(); break;
			case 'x': value.i = (int64)UInt64(); break;
			case 't': value.u = UInt64(); break;
			case 'd':
			{
				uint64 bits = UInt64();
				memcpy(&value.d, &bits, 8);
				break;
			}
			case 's': case 'o': value.s = String(); break;
			case 'g': value.s = Signature(); break;
			case 'v':
			{
				BString inner = Signature();
				if (fError || single_type_length(inner.String())
						!= (size_t)inner.Length()) {
					fError = true;
					return false;
				}
				value.signature = inner;
				DValue content;
				if (!Value(inner.String(), content, depth + 1))
					return false;
				value.items.push_back(content);
				break;
			}
			case 'a':
			{
				const char* element = signature + 1;
				size_t elementLength = single_type_length(element);
				BString elementSignature(element, elementLength);
				value.signature = elementSignature;
				uint32 length = UInt32();
				if (length > kMaxMessageSize) {
					fError = true;
					return false;
				}
				Align(alignment_of(element[0]));
				size_t end = fPosition + length;
				if (end > fSize) {
					fError = true;
					return false;
				}
				while (!fError && fPosition < end) {
					DValue item;
					if (!Value(elementSignature.String(), item, depth + 1))
						return false;
					value.items.push_back(item);
				}
				if (fPosition != end)
					fError = true;
				break;
			}
			case '(':
			case '{':
			{
				Align(8);
				const char* member = signature + 1;
				while (*member != ')' && *member != '}' && *member != '\0') {
					size_t length = single_type_length(member);
					if (length == 0) {
						fError = true;
						return false;
					}
					BString memberSignature(member, length);
					DValue item;
					if (!Value(memberSignature.String(), item, depth + 1))
						return false;
					value.items.push_back(item);
					member += length;
				}
				break;
			}
			default:
				fError = true;
		}
		return !fError;
	}

private:
	const uint8*	fData;
	size_t			fSize;
	size_t			fPosition;
	size_t			fBase;
	bool			fBigEndian;
	bool			fError;
};


// #pragma mark - Message


Message::Message()
	:
	type(0),
	flags(0),
	serial(0),
	replySerial(0),
	unixFDCount(0),
	bigEndianBody(false)
{
}


/*static*/ ssize_t
Message::NeededSize(const uint8* data, size_t size)
{
	if (size < 16)
		return 0;
	bool bigEndian = data[0] == 'B';
	if (data[0] != 'l' && data[0] != 'B')
		return B_BAD_DATA;
	if (data[3] != 1)
		return B_BAD_DATA;
	uint32 bodyLength, fieldsLength;
	memcpy(&bodyLength, data + 4, 4);
	memcpy(&fieldsLength, data + 12, 4);
	if (bigEndian) {
		bodyLength = __builtin_bswap32(bodyLength);
		fieldsLength = __builtin_bswap32(fieldsLength);
	}
	uint64 headerEnd = 16 + (uint64)fieldsLength;
	headerEnd = (headerEnd + 7) & ~(uint64)7;
	uint64 total = headerEnd + bodyLength;
	if (total > kMaxMessageSize)
		return B_BAD_DATA;
	return (ssize_t)total;
}


status_t
Message::Parse(const uint8* data, size_t size)
{
	ssize_t needed = NeededSize(data, size);
	if (needed <= 0 || (size_t)needed > size)
		return B_BAD_DATA;
	bool bigEndian = data[0] == 'B';
	type = data[1];
	flags = data[2];
	if (type < METHOD_CALL || type > SIGNAL)
		return B_BAD_DATA;

	Reader reader(data, size, 4, bigEndian, 0);
	uint32 bodyLength = reader.UInt32();
	serial = reader.UInt32();
	DValue fields;
	if (!reader.Value("a(yv)", fields))
		return B_BAD_DATA;
	for (size_t i = 0; i < fields.items.size(); i++) {
		const DValue& field = fields.items[i];
		if (field.items.size() != 2)
			return B_BAD_DATA;
		const DValue& content = field.items[1].Unwrapped();
		switch (field.items[0].u) {
			case 1: path = content.s; break;
			case 2: interface = content.s; break;
			case 3: member = content.s; break;
			case 4: errorName = content.s; break;
			case 5: replySerial = content.u; break;
			case 6: destination = content.s; break;
			case 7: sender = content.s; break;
			case 8: signature = content.s; break;
			case 9: unixFDCount = content.u; break;
		}
	}
	reader.Align(8);
	if (reader.Error() || reader.Position() + bodyLength != (size_t)needed)
		return B_BAD_DATA;
	body.assign(data + reader.Position(), data + needed);
	bigEndianBody = bigEndian;
	if (serial == 0)
		return B_BAD_DATA;
	if (!signature_is_valid(signature.String()))
		return B_BAD_DATA;
	return B_OK;
}


void
Message::Serialize(std::vector<uint8>& out) const
{
	out.clear();
	Writer writer(out);
	writer.UInt8('l');
	writer.UInt8(type);
	writer.UInt8(flags);
	writer.UInt8(1);
	writer.UInt32(body.size());
	writer.UInt32(serial);

	DValue fields = DValue::Array("(yv)");
	struct { uint8 code; char kind; const BString* text; } kTexts[] = {
		{ 1, 'o', &path }, { 2, 's', &interface }, { 3, 's', &member },
		{ 4, 's', &errorName }, { 6, 's', &destination },
		{ 7, 's', &sender }, { 8, 'g', &signature } };
	for (size_t i = 0; i < sizeof(kTexts) / sizeof(kTexts[0]); i++) {
		if (kTexts[i].text->IsEmpty())
			continue;
		DValue value;
		value.type = kTexts[i].kind;
		value.s = *kTexts[i].text;
		DValue field = DValue::Struct();
		field.Add(DValue::Byte(kTexts[i].code)).Add(DValue::Variant(value));
		fields.Add(field);
	}
	if (replySerial != 0) {
		DValue field = DValue::Struct();
		field.Add(DValue::Byte(5)).Add(DValue::Variant(
			DValue::UInt32(replySerial)));
		fields.Add(field);
	}
	uint32 fdCount = fds.size() > 0 ? fds.size() : unixFDCount;
	if (fdCount != 0) {
		DValue field = DValue::Struct();
		field.Add(DValue::Byte(9)).Add(DValue::Variant(
			DValue::UInt32(fdCount)));
		fields.Add(field);
	}
	writer.Value("a(yv)", fields);
	writer.Align(8);
	out.insert(out.end(), body.begin(), body.end());
}


status_t
Message::GetArguments(DValueList& values) const
{
	values.clear();
	Reader reader(body.data(), body.size(), 0, bigEndianBody, 0);
	const char* type = signature.String();
	while (*type != '\0') {
		size_t length = single_type_length(type);
		if (length == 0)
			return B_BAD_DATA;
		BString one(type, length);
		DValue value;
		if (!reader.Value(one.String(), value))
			return B_BAD_DATA;
		values.push_back(value);
		type += length;
	}
	return B_OK;
}


void
Message::SetArguments(const DValueList& values)
{
	body.clear();
	signature = "";
	Writer writer(body);
	for (size_t i = 0; i < values.size(); i++) {
		BString one = values[i].Signature();
		writer.Value(one.String(), values[i]);
		signature << one;
	}
	bigEndianBody = false;
}


/*static*/ Message
Message::MethodCall(const char* destination, const char* path,
	const char* interface, const char* member)
{
	Message message;
	message.type = METHOD_CALL;
	message.destination = destination;
	message.path = path;
	message.interface = interface;
	message.member = member;
	return message;
}


/*static*/ Message
Message::Signal(const char* path, const char* interface, const char* member)
{
	Message message;
	message.type = SIGNAL;
	message.flags = NO_REPLY_EXPECTED;
	message.path = path;
	message.interface = interface;
	message.member = member;
	return message;
}


Message
Message::Reply() const
{
	Message reply;
	reply.type = METHOD_RETURN;
	reply.flags = NO_REPLY_EXPECTED;
	reply.replySerial = serial;
	reply.destination = sender;
	return reply;
}


Message
Message::ErrorReply(const char* name, const char* text) const
{
	Message reply;
	reply.type = ERROR;
	reply.flags = NO_REPLY_EXPECTED;
	reply.replySerial = serial;
	reply.destination = sender;
	reply.errorName = name;
	DValueList values;
	values.push_back(DValue::String(text));
	reply.SetArguments(values);
	return reply;
}


}	// namespace DBus
