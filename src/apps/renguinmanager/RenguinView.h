/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RENGUIN_VIEW_H
#define RENGUIN_VIEW_H


#include <GroupView.h>


class BButton;
class BCheckBox;
class BStringView;
class BTextControl;


/*!	Renguin Manager's page (batch 5): the Linux compatibility layer's
	state, the Linux root, its packages and the diagnostics trace. Used by
	the RenguinManager program and the Control Panel ("Linux programs").
*/
class RenguinView : public BGroupView {
public:
								RenguinView();

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

			void				Refresh();

private:
			void				_SaveTrace();

			BStringView*		fLayer;
			BStringView*		fRoot;
			BStringView*		fRelease;
			BStringView*		fPackages;
			BButton*			fShell;
			BButton*			fSetup;
			BButton*			fUpgrade;
			BButton*			fDeskbar;
			BCheckBox*			fTrace;
			BTextControl*		fTracePattern;
			BCheckBox*			fTraceBuffer;
};


#endif	// RENGUIN_VIEW_H
