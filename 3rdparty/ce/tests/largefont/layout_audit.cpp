/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Layout audit for the large-font suite (batch 12, ui_large_font_test).

	Loaded into a program with LD_PRELOAD; does nothing unless the
	environment variable CE_LAYOUT_AUDIT names a directory. A thread then
	waits for a file "request" in that directory, whose content is a tag;
	it audits every shown window of the program and writes the result to
	"<tag>.txt" (written as "<tag>.tmp" and renamed, so a reader never sees
	half a report). The program itself needs no change, so the same check
	runs on every Renga window.

	What counts as clipped (one "PROBLEM" line each):

	  min_size    a shown view is smaller than its own minimum size: a
	              button, check box or menu field narrower than its label,
	              or a layout squeezed below what it needs
	  parent      a shown view reaches outside its parent's bounds; the
	              contents of a scroll view may, but only in the directions
	              it has a scroll bar for (a page wider than its scroll view
	              with only a vertical bar is cut off)
	  text        a BStringView narrower than its text and not set to
	              truncate; a read-only BTextView whose text is taller than
	              the view, or a line wider than it when not wrapping
	  menu_field  a menu field whose shown item would be cut off; the test
	              replicates BMenuItem::DrawContent(): the label is cut when
	              it is wider than the bar's content width
	  list_item   a list (no horizontal scroll bar) with an item wider than
	              the list, as BOutlineListView itself measures items
	  window      a window larger than its screen, reaching outside it, or
	              whose layout needs more than the screen offers

	Strings that are shortened on purpose (BStringView truncation, menu
	labels the program truncates itself) are listed as "SHORTENED" lines,
	which are not failures. Every tab of each BTabView is audited: the audit
	selects each tab in turn, lays the window out, and selects the original
	tab again afterwards.
*/


#include <Application.h>
#include <ControlLook.h>
#include <Font.h>
#include <Layout.h>
#include <ListView.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <OS.h>
#include <OutlineListView.h>
#include <Screen.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <String.h>
#include <StringItem.h>
#include <StringView.h>
#include <TabView.h>
#include <TextView.h>
#include <Window.h>

#include <MenuPrivate.h>

#include <cxxabi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <typeinfo>
#include <unistd.h>


namespace {


static BString
class_of(const BView* view)
{
	int status = 0;
	char* name = abi::__cxa_demangle(typeid(*view).name(), NULL, NULL,
		&status);
	BString result = status == 0 && name != NULL ? name : typeid(*view).name();
	free(name);
	return result;
}


static BString
one_line(const char* text, int32 maxLength = 60)
{
	BString result(text != NULL ? text : "");
	result.ReplaceAll('\n', ' ');
	result.ReplaceAll('\t', ' ');
	if (result.Length() > maxLength) {
		result.Truncate(maxLength);
		result << "...";
	}
	return result;
}


/*!	True for a view that a scroll bar scrolls: its content may be larger
	than the view, and its children may lie outside it.
*/
static bool
is_scroll_target(BView* view)
{
	return view->ScrollBar(B_HORIZONTAL) != NULL
		|| view->ScrollBar(B_VERTICAL) != NULL;
}


class Audit {
public:
	Audit(FILE* out)
		:
		fOut(out),
		fProblems(0)
	{
	}

	int32 Problems() const { return fProblems; }

