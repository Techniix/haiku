/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_SCREEN_FIT_H
#define _RENGA_SCREEN_FIT_H


/*!	Renga (batch 12, large-font audit): windows that fit the screen at any
	system font size.

	fit_to_screen() returns a window's content as it is when its minimum
	size fits the screen, and otherwise the content inside a scroll view
	(with a bar only in the direction that does not fit) whose minimum is
	what the screen allows; the content is laid out at least at its own
	minimum size, so nothing on it is squeezed or cut off.

	WrappingNote is a note or explanation that wraps to the width it gets
	instead of asking for its whole text on one line (a BStringView does,
	and long notes made windows wider than the screen at large fonts).

	size_window_to_screen() sizes a laid-out window to its layout's
	preferred size, clamped to the screen (never below the layout's
	minimum) and moves it fully on screen.

	Header only, so that each Renga program can use it without a library
	change (UsePrivateHeaders shared).
*/


#include <ControlLook.h>
#include <Font.h>
#include <Layout.h>
#include <Screen.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <math.h>

#include <algorithm>


namespace BPrivate {


//!	The largest window content a screen can show, beside the decorations.
static inline BSize
screen_content_limit(const BWindow* window = NULL)
{
	BRect frame(0, 0, 1023, 767);
	if (window != NULL) {
		BScreen screen(const_cast<BWindow*>(window));
		if (screen.IsValid())
			frame = screen.Frame();
	} else {
		BScreen screen;
		if (screen.IsValid())
			frame = screen.Frame();
	}
	// the window border on both sides and the tab plus the border on the
	// bottom, from the plain font as the decorator sizes its tab
	float tab = ceilf(be_plain_font->Size() * 1.6f) + 10;
	return BSize(frame.Width() - 20, frame.Height() - tab - 20);
}


class ScreenFitScroller : public BView {
public:
	ScreenFitScroller(BView* content, BSize limit)
		:
		BView(content->Name(), B_WILL_DRAW | B_FRAME_EVENTS),
		fContent(content),
		fLimit(limit)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		AddChild(content);
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		_Layout();
	}

	virtual void FrameResized(float width, float height)
	{
		BView::FrameResized(width, height);
		_Layout();
	}

	virtual BSize MinSize()
	{
		BSize min = fContent->MinSize();
		return BSize(std::min(min.width, fLimit.width),
			std::min(min.height, fLimit.height));
	}

	virtual BSize PreferredSize()
	{
		BSize preferred = fContent->PreferredSize();
		return BSize(std::min(preferred.width, fLimit.width),
			std::min(preferred.height, fLimit.height));
	}

	virtual BSize MaxSize()
	{
		return BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED);
	}

	virtual void InvalidateLayout(bool descendants = false)
	{
		BView::InvalidateLayout(descendants);
		if (Window() != NULL)
			_Layout();
	}

private:
	void _Layout()
	{
		BSize min = fContent->MinSize();
		BSize max = fContent->MaxSize();
		float width = std::max(Bounds().Width(), min.width);
		float height = std::max(Bounds().Height(), min.height);
		width = std::min(width, std::max(max.width, min.width));
		height = std::min(height, std::max(max.height, min.height));
		fContent->MoveTo(0, 0);
		fContent->ResizeTo(width, height);
		float step = ceilf(be_plain_font->Size() * 1.6f);
		if (BScrollBar* bar = ScrollBar(B_VERTICAL)) {
			float range = std::max(0.0f, height - Bounds().Height());
			bar->SetRange(0, range);
			bar->SetProportion(range > 0 ? Bounds().Height() / height : 1.0f);
			bar->SetSteps(step, Bounds().Height());
		}
		if (BScrollBar* bar = ScrollBar(B_HORIZONTAL)) {
			float range = std::max(0.0f, width - Bounds().Width());
			bar->SetRange(0, range);
			bar->SetProportion(range > 0 ? Bounds().Width() / width : 1.0f);
			bar->SetSteps(step, Bounds().Width());
		}
	}

	BView*	fContent;
	BSize	fLimit;
};


/*!	A note that wraps: a read-only BTextView whose height follows its
	line count at the width the layout gives it (the same approach as the
	Welcome's paragraphs: no height-for-width, which sent layouts round in
	a loop; the height only changes when the width or the text does).
	\a tint lightens the text as notes do (B_LIGHTEN_1_TINT), or 0.
*/
class WrappingNote : public BTextView {
public:
	WrappingNote(const char* name, const char* text, float tint = 0)
		:
		BTextView(name, be_plain_font, NULL, B_WILL_DRAW | B_FRAME_EVENTS),
		fTint(tint),
		fHeight(-1),
		fWidth(-1)
	{
		MakeEditable(false);
		MakeSelectable(false);
		SetWordWrap(true);
		SetStylable(false);
		SetInsets(0, 0, 0, 0);
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
		BTextView::SetText(text);
		_SetHeight(_Estimate());
	}

