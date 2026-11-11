/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Read-only queries to a running LinuxKPI driver (lkpi_control.h).
 */


#include "LinuxKPIManage.h"

#include <NetworkInterface.h>
#include <NetworkRoster.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <lkpi_control.h>


namespace LKPI {


status_t
KernelQuery::InterfacesFor(const Driver& driver, BStringList& out)
{
	out.MakeEmpty();
	if (driver.publish.IsEmpty())
		return B_OK;

	BString prefix("/dev/");
	prefix << driver.publish << "/";

	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		BString name(interface.Name());
		if (name.StartsWith(prefix))
			out.Add(name);
	}
	return B_OK;
}


status_t
KernelQuery::Query(const char* interface, uint32 op, BString& reply)
{
	if (interface == NULL || strlen(interface) >= LKPI_CONTROL_NAME_SIZE)
		return B_BAD_VALUE;
	if (op < LKPI_CTL_FIRST || op > LKPI_CTL_LAST)
		return B_BAD_VALUE;

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return errno;

	size_t size = 4096;
	status_t status = B_ERROR;
	for (int attempt = 0; attempt < 3; attempt++) {
		char* buffer = (char*)malloc(size);
		if (buffer == NULL) {
			status = B_NO_MEMORY;
			break;
		}

		lkpi_control_request request;
		memset(&request, 0, sizeof(request));
		strlcpy(request.name, interface, sizeof(request.name));
		request.magic = LKPI_CONTROL_MAGIC;
		request.version = LKPI_CONTROL_VERSION;
		request.buffer = (uint64)(addr_t)buffer;
		request.buffer_size = (uint32)size;

		if (ioctl(fd, op, &request, sizeof(request)) == 0) {
			buffer[min_c(request.reply_size, size - 1)] = '\0';
			reply = buffer;
			status = B_OK;
		} else {
			status = errno;
		}
		free(buffer);

		if (status == B_BUFFER_OVERFLOW && request.reply_size + 1 > size
			&& request.reply_size < LKPI_CONTROL_MAX_REPLY) {
			size = request.reply_size + 1;
			continue;
		}
		break;
	}
	close(fd);

	// A driver built before the control interface answers B_BAD_VALUE
	// (wlan_control()) or B_DEV_INVALID_IOCTL. The network stack also says
	// B_BAD_VALUE when the interface has gone away meanwhile, so tell the
	// two apart. (Our kernel side uses B_BAD_DATA for malformed requests.)
	if (status == B_DEV_INVALID_IOCTL)
		return B_NOT_SUPPORTED;
	if (status == B_BAD_VALUE) {
		BNetworkInterface check(interface);
		return check.Exists() ? B_NOT_SUPPORTED : B_ENTRY_NOT_FOUND;
	}
	return status;
}


status_t
KernelQuery::QueryValues(const char* interface, uint32 op, BStringList& keys,
	BStringList& values)
{
	BString reply;
	status_t status = Query(interface, op, reply);
	if (status != B_OK)
		return status;

	keys.MakeEmpty();
	values.MakeEmpty();
	BStringList lines;
	reply.Split("\n", true, lines);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		const BString& line = lines.StringAt(i);
		int32 eq = line.FindFirst('=');
		if (eq <= 0)
			continue;
		BString key, value;
		line.CopyInto(key, 0, eq);
		line.CopyInto(value, eq + 1, line.Length() - eq - 1);
		keys.Add(key);
		values.Add(value);
	}
	return B_OK;
}


}	// namespace LKPI
