/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef HOME_VIEW_H
#define HOME_VIEW_H


#include <Message.h>
#include <String.h>
#include <View.h>

#include <vector>

#include "MediaItem.h"


class BBitmap;
class BGroupView;
class VideoGrid;


struct ShelfSpec {
	BString				title;
	BMessage			seeAll;
	std::vector<MediaItem> videos;
	BString				message;

						ShelfSpec() : seeAll((uint32)0) {}
};


class HomeView : public BView {
public:
								HomeView();
	virtual						~HomeView();

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);

			void				SetShelves(
									const std::vector<ShelfSpec>& shelves);
			size_t				ShelfPictures() const;
									// the pictures a shelf keeps: no more
									// are worth getting
			void				SetShelfVideos(int32 index,
									const std::vector<MediaItem>& videos);
			void				SetShelfMessage(int32 index,
									const char* message);
			void				SetPicture(const char* id, BBitmap* picture);
			void				SetProgress(const char* id, int32 progress);
			void				SetFavorite(const char* id, bool favorite);
			void				SetDownloaded(const char* id,
									bool downloaded);
			void				SetBusy(const char* id, const char* title);
			void				ClearBusy();
			void				FailPending(const char* message);
			bool				MoveFocus(const char* id, int32 delta);
			void				MakeFirstFocus();
			void				MakeLastFocus();
			void				RememberFocus();
			void				SetMessage(const char* message);

private:
	struct Shelf {
		BGroupView*	group;
		VideoGrid*	row;
	};

			void				_Clear();
			void				_Relayout();
			void				_UpdateScrollBar();

			std::vector<Shelf>	fShelves;
			int32				fFocusedShelf;
			BString				fMessage;
			float				fContentHeight;
			bool				fLayingOut;
};


#endif	// HOME_VIEW_H
