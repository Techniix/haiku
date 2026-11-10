/*
 * Copyright 2003-2006, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors in chronological order:
 *		Sikosis
 *		Jérôme Duval
 */


#include "Media.h"
#include "MediaViews.h"

#include <stdio.h>
#include <string.h>

#include <Catalog.h>
#include <Locale.h>
#include <StorageKit.h>
#include <String.h>


Media::Media() 
	:
	BApplication("application/x-vnd.Haiku-Media"),
	fIcons(),
	fWindow(NULL)
{
	BRect rect(32, 64, 637, 462);

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append(SETTINGS_FILE);
		BFile file(path.Path(),B_READ_ONLY);
		if (file.InitCheck()==B_OK) {
			char buffer[255];
			ssize_t size = 0;
			while ((size = file.Read(buffer, 255)) > 0) {
				int32 i = 0;
				while (buffer[i] == '#') {
					while (i < size && buffer[i] != '\n')
						i++;
					i++;
				}
				int32 a, b, c, d;
				const char* scanString = " rect = %li,%li,%li,%li";
				if (sscanf(&buffer[i], scanString, &a, &b, &c, &d) == 4) {
					if (c - a >= rect.IntegerWidth()) {
						rect.left = a;
						rect.right = c;
					}
					if (d - b >= rect.IntegerHeight()) {
						rect.top = b;
						rect.bottom = d;
					}
				}
			}
		}
	}

	MediaListItem::SetIcons(&fIcons);
	fWindow = new MediaWindow(rect);
}


status_t
Media::InitCheck()
{
	if (fWindow)
		return fWindow->InitCheck();
	return B_OK;
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	// Opens the audio settings with the advanced controls shown (for
	// support instructions and the harness's screenshots).
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--advanced-audio") == 0)
			gShowAdvancedAudio = true;
		else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
			gInitialDevice = argv[++i];
	}

	Media app;
	if (app.InitCheck() == B_OK)
		app.Run();

	return 0;
}

