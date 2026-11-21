/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SEARCH_WINDOW_H
#define SEARCH_WINDOW_H


#include <Window.h>

#include "MenuSearch.h"


class BListView;
class BMessageRunner;
class BStringView;
class BTextControl;


const uint32 kShowSearchWindow = 'DSsw';
	// from the Deskbar menu's "Search..." item; "text" (optional) fills
	// the field, "test" (harness) keeps the window open when it is not
	// active


/*!	The Deskbar menu's search (roadmap F-41, batch 9): a small window
	next to the Deskbar with a text field and the results of MenuSearch.
	Quick sources run as the text changes (after a short pause); files are
	searched on a thread, and their results come in a message, dropped
	when the text has changed in the meantime. Up and Down choose a
	result, Return opens it (a calculator or conversion result is copied
	to the clipboard), Escape or clicking elsewhere hides the window.
*/
class TSearchWindow : public BWindow {
public:
								TSearchWindow();
	virtual						~TSearchWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	void				WindowActivated(bool active);
	virtual	bool				QuitRequested();

			void				ShowNear(BRect deskbarFrame, bool vertical,
									const char* text, bool test);
			void				SetCalculatorEnabled(bool enabled);
			void				SetFilesEnabled(bool enabled)
									{ fFiles = enabled; }

			//! What the window lists now (harness): "title" and "kind".
			void				GetResults(BMessage& reply);

	static	status_t			Open(const SearchResult& result);

private:
			void				_Search();
			void				_ShowResults();
			void				_StopFileSearch();
	static	status_t			_FileSearchThread(void* data);
			void				_MoveSelection(int32 delta);
			void				_OpenSelection();

private:
			BTextControl*		fText;
			BListView*			fList;
			BStringView*		fStatus;
			BMessageRunner*		fRunner;
			MenuSearch			fSearch;
			SearchResultList	fResults;
			int32				fGeneration;
			thread_id			fFileThread;
			volatile bool		fCancelFiles;
			bool				fFiles;
			bool				fTest;
};


#endif	// SEARCH_WINDOW_H
