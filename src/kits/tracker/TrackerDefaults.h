/*
 * Copyright 2023 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		John Scipione, jscipione@gmail.com
 */
#ifndef _TRACKER_DEFAULTS_H
#define _TRACKER_DEFAULTS_H


static const bool kDefaultShowDisksIcon = false;
static const bool kDefaultMountVolumesOntoDesktop = true;
static const bool kDefaultMountSharedVolumesOntoDesktop = true;
static const bool kDefaultEjectWhenUnmounting = true;

static const bool kDefaultDesktopFilePanelRoot = true;
static const bool kDefaultShowSelectionWhenInactive = true;

static const bool kDefaultShowFullPathInTitleBar = false;
static const bool kDefaultSingleWindowBrowse = true;
	// Renga (2026-11-20): one window navigates folders, with the navigator
	// (path bar) shown, for new installs; saved settings keep their values
static const bool kDefaultShowNavigator = true;
static const bool kDefaultTransparentSelection = true;
static const bool kDefaultSortFolderNamesFirst = true;
static const bool kDefaultHideDotFiles = false;
static const bool kDefaultTypeAheadFiltering = true;
	// Renga (F-66, batch 9): type to filter, for new installs; saved
	// settings keep their value
static const bool kDefaultGenerateImageThumbnails = true;
static const bool kDefaultPrefetchFolderMenus = true;
	// Renga (F-59), NavMenuPrefetch.h
static const bool kDefaultShowFolderSizes = true;
	// Renga (F-66, batch 9), FolderSizeCache.h
static const bool kDefaultKeepBothOnConflict = true;
	// Renga (F-66, batch 9): the name-conflict dialog with "Keep both" and
	// "Apply to all" (FSUtils.cpp, ConflictDialog)
static const bool kDefaultOpenTerminalHere = true;
	// Renga (F-66, batch 9): "Open Terminal here" in folder windows' menus
static const bool kDefaultQuickLook = true;
	// Renga (F-66, batch 9): QuickLookWindow.h
static const bool kDefaultBatchRename = true;
	// Renga (F-66, batch 9): BatchRenameWindow.h

static const int32 kDefaultRecentApplications = 10;
static const int32 kDefaultRecentDocuments = 10;
static const int32 kDefaultRecentFolders = 10;

static const bool kDefaultShowVolumeSpaceBar = true;
static const uint8 kDefaultSpaceBarAlpha = 192;
static const rgb_color kDefaultUsedSpaceColor = { 0, 203, 0, kDefaultSpaceBarAlpha };
static const rgb_color kDefaultFreeSpaceColor = { 255, 255, 255, kDefaultSpaceBarAlpha };
static const rgb_color kDefaultWarningSpaceColor = { 203, 0, 0, kDefaultSpaceBarAlpha };


#endif	// _TRACKER_DEFAULTS_H