	void Window(BWindow* window)
	{
		fWindowTitle = one_line(window->Title(), 40);
		BRect frame = window->Frame();
		BScreen screen(window);
		BRect screenFrame = screen.Frame();
		BSize layoutMin(0, 0);
		if (window->GetLayout() != NULL)
			layoutMin = window->GetLayout()->MinSize();
		fprintf(fOut, "WINDOW %s\t%.0fx%.0f at %.0f,%.0f\tlayout min "
			"%.0fx%.0f\tscreen %.0fx%.0f\n", fWindowTitle.String(),
			frame.Width() + 1, frame.Height() + 1, frame.left, frame.top,
			layoutMin.width + 1, layoutMin.height + 1,
			screenFrame.Width() + 1, screenFrame.Height() + 1);
		if (frame.Width() > screenFrame.Width()
			|| frame.Height() > screenFrame.Height()) {
			_Problem("(window)", "window", "%.0fx%.0f is larger than the "
				"screen", frame.Width() + 1, frame.Height() + 1);
		} else if (frame.left < screenFrame.left - 1
			|| frame.top < screenFrame.top - 1
			|| frame.right > screenFrame.right + 1
			|| frame.bottom > screenFrame.bottom + 1) {
			_Problem("(window)", "window", "frame %.0f,%.0f-%.0f,%.0f reaches "
				"outside the screen", frame.left, frame.top, frame.right,
				frame.bottom);
		}
		if (layoutMin.width > screenFrame.Width()
			|| layoutMin.height > screenFrame.Height()) {
			_Problem("(window)", "window", "the layout needs %.0fx%.0f",
				layoutMin.width + 1, layoutMin.height + 1);
		}

		for (int32 i = 0; BView* child = window->ChildAt(i); i++)
			_View(child, window->Bounds(), false, false, child->Name());

		// what makes a layout too large: the deepest views (shown or not,
		// such as other pages of a card layout) whose minimum alone takes
		// most of the screen
		if (layoutMin.width > screenFrame.Width()
			|| layoutMin.height > screenFrame.Height()) {
			for (int32 i = 0; BView* child = window->ChildAt(i); i++) {
				_Culprits(child, child->Name(), screenFrame.Width() * 0.6f,
					screenFrame.Height() * 0.6f);
			}
		}
	}

	void Tabs(BWindow* window, BView* view)
	{
		// every BTabView's other tabs, audited in turn
		if (BTabView* tabs = dynamic_cast<BTabView*>(view)) {
			if (!tabs->IsHidden()) {
				int32 original = tabs->Selection();
				for (int32 t = 0; t < tabs->CountTabs(); t++) {
					if (t == original)
						continue;
					tabs->Select(t);
					window->Layout(false);
					BString tag;
					tag << "tab " << one_line(tabs->TabAt(t)->Label(), 30);
					fprintf(fOut, "TAB %s\n", tag.String());
					fTab = tag;
					for (int32 i = 0; BView* child = window->ChildAt(i); i++)
						_View(child, window->Bounds(), false, false,
							child->Name());
					fTab = "";
				}
				tabs->Select(original);
				window->Layout(false);
			}
		}
		for (int32 i = 0; BView* child = view->ChildAt(i); i++)
			Tabs(window, child);
	}

private:
	/*!	Lists ("WIDE" or "TALL" lines) the deepest views below \a view
		whose minimum width (height) is above \a width (\a height).
	*/
	bool _Culprits(BView* view, BString path, float width, float height)
	{
		BSize min = view->MinSize();
		bool wide = min.width > width && min.width < B_SIZE_UNLIMITED;
		bool tall = min.height > height && min.height < B_SIZE_UNLIMITED;
		if (!wide && !tall)
			return false;
		bool below = false;
		for (int32 i = 0; BView* child = view->ChildAt(i); i++) {
			BString childPath = path;
			childPath << "/" << (child->Name() != NULL ? child->Name() : "?");
			if (_Culprits(child, childPath, width, height))
				below = true;
		}
		if (!below) {
			fprintf(fOut, "%s %s\t%s [%s]\tmin %.0fx%.0f\n",
				wide ? "WIDE" : "TALL", fWindowTitle.String(), path.String(),
				class_of(view).String(), min.width, min.height);
		}
		return true;
	}

	void _Problem(const char* path, const char* kind, const char* format,
		...)
	{
		char detail[512];
		va_list args;
		va_start(args, format);
		vsnprintf(detail, sizeof(detail), format, args);
		va_end(args);
		fprintf(fOut, "PROBLEM %s\t%s%s%s\t%s\t%s\n", fWindowTitle.String(),
			fTab.String(), fTab.IsEmpty() ? "" : ": ", path, kind, detail);
		fProblems++;
	}

	void _Shortened(const char* path, const char* text)
	{
		fprintf(fOut, "SHORTENED %s\t%s\t%s\n", fWindowTitle.String(), path,
			one_line(text).String());
	}