	virtual void SetText(const char* text, const text_run_array* runs = NULL)
	{
		BTextView::SetText(text, runs);
		_ApplyColor();
		if (Window() != NULL)
			_Measure();
		else
			_SetHeight(_Estimate());
	}

	void SetTint(float tint)
	{
		fTint = tint;
		_ApplyColor();
	}

	virtual void AttachedToWindow()
	{
		BTextView::AttachedToWindow();
		if (Parent() != NULL) {
			SetViewUIColor(Parent()->ViewUIColor());
			SetLowUIColor(Parent()->ViewUIColor());
		}
		_ApplyColor();
		_Measure();
	}

	virtual void FrameResized(float width, float height)
	{
		BTextView::FrameResized(width, height);
		if (width != fWidth)
			_Measure();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_COLORS_UPDATED)
			_ApplyColor();
		BTextView::MessageReceived(message);
	}

private:
	void _ApplyColor()
	{
		rgb_color color = ui_color(B_PANEL_TEXT_COLOR);
		if (fTint != 0)
			color = tint_color(color, fTint);
		BFont font;
		GetFontAndColor(0, &font);
		SetFontAndColor(&font, B_FONT_ALL, &color);
	}

	float _Estimate()
	{
		// a line as wide as the preferred width, words a fifth short
		float lines = ceilf(be_plain_font->StringWidth(Text()) * 1.2f
			/ _PreferredWidth());
		for (const char* c = Text(); *c != '\0'; c++) {
			if (*c == '\n')
				lines++;
		}
		font_height metrics;
		be_plain_font->GetHeight(&metrics);
		return std::max(1.0f, lines)
			* ceilf(metrics.ascent + metrics.descent + metrics.leading) + 2;
	}

	float _PreferredWidth()
	{
		return std::min(ceilf(be_plain_font->StringWidth(Text()) + 4),
			ceilf(be_plain_font->Size() * 34));
	}

	void _Measure()
	{
		fWidth = Bounds().Width();
		if (fWidth < 10 || Window() == NULL)
			return;
		_SetHeight(TextHeight(0, CountLines() - 1) + 2);
	}

	void _SetHeight(float height)
	{
		height = ceilf(height);
		if (fabsf(height - fHeight) < 1)
			return;
		fHeight = height;
		float minWidth = std::min(_PreferredWidth(),
			ceilf(be_plain_font->Size() * 12));
		SetExplicitMinSize(BSize(minWidth, height));
		SetExplicitPreferredSize(BSize(_PreferredWidth(), height));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
		InvalidateLayout();
	}

	float	fTint;
	float	fHeight;
	float	fWidth;
};


/*!	\a content as it is if its minimum size is within \a limit (see
	screen_content_limit(); subtract what the window adds around the
	content), else in a scroll view.
*/
static inline BView*
fit_to_screen(BView* content, BSize limit)
{
	BSize min = content->MinSize();
	bool horizontal = min.width > limit.width;
	bool vertical = min.height > limit.height;
	if (!horizontal && !vertical)
		return content;
	// room for the bars themselves
	if (horizontal)
		limit.height -= be_control_look->GetScrollBarWidth(B_HORIZONTAL);
	if (vertical)
		limit.width -= be_control_look->GetScrollBarWidth(B_VERTICAL);
	return new BScrollView(content->Name(),
		new ScreenFitScroller(content, limit), 0, horizontal, vertical,
		B_NO_BORDER);
}


/*!	The window at its layout's preferred size, clamped to the screen and
	never below the layout's minimum, fully on screen. Call once the
	window's layout is complete, before Show().
*/
static inline void
size_window_to_screen(BWindow* window)
{
	BLayout* layout = window->GetLayout();
	if (layout == NULL)
		return;
	BSize preferred = layout->PreferredSize();
	BSize min = layout->MinSize();
	BSize limit = screen_content_limit(window);
	float width = std::max(std::min(preferred.width, limit.width),
		min.width);
	float height = std::max(std::min(preferred.height, limit.height),
		min.height);
	window->ResizeTo(width, height);
	window->MoveOnScreen(B_MOVE_IF_PARTIALLY_OFFSCREEN);
}


}	// namespace BPrivate


#endif	// _RENGA_SCREEN_FIT_H
