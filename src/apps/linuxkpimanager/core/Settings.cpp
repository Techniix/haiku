/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * The driver's settings file, read by lkpi_tunables_fetch() in the kernel
 * when the driver loads. We only write keys the manifest describes, with
 * values its TunableSpec accepts, and we keep every other line (comments,
 * keys we do not know) exactly as it was.
 */


#include "LinuxKPIManage.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <driver_settings.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


namespace LKPI {


status_t
SettingsStore::PathFor(const Driver& driver, BString& _path)
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	path.Append("kernel/drivers");
	path.Append(driver.name.String());
	_path = path.Path();
	return B_OK;
}


status_t
SettingsStore::Read(const Driver& driver, BStringList& keys,
	BStringList& values)
{
	keys.MakeEmpty();
	values.MakeEmpty();

	BString path;
	status_t status = PathFor(driver, path);
	if (status != B_OK)
		return status;
	void* handle = load_driver_settings(path.String());
	if (handle == NULL)
		return B_ENTRY_NOT_FOUND;
	const driver_settings* settings = get_driver_settings(handle);
	// The kernel's get_driver_parameter() uses the LAST occurrence of a
	// key (driver_settings.cpp searches backwards), so later lines win here.
	for (int i = 0; settings != NULL && i < settings->parameter_count; i++) {
		const driver_parameter& p = settings->parameters[i];
		BString value(p.value_count > 0 ? p.values[0] : "");
		int32 index = keys.IndexOf(p.name);
		if (index >= 0) {
			values.Replace(index, value);
			continue;
		}
		keys.Add(p.name);
		values.Add(value);
	}
	unload_driver_settings(handle);
	return B_OK;
}


static status_t
read_lines(const char* path, BStringList& lines)
{
	lines.MakeEmpty();
	BFile file(path, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return B_OK;	// no file yet: empty
	off_t size;
	if (file.GetSize(&size) != B_OK || size > 32 * 1024)
		return B_BAD_DATA;
	BString text;
	char* buffer = text.LockBuffer(size + 1);
	ssize_t bytes = file.Read(buffer, size);
	text.UnlockBuffer(bytes > 0 ? bytes : 0);
	text.Split("\n", false, lines);
	if (lines.CountStrings() > 0
		&& lines.StringAt(lines.CountStrings() - 1).IsEmpty())
		lines.Remove(lines.CountStrings() - 1);
	return B_OK;
}


// Rewrites the file atomically: write to "<path>.new", then rename.
static status_t
write_lines(const char* path, const BStringList& lines)
{
	BPath parent;
	BPath(path).GetParent(&parent);
	create_directory(parent.Path(), 0755);

	BString temp(path);
	temp << ".new";
	BFile file(temp.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BString line(lines.StringAt(i));
		line << "\n";
		if (file.Write(line.String(), line.Length()) != line.Length())
			return B_IO_ERROR;
	}
	file.Sync();
	file.Unset();
	if (rename(temp.String(), path) != 0)
		return errno;
	return B_OK;
}


// Index of the line that sets "key" ("key value", ignoring leading blanks).
static int32
find_key_line(const BStringList& lines, const char* key)
{
	size_t len = strlen(key);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		const char* s = lines.StringAt(i).String();
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, key, len) == 0
			&& (s[len] == ' ' || s[len] == '\t' || s[len] == '\0'))
			return i;
	}
	return -1;
}


status_t
SettingsStore::Set(const Driver& driver, const char* key, const char* value,
	BString& error)
{
	const TunableSpec* spec = driver.FindTunable(key);
	if (spec == NULL) {
		error.SetToFormat("%s is not a setting of %s.", key,
			driver.name.String());
		return B_BAD_VALUE;
	}
	if (!spec->Validate(value, error))
		return B_BAD_VALUE;

	BString path;
	status_t status = PathFor(driver, path);
	if (status != B_OK)
		return status;
	BStringList lines;
	if ((status = read_lines(path.String(), lines)) != B_OK) {
		error = "The settings file is too large or unreadable.";
		return status;
	}

	// One line per key: drop every earlier occurrence (the kernel would use
	// the last one), then add ours.
	BString line;
	line << key << " " << value;
	int32 index;
	while ((index = find_key_line(lines, key)) >= 0)
		lines.Remove(index);
	lines.Add(line);

	status = write_lines(path.String(), lines);
	if (status != B_OK)
		error.SetToFormat("Could not write %s: %s", path.String(),
			strerror(status));
	return status;
}


status_t
SettingsStore::Unset(const Driver& driver, const char* key, BString& error)
{
	const TunableSpec* spec = driver.FindTunable(key);
	if (spec == NULL) {
		error.SetToFormat("%s is not a setting of %s.", key,
			driver.name.String());
		return B_BAD_VALUE;
	}
	if (spec->risk == kRiskUnsafe) {
		error = "This setting cannot be changed from here.";
		return B_NOT_ALLOWED;
	}

	BString path;
	status_t status = PathFor(driver, path);
	if (status != B_OK)
		return status;
	BStringList lines;
	if ((status = read_lines(path.String(), lines)) != B_OK) {
		error = "The settings file is too large or unreadable.";
		return status;
	}
	int32 index;
	while ((index = find_key_line(lines, key)) >= 0)
		lines.Remove(index);

	if (lines.IsEmpty()) {
		if (unlink(path.String()) != 0 && errno != ENOENT) {
			error.SetToFormat("Could not remove %s.", path.String());
			return errno;
		}
		return B_OK;
	}
	status = write_lines(path.String(), lines);
	if (status != B_OK)
		error.SetToFormat("Could not write %s.", path.String());
	return status;
}


}	// namespace LKPI
