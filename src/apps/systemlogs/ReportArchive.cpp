/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ReportArchive.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include <zlib.h>

#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>

#include <crash_report_format.h>


namespace {


struct ZipEntry {
	std::string	name;
	uint32		crc;
	uint32		compressedSize;
	uint32		size;
	uint32		offset;
	uint16		method;
};


/*!	A minimal ZIP writer: one file after the other, each deflated in one go
	(raw deflate, method 8) or stored when that is not smaller, then the
	central directory. No ZIP64: the logs are far below 4 GiB. Format from
	PKWARE's APPNOTE.TXT.
*/
class ZipWriter {
public:
	ZipWriter()
		:
		fFile(NULL),
		fOffset(0),
		fError(false)
	{
	}

	~ZipWriter()
	{
		if (fFile != NULL)
			fclose(fFile);
	}

	status_t Open(const char* path)
	{
		fFile = fopen(path, "wb");
		if (fFile == NULL)
			return errno;
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		fDosTime = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
		fDosDate = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5)
			| tm.tm_mday;
		return B_OK;
	}

	void Add(const std::string& name, const std::string& data)
	{
		ZipEntry entry;
		entry.name = name;
		entry.size = data.size();
		entry.crc = crc32(0, (const Bytef*)data.data(), data.size());
		entry.offset = fOffset;

		std::string compressed;
		uLongf bound = compressBound(data.size()) + 64;
		compressed.resize(bound);
		z_stream stream;
		memset(&stream, 0, sizeof(stream));
		bool deflated = false;
		if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8,
				Z_DEFAULT_STRATEGY) == Z_OK) {
			stream.next_in = (Bytef*)data.data();
			stream.avail_in = data.size();
			stream.next_out = (Bytef*)&compressed[0];
			stream.avail_out = bound;
			if (deflate(&stream, Z_FINISH) == Z_STREAM_END) {
				compressed.resize(stream.total_out);
				deflated = compressed.size() < data.size();
			}
			deflateEnd(&stream);
		}
		if (!deflated)
			compressed = data;
		entry.method = deflated ? 8 : 0;
		entry.compressedSize = compressed.size();

		_Write32(0x04034b50);
		_Write16(20);					// version needed
		_Write16(0x0800);				// UTF-8 names
		_Write16(entry.method);
		_Write16(fDosTime);
		_Write16(fDosDate);
		_Write32(entry.crc);
		_Write32(entry.compressedSize);
		_Write32(entry.size);
		_Write16(name.size());
		_Write16(0);
		_WriteBytes(name.data(), name.size());
		_WriteBytes(compressed.data(), compressed.size());
		fEntries.push_back(entry);
	}

	status_t Close()
	{
		uint32 directoryStart = fOffset;
		for (size_t i = 0; i < fEntries.size(); i++) {
			const ZipEntry& entry = fEntries[i];
			_Write32(0x02014b50);
			_Write16(0x0314);			// made by: Unix, 2.0
			_Write16(20);
			_Write16(0x0800);
			_Write16(entry.method);
			_Write16(fDosTime);
			_Write16(fDosDate);
			_Write32(entry.crc);
			_Write32(entry.compressedSize);
			_Write32(entry.size);
			_Write16(entry.name.size());
			_Write16(0);				// extra
			_Write16(0);				// comment
			_Write16(0);				// disk
			_Write16(0);				// internal attributes
			_Write32(0644 << 16);		// external: Unix mode
			_Write32(entry.offset);
			_WriteBytes(entry.name.data(), entry.name.size());
		}
		uint32 directorySize = fOffset - directoryStart;
		_Write32(0x06054b50);
		_Write16(0);
		_Write16(0);
		_Write16(fEntries.size());
		_Write16(fEntries.size());
		_Write32(directorySize);
		_Write32(directoryStart);
		_Write16(0);

		bool error = fError;
		if (fclose(fFile) != 0)
			error = true;
		fFile = NULL;
		return error ? B_IO_ERROR : B_OK;
	}

	size_t CountEntries() const { return fEntries.size(); }

