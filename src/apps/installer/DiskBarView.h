/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DISK_BAR_VIEW_H
#define DISK_BAR_VIEW_H


#include <View.h>

#include <vector>

#include "DestinationPlan.h"


/*!	A disk as a bar (the Installer wizard's Summary page, F-23): each
	partition and free space as wide as its share of the disk, partitions
	that stay in one colour, new ones and the destination in the highlight
	colour, free space hatched; the name inside when it fits.
*/
class DiskBarView : public BView {
public:
								DiskBarView(const char* name);

			void				SetSegments(off_t diskSize,
									const std::vector<DiskSegment>& segments);

	virtual	void				Draw(BRect updateRect);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();
	virtual	BSize				MaxSize();

private:
			off_t				fDiskSize;
			std::vector<DiskSegment> fSegments;
};


#endif	// DISK_BAR_VIEW_H
