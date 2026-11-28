/*
 * Copyright 2026, Techniix.
 * Distributed under the terms of the MIT License.
 *
 * The Flatpak test application's program (org.renga.Hello, batch 14;
 * root/build-flatpak-root.sh builds it into a local OSTree repository).
 * It runs inside Flatpak's layout and says what it sees there: its
 * Flatpak ID, its own file under /app, the runtime's shell under /usr and
 * the /.flatpak-info file Flatpak writes for every program it starts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


int
main(int argc, char** argv)
{
	const char* id = getenv("FLATPAK_ID");
	char line[256] = "";
	FILE* info = fopen("/.flatpak-info", "r");
	if (info != NULL) {
		/* the first line names the section, [Application] */
		if (fgets(line, sizeof(line), info) != NULL)
			line[strcspn(line, "\n")] = '\0';
		fclose(info);
	}
	printf("renga-hello: hello from a Flatpak app\n");
	printf("renga-hello: FLATPAK_ID=%s\n", id != NULL ? id : "(unset)");
	printf("renga-hello: app=%s runtime=%s info=%s\n",
		access("/app/bin/renga-hello", X_OK) == 0 ? "yes" : "no",
		access("/usr/bin/sh", X_OK) == 0 ? "yes" : "no",
		line[0] != '\0' ? line : "(none)");
	for (int i = 1; i < argc; i++)
		printf("renga-hello: arg %d=%s\n", i, argv[i]);
	return 0;
}