private:
	void _WriteBytes(const void* data, size_t size)
	{
		if (size > 0 && fwrite(data, 1, size, fFile) != size)
			fError = true;
		fOffset += size;
	}

	void _Write16(uint16 value)
	{
		uint8 bytes[2] = { (uint8)value, (uint8)(value >> 8) };
		_WriteBytes(bytes, 2);
	}

	void _Write32(uint32 value)
	{
		uint8 bytes[4] = { (uint8)value, (uint8)(value >> 8),
			(uint8)(value >> 16), (uint8)(value >> 24) };
		_WriteBytes(bytes, 4);
	}

	FILE*					fFile;
	uint32					fOffset;
	bool					fError;
	uint16					fDosTime;
	uint16					fDosDate;
	std::vector<ZipEntry>	fEntries;
};


bool
read_file(const char* path, std::string& contents)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	char buffer[65536];
	size_t bytesRead;
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0)
		contents.append(buffer, bytesRead);
	fclose(file);
	return true;
}


std::string
command_output(const char* command)
{
	std::string output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return std::string("could not run ") + command + "\n";
	char buffer[4096];
	size_t bytesRead;
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, bytesRead);
	int status = pclose(pipe);
	if (status != 0) {
		char note[64];
		snprintf(note, sizeof(note), "\n(exit status %d)\n", status);
		output += note;
	}
	return output;
}


}	// namespace


BString
default_report_path()
{
	BPath path;
	if (find_directory(B_DESKTOP_DIRECTORY, &path, true) != B_OK)
		path.SetTo("/boot/home/Desktop");
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char name[64];
	strftime(name, sizeof(name), "system-report-%Y-%m-%d_%H-%M-%S.zip", &tm);
	path.Append(name);
	return BString(path.Path());
}


status_t
save_report_archive(const char* path, BString* _summary)
{
	ZipWriter zip;
	status_t status = zip.Open(path);
	if (status != B_OK)
		return status;

	BString summary;
	BPath logDirectory;
	if (find_directory(B_SYSTEM_LOG_DIRECTORY, &logDirectory) != B_OK)
		logDirectory.SetTo("/boot/system/var/log");

	static const char* kLogs[] = { "syslog.1", "syslog", "previous_syslog",
		NULL };
	for (int32 i = 0; kLogs[i] != NULL; i++) {
		BPath logPath(logDirectory);
		logPath.Append(kLogs[i]);
		std::string contents;
		if (read_file(logPath.Path(), contents)) {
			zip.Add(std::string("logs/") + kLogs[i], contents);
			summary << "logs/" << kLogs[i] << "\n";
		}
	}

	BDirectory crashDirectory(CRASH_REPORT_DIRECTORY);
	BEntry entry;
	while (crashDirectory.GetNextEntry(&entry) == B_OK) {
		BPath crashPath;
		if (entry.GetPath(&crashPath) != B_OK || !entry.IsFile())
			continue;
		std::string contents;
		if (read_file(crashPath.Path(), contents)) {
			zip.Add(std::string("crash/") + crashPath.Leaf(), contents);
			summary << "crash/" << crashPath.Leaf() << "\n";
		}
	}

	// Hardware and system information, from the tools a bug report usually
	// asks for.
	static const struct {
		const char*	name;
		const char*	command;
	} kCommands[] = {
		{ "hardware/listdev.txt", "/bin/listdev 2>&1" },
		{ "hardware/listusb.txt", "/bin/listusb -v 2>&1" },
		{ "hardware/sysinfo.txt", "/bin/sysinfo 2>&1" },
		{ "hardware/uname.txt", "/bin/uname -a 2>&1" },
		{ "hardware/df.txt", "/bin/df 2>&1" },
		{ NULL, NULL }
	};
	for (int32 i = 0; kCommands[i].name != NULL; i++) {
		zip.Add(kCommands[i].name, command_output(kCommands[i].command));
		summary << kCommands[i].name << "\n";
	}

	std::string readme = "System report made by System logs.\n\n"
		"logs/      the system log (syslog, syslog.1 is the older part) and the\n"
		"           kernel's log of the previous session (previous_syslog)\n"
		"crash/     kernel crash reports (kernel-*.txt) and application crash\n"
		"           reports (app-*.txt) from /boot/system/var/log/crash\n"
		"hardware/  the output of listdev, listusb -v, sysinfo, uname -a, df\n";
	zip.Add("README.txt", readme);

	status = zip.Close();
	if (status != B_OK) {
		unlink(path);
		return status;
	}
	if (_summary != NULL)
		*_summary = summary;
	return B_OK;
}