	/*!	\a scrollsX and \a scrollsY: the parent's content can be scrolled
		that way (a scroll bar of that orientation targets the parent); a
		child may reach outside the parent only in those directions.
	*/
	void _View(BView* view, BRect parentBounds, bool scrollsX, bool scrollsY,
		BString path)
	{
		if (view->IsHidden())
			return;

		BString name = path;
		name << " [" << class_of(view) << "]";
		BRect frame = view->Frame();
		BRect bounds = view->Bounds();
		bool scrolls = is_scroll_target(view);
		bool isScrollBar = dynamic_cast<BScrollBar*>(view) != NULL;

		if (!isScrollBar && frame.IsValid()
			&& ((!scrollsX && (frame.left < parentBounds.left - 1
					|| frame.right > parentBounds.right + 1))
				|| (!scrollsY && (frame.top < parentBounds.top - 1
					|| frame.bottom > parentBounds.bottom + 1)))) {
			_Problem(name, "parent", "frame %.0f,%.0f-%.0f,%.0f, parent "
				"%.0f,%.0f-%.0f,%.0f", frame.left, frame.top, frame.right,
				frame.bottom, parentBounds.left, parentBounds.top,
				parentBounds.right, parentBounds.bottom);
			// for diagnosis: the ancestors' sizes against their minimums
			for (BView* up = view->Parent(); up != NULL; up = up->Parent()) {
				BSize upMin = up->MinSize();
				fprintf(fOut, "CHAIN %s\t%s [%s]\t%.0fx%.0f, min %.0fx%.0f"
					"%s\n", fWindowTitle.String(), up->Name() != NULL
						? up->Name() : "?", class_of(up).String(),
					up->Bounds().Width() + 1, up->Bounds().Height() + 1,
					upMin.width, upMin.height,
					(up->Flags() & B_SUPPORTS_LAYOUT) != 0 ? "" : ", no layout");
			}
		}

		BStringView* truncating = dynamic_cast<BStringView*>(view);
		if (truncating != NULL && truncating->Truncation() == B_NO_TRUNCATION)
			truncating = NULL;
		// Views whose content scrolls inside them (scroll views, column
		// list views) and editable text fields (their text scrolls; the
		// spinner's and text control's field report a minimum from the
		// font, not from their text) are only held to their minimum
		// height; one and a half pixels of rounding are allowed.
		BTextView* editable = dynamic_cast<BTextView*>(view);
		if (editable != NULL && !editable->IsEditable())
			editable = NULL;
		bool container = dynamic_cast<BScrollView*>(view) != NULL
			|| class_of(view) == "BColumnListView";
		if (!scrolls && !isScrollBar && truncating == NULL && !container) {
			BSize min = view->MinSize();
			if ((editable == NULL && min.width < B_SIZE_UNLIMITED
					&& bounds.Width() + 1 < min.width - 1.5f)
				|| (min.height < B_SIZE_UNLIMITED
					&& bounds.Height() + 1 < min.height - 1.5f)) {
				_Problem(name, "min_size", "%.0fx%.0f, needs %.0fx%.0f",
					bounds.Width() + 1, bounds.Height() + 1, min.width,
					min.height);
			}
		}

		BFont font;
		view->GetFont(&font);

		if (BStringView* string = dynamic_cast<BStringView*>(view)) {
			float width = font.StringWidth(string->Text());
			if (width > bounds.Width() + 1.5f) {
				if (string->Truncation() != B_NO_TRUNCATION)
					_Shortened(name, string->Text());
				else {
					_Problem(name, "text", "\"%s\" needs %.0f, has %.0f",
						one_line(string->Text()).String(), width,
						bounds.Width() + 1);
				}
			}
		}

		if (BTextView* text = dynamic_cast<BTextView*>(view)) {
			if (!scrolls && !text->IsEditable() && text->TextLength() > 0) {
				float height = text->TextHeight(0, text->TextLength());
				BRect textRect = text->TextRect();
				if (height > bounds.Height() + 1.5f) {
					_Problem(name, "text", "%.0f high, text needs %.0f: "
						"\"%s\"", bounds.Height() + 1, height,
						one_line(text->Text()).String());
				}
				if (!text->DoesWordWrap()) {
					for (int32 line = 0; line < text->CountLines(); line++) {
						float width = text->LineWidth(line);
						if (width > bounds.Width() + 1.5f) {
							_Problem(name, "text", "line %" B_PRId32 " needs "
								"%.0f, has %.0f", line, width,
								bounds.Width() + 1);
							break;
						}
					}
				} else if (textRect.right > bounds.right + 1.5f) {
					_Problem(name, "text", "text rect %.0f wide, view %.0f",
						textRect.Width() + 1, bounds.Width() + 1);
				}
			}
		}

		if (BMenuField* field = dynamic_cast<BMenuField*>(view)) {
			BMenuBar* bar = field->MenuBar();
			BMenuItem* item = bar != NULL ? bar->ItemAt(0) : NULL;
			if (item != NULL && item->Label() != NULL && !bar->IsHidden()) {
				BFont barFont;
				bar->GetFont(&barFont);
				BPrivate::MenuPrivate menuPrivate(bar);
				BRect padding = menuPrivate.Padding();
				float maxContentWidth = bar->MaxContentWidth();
				float available = maxContentWidth > 0 ? maxContentWidth
					: bar->Frame().Width() - padding.left - padding.right;
				float needed = barFont.StringWidth(item->Label());
				if (roundf(available) < roundf(needed)) {
					_Problem(name, "menu_field", "\"%s\" needs %.0f, has %.0f",
						one_line(item->Label()).String(), needed, available);
				}
			}
		}

		if (BListView* list = dynamic_cast<BListView*>(view)) {
			BOutlineListView* outline = dynamic_cast<BOutlineListView*>(list);
			if (list->ScrollBar(B_HORIZONTAL) == NULL) {
				float plain = be_plain_font->Size();
				float spacing = be_control_look->DefaultItemSpacing();
				for (int32 i = 0; i < list->CountItems(); i++) {
					BListItem* item = list->ItemAt(i);
					float needed = item->Width();
					if (outline != NULL)
						needed += plain + (item->OutlineLevel() + 1) * spacing;
					if (needed > bounds.Width() + 1.5f) {
						BStringItem* stringItem
							= dynamic_cast<BStringItem*>(item);
						_Problem(name, "list_item", "item %" B_PRId32
							" \"%s\" needs %.0f, has %.0f", i,
							one_line(stringItem != NULL ? stringItem->Text()
								: "").String(), needed, bounds.Width() + 1);
					}
				}
			}
		}

		for (int32 i = 0; BView* child = view->ChildAt(i); i++) {
			BString childPath = path;
			childPath << "/" << (child->Name() != NULL ? child->Name() : "?");
			_View(child, bounds, view->ScrollBar(B_HORIZONTAL) != NULL,
				view->ScrollBar(B_VERTICAL) != NULL, childPath);
		}
	}

private:
	FILE*		fOut;
	int32		fProblems;
	BString		fWindowTitle;
	BString		fTab;
};


static void
audit_all(const char* directory, const char* tag)
{
	BString temporary, final;
	temporary << directory << "/" << tag << ".tmp";
	final << directory << "/" << tag << ".txt";
	FILE* out = fopen(temporary.String(), "w");
	if (out == NULL)
		return;

	Audit audit(out);
	int32 windows = 0;
	for (int32 i = 0; BWindow* window = be_app->WindowAt(i); i++) {
		if (window->LockWithTimeout(3000000) != B_OK) {
			fprintf(out, "PROBLEM %s\t(window)\tlock\tcould not lock\n",
				window->Title());
			continue;
		}
		if (!window->IsHidden() && window->Look() != B_NO_BORDER_WINDOW_LOOK
			&& window->Frame().Width() > 8) {
			window->Layout(false);
			audit.Window(window);
			for (int32 c = 0; BView* child = window->ChildAt(c); c++)
				audit.Tabs(window, child);
			windows++;
		}
		window->Unlock();
	}
	fprintf(out, "END windows %" B_PRId32 " problems %" B_PRId32 "\n",
		windows, audit.Problems());
	fclose(out);
	rename(temporary.String(), final.String());
}


static status_t
audit_thread(void* data)
{
	const char* directory = (const char*)data;
	BString request;
	request << directory << "/request";
	while (true) {
		snooze(200000);
		if (be_app == NULL)
			continue;
		FILE* file = fopen(request.String(), "r");
		if (file == NULL)
			continue;
		char tag[128] = "";
		if (fgets(tag, sizeof(tag), file) == NULL)
			tag[0] = '\0';
		fclose(file);
		unlink(request.String());
		tag[strcspn(tag, "\r\n/")] = '\0';
		if (tag[0] == '\0')
			strcpy(tag, "audit");
		audit_all(directory, tag);
	}
	return B_OK;
}


struct Starter {
	Starter()
	{
		const char* directory = getenv("CE_LAYOUT_AUDIT");
		if (directory == NULL || directory[0] == '\0')
			return;
		thread_id thread = spawn_thread(audit_thread, "layout audit",
			B_LOW_PRIORITY, strdup(directory));
		if (thread >= 0)
			resume_thread(thread);
	}
};


static Starter sStarter;


}	// namespace
