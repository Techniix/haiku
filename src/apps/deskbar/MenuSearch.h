/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MENU_SEARCH_H
#define MENU_SEARCH_H


/*!	The Deskbar menu's search (roadmap F-41, batch 9): one text, results
	from several sources, in this order:

	  calculator   an arithmetic expression ("2*(3+4)", "sqrt(2)") through
	               libshared's ExpressionParser, the same as DeskCalc
	  conversion   "<number> <unit> to|in <unit>" ("5 km to mi",
	               "100 f in c", "2 GiB in MB"), see MenuSearch.cpp
	  application  the entries of the Deskbar menu (applications,
	               preferences, demos, the user's own entries)
	  setting      the Control Panel's pages and settings, from the same
	               index the Control Panel searches (control_panel_index.h)
	  recent       recent documents and folders (BRoster)
	  file         files and folders by name, from a BFS query on the boot
	               volume (the name index; slow sources run on a thread, see
	               TSearchWindow)

	Kept free of any window, so the harness hook (TBarApp, kTestSearch)
	runs exactly what the window shows. Matching ignores case; a name
	starting with the text comes before one only containing it.
*/


#include <Entry.h>
#include <ObjectList.h>
#include <String.h>


enum search_result_kind {
	kResultCalculator = 0,
	kResultConversion,
	kResultApplication,
	kResultSetting,
	kResultRecentDocument,
	kResultRecentFolder,
	kResultFile
};


struct SearchResult {
	search_result_kind	kind;
	BString				title;
	BString				detail;
		// the kind's label, a path or the Control Panel page
	entry_ref			ref;
		// what opens (applications, recent items, files)
	BString				page;
		// Control Panel page id (settings)
	BString				value;
		// the calculator's or the conversion's result alone (copied)
	int32				rank;
		// lower first: 0 the text starts the name, 1 contains it
};


typedef BObjectList<SearchResult, true> SearchResultList;


class MenuSearch {
public:
								MenuSearch();
								~MenuSearch();

			void				SetCalculatorEnabled(bool enabled)
									{ fCalculator = enabled; }

			//! The quick sources: everything but files.
			void				SearchQuick(const char* text,
									SearchResultList& results);
			//!	Files by name; at most \a max, stops when \a cancel turns
			//	true (checked between entries).
	static	void				SearchFiles(const char* text, int32 max,
									SearchResultList& results,
									volatile bool* cancel = NULL);

			//! "= 42" for an expression, false when it is none.
	static	bool				Calculate(const char* text, BString& value);
			//! "3.10686 mi" for "5 km to mi", false when it is none.
	static	bool				Convert(const char* text, BString& value,
									BString& title);

	static	const char*			KindLabel(search_result_kind kind);

			//! Re-reads the Deskbar menu's entries (next search).
			void				Invalidate() { fApplicationsRead = false; }
			int32				CountApplications() const
									{ return fApplications.CountItems(); }

private:
			struct Application {
				BString		name;
				BString		lowerName;
				entry_ref	ref;
			};

			void				_ReadApplications();
			void				_ReadDirectory(const char* path, int32 depth);

private:
			bool				fCalculator;
			bool				fApplicationsRead;
			BObjectList<Application, true> fApplications;
};


#endif	// MENU_SEARCH_H
