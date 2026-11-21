/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Stands in for an AppImage in linux_apps_test (batch 10): the test copies
 * it and writes the AppImage magic ("AI" 2) into its ELF header padding,
 * then opens it the way Tracker does. It records what an AppImage runtime
 * would see: APPIMAGE_EXTRACT_AND_RUN and APPIMAGE.
 */

#include <stdio.h>
#include <stdlib.h>


int
main(int argc, char** argv)
{
	FILE* file = fopen("/tmp/ce-appimage-ran", "w");
	if (file == NULL)
		return 1;
	const char* extract = getenv("APPIMAGE_EXTRACT_AND_RUN");
	const char* appimage = getenv("APPIMAGE");
	fprintf(file, "extract=%s appimage=%s argc=%d\n",
		extract != NULL ? extract : "-", appimage != NULL ? appimage : "-",
		argc);
	fclose(file);
	(void)argv;
	return 0;
}
