/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef VIDEO_GRID_H
#define VIDEO_GRID_H


#include <Message.h>
#include <String.h>
#include <View.h>

#include <vector>

#include "MediaItem.h"


class BBitmap;
class BMessageRunner;


/*!	What a channel is, shown above its videos. */
struct ChannelHeader {
	BString		provider;
	BString		id;
	BString		name;
	BString		handle;			// "@TEDEd"
	BString		description;
	BString		avatarUrl;
	int64		followers = 0;
	int32		followed = -1;	// among the tabs: 1, 0, or -1 for none
	bool		favorite = false;
};


/*!	The videos found, as big pictures in rows. A click, or Enter on the chosen
	one, sends kMsgPlay to the window; a click on the name of its channel
	sends kMsgChannel. D, or the menu of a right click, sends kMsgDownload. It can also show a message instead
	(while searching, when nothing was found), and a "getting it ready" banner
	over the pictures.
*/
class VideoGrid : public BView {
public:
								VideoGrid();
	virtual						~VideoGrid();

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MakeFocus(bool focus = true);
	virtual	void				KeyDown(const char* bytes, int32 count);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MessageReceived(BMessage* message);

			void				SetVideos(const std::vector<MediaItem>& videos);
			void				SetShelfMode(float tileWidth);
			float				ShelfHeight() const;
			static float			ShelfHeightFor(float tileWidth);
			bool				HasVideo(const char* id) const;
			bool				IsEmpty() const { return fVideos.empty(); }
			void				SetPicture(const char* id, BBitmap* picture);
			void				SetProgress(const char* id, int32 progress);
			void				SetStatus(const char* id, const char* status);
			void				SetWatched(const char* id, bool watched,
									int32 resume);
			void				Select(const char* id);
			bool				NextOf(const char* id, MediaItem& next) const;
									// the item after id, if there is one
									// -1: not downloading
			void				SetDownloaded(const char* id,
									bool downloaded);
			void				SetFavorite(const char* id, bool favorite);
			void				SetMessage(const char* message);
			void				SetBusy(const char* title);
									// NULL when no longer busy
			void				SetChannelHeader(const ChannelHeader& header);
									// above the videos of a channel: its
									// name, followers, a star and Follow
			void				ClearChannelHeader();
			bool				HasChannelHeader() const { return fHasHeader; }
			const ChannelHeader&	Header() const { return fHeader; }
			void				SetChannelAvatar(BBitmap* avatar);
									// its own to delete

private:
			void				_Clear();
			int32				_Columns() const;
			int32				_VisibleCount() const;
			BRect				_TileFrame(int32 index) const;
			int32				_TileAt(BPoint where) const;
			BRect				_ChannelFrame(int32 index) const;
			int32				_ChannelAt(BPoint where) const;
			void				_UpdateScrollBar();
			void				_Select(int32 index);
			void				_Play(int32 index);
			void				_Download(int32 index);
			void				_ShowDetails(int32 index);
			void				_ToggleFavorite(int32 index);
			BMessage			_DownloadMessage(int32 index) const;
			BMessage			_DetailsMessage(int32 index) const;
			void				_ShowMenu(int32 index, BPoint where);
			void				_DrawTile(int32 index, BRect frame);
			void				_DrawBusy();
			void				_DrawChannelTile(int32 index, BRect frame,
									bool selected, bool hover);
			float				_HeaderHeight() const;
			BRect				_HeaderFrame() const;
			BRect				_AvatarFrame() const;
			BRect				_StarFrame() const;
			BRect				_FollowFrame() const;
			int32				_HeaderButtonAt(BPoint where) const;
			void				_DrawHeader();

			std::vector<MediaItem>	fVideos;
			BString				fMessage;
			BString				fBusy;
			int32				fSelected;
			int32				fHover;
			int32				fHoverChannel;
			int32				fDots;
			bool				fShelf;
			float				fShelfTileWidth;
			BMessageRunner*		fAnimation;
			ChannelHeader		fHeader;
			bool				fHasHeader = false;
			BBitmap*			fAvatar = NULL;
			int32				fHeaderHover = 0;	// 1 star, 2 follow
};


#endif	// VIDEO_GRID_H
