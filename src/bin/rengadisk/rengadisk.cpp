/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	rengadisk (Renga, F-24 first part): the shared disk-management
	engine (headers/private/diskengine/DiskEngine.h) on the command line.

	  rengadisk list
	      every disk: path, size, partition table, image file, boot disk,
	      and its partitions with file system, name, mount point and the
	      operating system found on it
	  rengadisk attach <image file>      the image as a disk; prints its path
	  rengadisk detach <image file>
	  rengadisk plan <disk> <step>... [--apply] [--allow-real-disks]
	          [--backup-dir <dir>]
	      checks the steps and shows what would change; --apply saves the
	      partition table and applies them. Steps:
	        table:gpt | table:mbr             new partition table
	        create:<size>[:<name>[:<type>]]   new partition (size 0: rest)
	        delete:<index>
	        format:<index>:<bfs|fat32|...>[:<name>]
	        name:<index>:<name>
	  rengadisk backup <disk> <file>       save the partition table
	  rengadisk restore <disk> <file>      write a saved table back (disk
	                                       images only; prints the image's
	                                       new disk path)

	Only disk images unless --allow-real-disks; never the boot disk.
	Exit status: 0 done, 1 refused or failed, 2 usage.
*/


#include <DiskEngine.h>

#include <FindDirectory.h>
#include <Path.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


using namespace RengaDisk;


static int
usage()
{
	fprintf(stderr, "usage: rengadisk list\n"
		"       rengadisk attach|detach <image file>\n"
		"       rengadisk plan <disk> <step>... [--apply] "
			"[--allow-real-disks] [--backup-dir <dir>]\n"
		"         steps: table:gpt|mbr  create:<size>[:<name>[:<type>]]  "
			"delete:<index>\n"
		"                format:<index>:<fs>[:<name>]  name:<index>:<name>\n"
		"       rengadisk backup|restore <disk> <file>\n");
	return 2;
}


static void
list()
{
	std::vector<DiskInfo> disks;
	ListDisks(disks);
	for (const DiskInfo& disk : disks) {
		printf("disk\t%s\t%s\t%s%s%s%s%s\n", disk.path.String(),
			SizeString(disk.size).String(),
			disk.partitioning.Length() > 0 ? disk.partitioning.String()
				: "(no partition table)",
			disk.isBootDisk ? "\tboot disk" : "",
			disk.readOnly ? "\tread-only" : "",
			disk.isFile ? "\timage " : "", disk.filePath.String());
		for (const PartitionInfo& partition : disk.partitions) {
			printf("partition\t%" B_PRId32 "\t%s\t%s\t\"%s\"\t\"%s\"\t%s\t%s"
				"\n", partition.index, SizeString(partition.size).String(),
				partition.content.Length() > 0 ? partition.content.String()
					: "(empty)",
				partition.name.String(), partition.contentName.String(),
				partition.mounted ? partition.mountPoint.String()
					: "(not mounted)",
				partition.system.Length() > 0 ? partition.system.String()
					: "-");
			if (partition.warning.Length() > 0)
				printf("warning\t%s\n", partition.warning.String());
		}
	}
}


static bool
parse_step(const char* text, Operation& operation)
{
	BString step(text);
	BString fields[4];
	int count = 0;
	int32 start = 0;
	while (count < 4) {
		int32 colon = step.FindFirst(':', start);
		if (colon < 0 || count == 3) {
			step.CopyInto(fields[count++], start, step.Length() - start);
			break;
		}
		step.CopyInto(fields[count++], start, colon - start);
		start = colon + 1;
	}
	operation.index = -1;
	operation.size = 0;
	if (fields[0] == "table" && count == 2) {
		operation.kind = kInitializePartitioning;
		operation.system = fields[1];
		return true;
	}
	if (fields[0] == "create" && count >= 2) {
		operation.kind = kCreatePartition;
		operation.size = ParseSize(fields[1].String());
		operation.name = fields[2];
		operation.type = fields[3];
		return operation.size >= 0;
	}
	if (fields[0] == "delete" && count == 2) {
		operation.kind = kDeletePartition;
		operation.index = atoi(fields[1].String());
		return true;
	}
	if (fields[0] == "format" && count >= 3) {
		operation.kind = kFormat;
		operation.index = atoi(fields[1].String());
		operation.system = fields[2];
		operation.name = fields[3];
		return true;
	}
	if (fields[0] == "name" && count == 3) {
		operation.kind = kSetName;
		operation.index = atoi(fields[1].String());
		operation.name = fields[2];
		return true;
	}
	return false;
}


int
main(int argc, char** argv)
{
	if (argc < 2)
		return usage();
	const char* command = argv[1];
	if (strcmp(command, "list") == 0) {
		list();
		return 0;
	}
	if ((strcmp(command, "attach") == 0 || strcmp(command, "detach") == 0)
		&& argc == 3) {
		if (command[0] == 'a') {
			BString path;
			status_t status = AttachImage(argv[2], path);
			if (status != B_OK) {
				fprintf(stderr, "rengadisk: %s: %s\n", argv[2],
					strerror(status));
				return 1;
			}
			printf("%s\n", path.String());
			return 0;
		}
		status_t status = DetachImage(argv[2]);
		if (status != B_OK)
			fprintf(stderr, "rengadisk: %s: %s\n", argv[2], strerror(status));
		return status == B_OK ? 0 : 1;
	}
	if ((strcmp(command, "backup") == 0 || strcmp(command, "restore") == 0)
		&& argc == 4) {
		DiskInfo disk;
		status_t status = GetDisk(argv[2], disk);
		if (status == B_OK && command[0] == 'r'
			&& (disk.isBootDisk || !disk.isFile)) {
			fprintf(stderr, "rengadisk: restore: only disk images, never the "
				"boot disk\n");
			return 1;
		}
		BString newPath;
		if (status == B_OK) {
			status = command[0] == 'b'
				? BackupTable(disk.path.String(), argv[3])
				: RestoreTable(disk.path.String(), argv[3], &newPath);
		}
		// an image is attached again to read the table: its new path
		if (status == B_OK && command[0] == 'r')
			printf("%s\n", newPath.String());
		if (status != B_OK)
			fprintf(stderr, "rengadisk: %s: %s\n", command, strerror(status));
		return status == B_OK ? 0 : 1;
	}
	if (strcmp(command, "plan") == 0 && argc >= 4) {
		Plan plan(argv[2]);
		bool apply = false;
		BPath backupDir;
		find_directory(B_USER_SETTINGS_DIRECTORY, &backupDir, true);
		backupDir.Append("Renga/disk-backups");
		BString backup = backupDir.Path();
		for (int i = 3; i < argc; i++) {
			if (strcmp(argv[i], "--apply") == 0)
				apply = true;
			else if (strcmp(argv[i], "--allow-real-disks") == 0)
				plan.AllowRealDisks(true);
			else if (strcmp(argv[i], "--backup-dir") == 0 && i + 1 < argc)
				backup = argv[++i];
			else {
				Operation operation;
				if (!parse_step(argv[i], operation)) {
					fprintf(stderr, "rengadisk: bad step \"%s\"\n", argv[i]);
					return usage();
				}
				plan.Add(operation);
			}
		}
		BString why;
		status_t status = plan.Check(why);
		printf("%s", plan.Preview().String());
		if (status != B_OK) {
			printf("refused: %s\n", why.String());
			return 1;
		}
		if (!apply) {
			printf("checked; nothing written (add --apply)\n");
			return 0;
		}
		BString log;
		status = plan.Apply(backup.String(), log);
		printf("%s", log.String());
		return status == B_OK ? 0 : 1;
	}
	return usage();
}
