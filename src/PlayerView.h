/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef PLAYER_VIEW_H
#define PLAYER_VIEW_H


#include <String.h>
#include <View.h>

#include <vector>


class BBitmap;
class BMessageRunner;
class Player;


/*!	The picture of the player, fitted to the view, and its controls over
	it: the title at the top, a bar with the time and where it is at the
	bottom. They show with the mouse or a key, and hide while it plays.

	Space or Enter pauses, the arrows go back and on by 10 seconds (a minute
	with Shift) and change the volume (shown upright at the right, as the
	keys; the wheel and a click on it change it too), M plays it in
	MediaPlayer instead, I shows what the player does (small, at the top), V
	chooses the subtitles, S gives the video a star; a click on the bar goes
	there, on CC chooses the subtitles, on the star next to it gives or
	takes the star, a click elsewhere pauses. The window closes it, with Esc.
*/
class PlayerView : public BView {
public:
								PlayerView(Player* player);
	virtual						~PlayerView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				KeyDown(const char* bytes, int32 count);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MessageReceived(BMessage* message);

			void				Start(const char* title);
			void				SetOpened(bool opened);
			void				SetOverlay(BBitmap* overlay);
									// the screen shows the pictures by itself
			void				ClearOverlay();
			void				RefreshSubtitles();
									// the text of a track arrived
			void				DrawOverlay(BView* target);
			bool				HasOverlayContent() const;
			void				SetDirect(bool direct);
			void				SetFavorite(int32 favorite);
			void				SetOverlayQuestion(const char* card);
									// 1 with a star, 0 without, -1 none
									// to give (live TV, an address)

private:
			BRect				_PictureFrame(const BBitmap* picture) const;
			bool				_PlaceOverlay();
			BRect				_BarFrame() const;
			BRect				_BarFrame(const BRect& bounds) const;
			BRect				_TrackFrame() const;
			BRect				_TrackFrame(const BRect& bounds) const;
			BRect				_SubtitleButtonFrame() const;
			BRect				_SubtitleButtonFrame(const BRect& bounds) const;
			BRect				_VolumeFrame(const BRect& bounds) const;
			BRect				_VolumeTrack(const BRect& bounds) const;
			BRect				_FavoriteButtonFrame() const;
			BRect				_QuestionFrame() const;
			BRect				_YesFrame() const;
			BRect				_NoFrame() const;
			void				_DrawQuestion(BView* target);
			void				_UpdateStatistics();
			void				_DrawStatistics(BView* target);
			BRect				_FavoriteButtonFrame(const BRect& bounds) const;
			void				_ShowControls();
			void				_TogglePause();
			void				_SeekBy(bigtime_t delta);
			void				_DrawControls(BView* target);
			void				_DrawSubtitle(BView* target);
			void				_SubtitleLayout(const BString& text,
									std::vector<BString>& lines,
									BRect& frame) const;
			void				_UpdateSubtitle();
			void				_ShowSubtitleMenu();
			void				_ChooseSubtitle(int32 index);
			void				_RequestSelectedSubtitle();

			Player*				fPlayer;
			BString				fTitle;
			bool				fOpened;
			bool				fDirect;
			BBitmap*			fOverlay;
			rgb_color			fKey;			// where the overlay shows
			bool				fOverlayShown;	// a first picture came
			bool				fControls;
			bigtime_t			fControlsUntil;
			BPoint				fLastMouse;
			bigtime_t			fMouseMovedAt = 0;
			BPoint				fTickPointer;
			bool				fCursorObscured = false;
			BMessageRunner*		fTicker;
			std::vector<BString> fSubtitleLines;
			BRect				fSubtitleRect;
			BString				fSubtitleShown;
			float				fSubtitleWidth;
			float				fSubtitleHeight;
			bool				fSubtitleControls;
			int32				fFavorite = -1;
			bool				fAsking = false;	// whether the video shows
			BString				fCard;
			bool				fShowStats = false;	// the I key
			std::vector<BString> fStatsLines;
			BRect				fStatsRect;
};


#endif	// PLAYER_VIEW_H
