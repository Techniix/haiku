/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "DiskBarView.h"

#include <ControlLook.h>
#include <InterfaceDefs.h>


static const pattern kHatch = { { 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44,
	0x88 } };


DiskBarView::DiskBarView(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fDiskSize(0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}


void
DiskBarView::SetSegments(off_t diskSize,
	const std::vector<DiskSegment>& segments)
{
	fDiskSize = diskSize;
	fSegments = segments;
	Invalidate();
}


void
DiskBarView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	rgb_color frame = tint_color(panel, B_DARKEN_3_TINT);
	rgb_color kept = tint_color(panel, B_DARKEN_1_TINT);
	rgb_color highlight = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
	rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
	rgb_color highlightText = highlight.IsLight()
		? make_color(0, 0, 0) : make_color(255, 255, 255);

	SetHighColor(panel);
	FillRect(bounds);
	BRect bar = bounds.InsetByCopy(1, 1);
	if (fDiskSize <= 0 || bar.Width() < 4)
		return;

	font_height height;
	GetFontHeight(&height);
	float width = bar.Width();
	for (const DiskSegment& segment : fSegments) {
		float left = bar.left + width * segment.offset / fDiskSize;
		float right = bar.left
			+ width * (segment.offset + segment.size) / fDiskSize;
		if (right - left < 2)
			right = left + 2;
		BRect rect(left, bar.top, right, bar.bottom);
		bool highlighted = segment.kind == SEGMENT_NEW
			|| segment.kind == SEGMENT_TARGET;
		if (segment.kind == SEGMENT_FREE) {
			SetHighColor(tint_color(panel, B_DARKEN_2_TINT));
			SetLowColor(panel);
			FillRect(rect, kHatch);
		} else {
			SetHighColor(highlighted ? highlight : kept);
			FillRect(rect);
		}
		SetHighColor(frame);
		StrokeRect(rect);

		if (segment.label.Length() == 0)
			continue;
		BString label = segment.label;
		float room = rect.Width() - 6;
		TruncateString(&label, B_TRUNCATE_END, room);
		if (label.Length() == 0 || StringWidth(label) > room)
			continue;
		SetHighColor(highlighted ? highlightText : text);
		SetLowColor(highlighted ? highlight : kept);
		DrawString(label, BPoint(rect.left + 3,
			(rect.top + rect.bottom + height.ascent - height.descent) / 2));
	}
	SetHighColor(frame);
	StrokeRect(bar);
}


BSize
DiskBarView::MinSize()
{
	font_height height;
	GetFontHeight(&height);
	return BSize(be_control_look->DefaultLabelSpacing() * 40,
		ceilf((height.ascent + height.descent) * 1.8f));
}


BSize
DiskBarView::PreferredSize()
{
	BSize size = MinSize();
	size.width *= 1.5f;
	return size;
}


BSize
DiskBarView::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, MinSize().height);
}
