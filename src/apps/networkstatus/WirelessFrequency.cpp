/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WirelessFrequency.h"

#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

extern "C" {
#	include <compat/sys/cdefs.h>
#	include <compat/sys/ioccom.h>
#	include <net80211/ieee80211_ioctl.h>
}


/*!	The same request as FreeBSD's "ifconfig wlan0 channel": the channel of
	the access point while associated, else the one the card is on (which
	changes while it scans). Kept in its own file for the net80211 headers.
*/
uint32
wireless_frequency(const char* interfaceName)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 0;

	struct ieee80211_channel channel;
	memset(&channel, 0, sizeof(channel));
	struct ieee80211req request;
	memset(&request, 0, sizeof(request));
	strlcpy(request.i_name, interfaceName, IF_NAMESIZE);
	request.i_type = IEEE80211_IOC_CURCHAN;
	request.i_len = sizeof(channel);
	request.i_data = &channel;

	uint32 frequency = 0;
	if (ioctl(fd, SIOCG80211, &request, sizeof(request)) == 0)
		frequency = channel.ic_freq;
	close(fd);
	return frequency;
}
