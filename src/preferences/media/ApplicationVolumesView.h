/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APPLICATION_VOLUMES_VIEW_H
#define APPLICATION_VOLUMES_VIEW_H


#include <GroupView.h>
#include <ObjectList.h>
#include <String.h>

#include <AudioStreamControl.h>


class BCheckBox;
class BGridView;
class BMenuField;
class BMessageRunner;
class BSlider;
class BStringView;


/*!	Volume, mute and output device of every application that plays sound,
	from the system mixer (roadmap F-31/F-32). Used by the Media
	preferences ("Audio settings") and by the Deskbar's volume control
	popup (desklink compiles this file too). It refreshes itself once a
	second while it is shown.
*/
class ApplicationVolumesView : public BGroupView {
public:
								ApplicationVolumesView(const char* name,
									bool showOutputs);
	virtual						~ApplicationVolumesView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

			void				SetShowOutputs(bool show);
			bool				ShowOutputs() const { return fShowOutputs; }
			// A "Low latency" switch per application (Media preferences)
			void				SetShowLowLatency(bool show);
			int32				CountStreams() const
									{ return fRows.CountItems(); }

private:
	struct Row;

			void				_Refresh();
			void				_Rebuild();
			void				_UpdateValues();
			Row*				_FindRow(int32 id) const;

			BPrivate::AudioStreamControl fControl;
			BGridView*			fGrid;
			BStringView*		fEmptyLabel;
			BObjectList<Row, true> fRows;
			BMessageRunner*		fRunner;
			bool				fShowOutputs;
			bool				fShowLowLatency;
			BString				fLayoutKey;
};


#endif	// APPLICATION_VOLUMES_VIEW_H
