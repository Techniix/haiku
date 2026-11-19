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


/*!	Renga: where a Wi-Fi link's speed goes ("lkpictl wifi-stats", the
	Wi-Fi tab of LinuxKPI Manager): link mode, channel and width, the
	driver's RX and TX rates, aggregation (A-MPDU block-ack sessions and
	A-MSDU), hardware encryption, the skb low-memory switch and DMA
	bounces, packet, drop and retry counts, and the data path's interrupt,
	NAPI and 802.11 receive counts with their time. Plain text, one fact
	per line, for people and for reports.
*/
static BString
value_of(const BStringList& keys, const BStringList& values, const char* key,
	const char* fallback = "-")
{
	int32 index = keys.IndexOf(key);
	return index >= 0 ? values.StringAt(index) : BString(fallback);
}


static void
add_line(BString& out, const char* label, const BString& value)
{
	BString line;
	line.SetToFormat("  %-24s %s\n", label, value.String());
	out << line;
}


static BString
per_event(const BString& totalUs, const BString& count, const char* unit)
{
	long long total = strtoll(totalUs.String(), NULL, 10);
	long long n = strtoll(count.String(), NULL, 10);
	BString text;
	if (n <= 0)
		text.SetToFormat("%s (none yet)", count.String());
	else {
		text.SetToFormat("%lld, %lld us in all, %.1f us per %s", n, total,
			(double)total / n, unit);
	}
	return text;
}


status_t
KernelQuery::WifiReport(const char* interface, BString& out)
{
	BStringList keys, values, tunKeys, tunValues;
	status_t status = QueryValues(interface, LKPI_CTL_GET_STATS, keys,
		values);
	if (status != B_OK)
		return status;
	QueryValues(interface, LKPI_CTL_GET_TUNABLES, tunKeys, tunValues);

	BString aggregation;
	for (int32 i = 0; i < keys.CountStrings(); i++) {
		const BString& key = keys.StringAt(i);
		if (key.StartsWith("wlan.rx_ba.") || key.StartsWith("wlan.tx_ba.")) {
			BString name(key.String() + 5);
			aggregation << name << " (" << values.StringAt(i) << ") ";
		}
	}

	out << "Link\n";
	add_line(out, "State", value_of(keys, values, "wlan.state"));
	add_line(out, "Mode", value_of(keys, values, "wlan.channel_mode"));
	BString channel;
	channel << value_of(keys, values, "wlan.channel") << " ("
		<< value_of(keys, values, "wlan.band") << ", "
		<< value_of(keys, values, "wlan.width_mhz") << " MHz)";
	add_line(out, "Channel", channel);
	add_line(out, "Station flags", value_of(keys, values, "wlan.node_flags"));
	add_line(out, "Signal (dBm)", value_of(keys, values, "wlan.signal_dbm"));
	out << "Rates (from the driver)\n";
	add_line(out, "Receive", value_of(keys, values, "wlan.rx_rate"));
	add_line(out, "Send", value_of(keys, values, "wlan.tx_rate"));
	out << "Aggregation and offloads\n";
	add_line(out, "Block-ack sessions", aggregation.IsEmpty()
		? BString("none (no A-MPDU in either direction)") : aggregation);
	add_line(out, "Hardware encryption",
		value_of(keys, values, "wlan.hw_crypto"));
	add_line(out, "skb low memory (297121)", value_of(tunKeys, tunValues,
		"compat.linuxkpi.skb.mem_limit", "default (off)"));
	add_line(out, "DMA maps / bounces", value_of(keys, values, "dma.maps")
		<< " / " << value_of(keys, values, "dma.bounces"));
	out << "Traffic\n";
	add_line(out, "Received packets", value_of(keys, values,
		"wlan.rx_packets") << ", dropped "
		<< value_of(keys, values, "wlan.rx_dropped") << ", errors "
		<< value_of(keys, values, "wlan.ierrors"));
	add_line(out, "Sent packets", value_of(keys, values, "wlan.tx_packets")
		<< ", failed " << value_of(keys, values, "wlan.tx_failed")
		<< ", retries " << value_of(keys, values, "wlan.tx_retries"));
	out << "Data path\n";
	add_line(out, "Interrupts", per_event(value_of(keys, values,
		"perf.irq_us", "0"), value_of(keys, values, "perf.irqs", "0"),
		"interrupt"));
	add_line(out, "NAPI polls", per_event(value_of(keys, values,
		"perf.napi_us", "0"), value_of(keys, values, "perf.napi_runs", "0"),
		"poll") << ", " << value_of(keys, values, "perf.napi_packets", "0")
		<< " packets");
	add_line(out, "802.11 receive", per_event(value_of(keys, values,
		"perf.rx80211_us", "0"),
		value_of(keys, values, "perf.rx80211_frames", "0"), "frame"));
	add_line(out, "To the network stack", per_event(value_of(keys, values,
		"perf.rxq_us", "0"), value_of(keys, values, "perf.rxq_frames", "0"),
		"frame"));
	return B_OK;
}


}	// namespace LKPI
