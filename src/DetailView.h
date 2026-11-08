/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef DETAIL_VIEW_H
#define DETAIL_VIEW_H


#include <Messenger.h>
#include <String.h>
#include <View.h>

#include <vector>

#include "MediaItem.h"


class BBitmap;


/*!	One video on a page of its own: its big picture, title, channel, when it
	was published, how long it is, how many watched it, and its description,
	which scrolls. The buttons send kMsgPlay and kMsgDownload to the window,
	the channel sends kMsgChannel, and Back or Esc sends kMsgCloseDetails.
*/
class DetailView : public BView {
public:
								DetailView(BMessenger target);
	virtual						~DetailView();

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				KeyDown(const char* bytes, int32 count);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MessageReceived(BMessage* message);

			void				SetItem(const MediaItem& item);
			void				SetPicture(BBitmap* picture);
									// takes it over
			void				SetProgress(const char* id, int32 progress);
			void				SetDownloaded(const char* id,
									bool downloaded);
			void				SetLoading(bool loading);
			void				SetFollowed(const char* channelId,
									int32 followed);
									// 1 among the tabs, 0 not, -1 nothing
									// to follow
			void				SetFavorite(const char* id, bool favorite);

			const MediaItem&	Item() const { return fItem; }

private:
			void				_Update();
			void				_Scroll(float delta);
			void				_ClampScroll();
			void				_Play();
			void				_Download();
			void				_Channel();
			void				_ToggleFollow();
			void				_Back();
			void				_ToggleFavorite();
			void				_Trash();
			bool				_CanTrash() const;
			void				_DrawStar();
			void				_DrawPoster();
			void				_DrawButton(BRect frame, const char* label,
									bool primary, bool enabled, bool hover);
			void				_DrawPlayButton();
			void				_DrawDescriptionLine(int32 index,
									BPoint at, const BFont& font);
			void				_FindLinks(int32 line);
			BString				_LinkAt(BPoint where) const;
			void				_OpenLink(const BString& url);
			int32				_ButtonAt(BPoint where) const;
			void				_InvalidateButton(int32 which);
			bool				_CanDownload() const;
			BString				_Meta() const;
			BString				_DownloadLabel() const;

			BMessenger			fTarget;
			MediaItem			fItem;
			BBitmap*			fPicture;
			struct Link {
				int32				line;
				int32				start;
				int32				end;
				BString				url;
			};

			std::vector<BString> fTitleLines;
			std::vector<BString> fLines;
			std::vector<Link> fLinks;
			BString				fMeta;
			float				fScroll;
			float				fLineHeight;
			BRect				fPosterFrame;
			BRect				fTextFrame;
			BRect				fDescFrame;
			BRect				fMetaFrame;
			BRect				fPlayFrame;
			BRect				fDownloadFrame;
			BRect				fBackFrame;
			BRect				fStarFrame;		// a favorite or not
			BRect				fTrashFrame;	// downloaded videos only
			BRect				fChannelFrame;
			BRect				fFollowFrame;	// follow its channel
			int32				fFollowed = -1;
			int32				fHover;
			bool				fLoading;
};


#endif	// DETAIL_VIEW_H
