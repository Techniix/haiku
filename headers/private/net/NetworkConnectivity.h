/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NETWORK_CONNECTIVITY_H
#define _NETWORK_CONNECTIVITY_H


/*!	The internet check of the Deskbar network icon (NetworkStatus): one
	plain HTTP request to a well-known address that answers with a fixed
	reply. Another reply (a redirect, a sign-in page) means a captive
	portal; no reply means no internet. On by default; the Network
	preferences switch it off. Settings: ~/config/settings/network_connectivity

		check true|false
		url http://...
		interval <seconds>

	The default is Mozilla's address for Firefox's own check (also used by
	other browsers): run by a non-profit for this one purpose, answered
	worldwide, plain HTTP (a portal can only intercept that), and a reply
	that a portal cannot fake by accident ("success"). HTTP 204 replies
	(as from .../generate_204 addresses) count as well, so either kind
	can be set.

	Header only, so that the tests (tests/wifi) use the same code.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <NetworkAddress.h>
#include <OS.h>
#include <Path.h>
#include <String.h>


namespace BPrivate {


static const char* const kDefaultConnectivityURL
	= "http://detectportal.firefox.com/success.txt";
static const int32 kDefaultConnectivityInterval = 300;	// seconds
static const char* const kConnectivitySettingsName = "network_connectivity";


enum connectivity_result {
	CONNECTIVITY_ONLINE = 0,
	CONNECTIVITY_CAPTIVE,	// an answer, but not the expected one
	CONNECTIVITY_OFFLINE	// no name resolution, no connection or no answer
};


struct connectivity_settings {
	bool	enabled;
	BString	url;
	int32	interval;

	connectivity_settings()
		:
		enabled(true),
		url(kDefaultConnectivityURL),
		interval(kDefaultConnectivityInterval)
	{
	}
};


static inline bool
connectivity_settings_path(BPath& path)
{
	return find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append(kConnectivitySettingsName) == B_OK;
}


//!	Reads \a path (the settings file if NULL); missing values keep defaults.
static inline void
load_connectivity_settings(connectivity_settings& settings,
	const char* path = NULL)
{
	settings = connectivity_settings();
	BPath settingsPath;
	if (path == NULL) {
		if (!connectivity_settings_path(settingsPath))
			return;
		path = settingsPath.Path();
	}
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return;
	char line[512];
	while (fgets(line, sizeof(line), file) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "check ", 6) == 0)
			settings.enabled = strcmp(line + 6, "false") != 0;
		else if (strncmp(line, "url ", 4) == 0 && line[4] != '\0')
			settings.url = line + 4;
		else if (strncmp(line, "interval ", 9) == 0) {
			int32 interval = atoi(line + 9);
			if (interval >= 30)
				settings.interval = interval;
		}
	}
	fclose(file);
}


/*!	Writes the settings to a new file that then replaces the old one, so
	that a reader (NetworkStatus watches the file) never sees half of it.
*/
static inline status_t
save_connectivity_settings(const connectivity_settings& settings,
	const char* path = NULL)
{
	BPath settingsPath;
	if (path == NULL) {
		if (!connectivity_settings_path(settingsPath))
			return B_ERROR;
		path = settingsPath.Path();
	}
	BString temporary(path);
	temporary << ".new";
	FILE* file = fopen(temporary.String(), "w");
	if (file == NULL)
		return errno;
	fprintf(file, "# Network preferences: the internet check of the "
		"Deskbar network icon\ncheck %s\nurl %s\ninterval %" B_PRId32 "\n",
		settings.enabled ? "true" : "false", settings.url.String(),
		settings.interval);
	if (fclose(file) != 0 || rename(temporary.String(), path) != 0) {
		status_t error = errno;
		unlink(temporary.String());
		return error;
	}
	return B_OK;
}


//!	Splits an http:// URL (no https: a portal cannot answer that one).
static inline bool
parse_connectivity_url(const char* url, BString& host, uint16& port,
	BString& path)
{
	if (url == NULL || strncasecmp(url, "http://", 7) != 0)
		return false;
	const char* start = url + 7;
	const char* slash = strchr(start, '/');
	BString hostPort;
	if (slash != NULL) {
		hostPort.SetTo(start, slash - start);
		path = slash;
	} else {
		hostPort = start;
		path = "/";
	}
	port = 80;
	int32 colon = hostPort.FindLast(':');
	if (colon >= 0) {
		int value = atoi(hostPort.String() + colon + 1);
		if (value <= 0 || value > 65535)
			return false;
		port = (uint16)value;
		hostPort.Truncate(colon);
	}
	host = hostPort;
	return !host.IsEmpty();
}


/*!	Judges an HTTP reply: 204, or 200 with a body that starts "success"
	(Mozilla's address) means online; anything else is a portal.
*/
static inline connectivity_result
connectivity_result_for_reply(const char* reply, size_t length)
{
	if (length < 12 || strncmp(reply, "HTTP/1.", 7) != 0)
		return length == 0 ? CONNECTIVITY_OFFLINE : CONNECTIVITY_CAPTIVE;
	int status = atoi(reply + 9);
	if (status == 204)
		return CONNECTIVITY_ONLINE;
	if (status != 200)
		return CONNECTIVITY_CAPTIVE;
	BString text(reply, length);
	int32 body = text.FindFirst("\r\n\r\n");
	if (body < 0)
		return CONNECTIVITY_CAPTIVE;
	return strncmp(text.String() + body + 4, "success", 7) == 0
		? CONNECTIVITY_ONLINE : CONNECTIVITY_CAPTIVE;
}


/*!	Runs the check; blocks for up to about \a timeout (name resolution
	can take longer), so call it from a thread of its own.
*/
static inline connectivity_result
check_connectivity(const char* url, bigtime_t timeout = 5000000)
{
	BString host, path;
	uint16 port;
	if (!parse_connectivity_url(url, host, port, path))
		return CONNECTIVITY_OFFLINE;

	BNetworkAddress address;
	if (address.SetTo(host.String(), port) != B_OK)
		return CONNECTIVITY_OFFLINE;

	int fd = socket(address.Family(), SOCK_STREAM, 0);
	if (fd < 0)
		return CONNECTIVITY_OFFLINE;

	struct timeval limit;
	limit.tv_sec = timeout / 1000000;
	limit.tv_usec = timeout % 1000000;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));

	// connect without blocking, then wait at most the timeout
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	int result = connect(fd, address, address.Length());
	if (result < 0 && errno == EINPROGRESS) {
		fd_set writable;
		FD_ZERO(&writable);
		FD_SET(fd, &writable);
		struct timeval wait = limit;
		int error = 0;
		socklen_t errorLength = sizeof(error);
		if (select(fd + 1, NULL, &writable, NULL, &wait) == 1
			&& getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength) == 0
			&& error == 0)
			result = 0;
	}
	fcntl(fd, F_SETFL, flags);
	if (result < 0) {
		close(fd);
		return CONNECTIVITY_OFFLINE;
	}

	BString request;
	request << "GET " << path << " HTTP/1.1\r\nHost: " << host;
	if (port != 80)
		request << ":" << port;
	request << "\r\nUser-Agent: NetworkStatus\r\nCache-Control: no-cache"
		"\r\nConnection: close\r\n\r\n";
	if (send(fd, request.String(), request.Length(), 0)
			!= (ssize_t)request.Length()) {
		close(fd);
		return CONNECTIVITY_OFFLINE;
	}

	char reply[2048];
	size_t length = 0;
	bigtime_t end = system_time() + timeout;
	while (length < sizeof(reply) - 1 && system_time() < end) {
		ssize_t bytes = recv(fd, reply + length, sizeof(reply) - 1 - length,
			0);
		if (bytes <= 0)
			break;
		length += bytes;
	}
	close(fd);
	reply[length] = '\0';
	return connectivity_result_for_reply(reply, length);
}


}	// namespace BPrivate


#endif	// _NETWORK_CONNECTIVITY_H
