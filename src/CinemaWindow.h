/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef CINEMA_WINDOW_H
#define CINEMA_WINDOW_H


#include <DirectWindow.h>
#include <Locker.h>
#include <Message.h>
#include <Messenger.h>
#include <String.h>
#include <Window.h>

#include <map>
#include <set>
#include <vector>
#include <deque>

#include "Categories.h"
#include "Settings.h"
#include "MediaItem.h"
#include "Pictures.h"
#include "Provider.h"


class BBitmap;
class BCardLayout;
class BMenuField;
class BView;
class BMessageRunner;
class Player;
class PlayerView;
class BTextControl;
class CategoryBar;
class DetailView;
class Gamepad;
class Helper;
class HomeView;
class VideoGrid;
class Worker;


class CinemaWindow : public BDirectWindow {
public:
								CinemaWindow();
	virtual						~CinemaWindow();

	virtual	void				DispatchMessage(BMessage* message,
									BHandler* handler);
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				DirectConnected(direct_buffer_info* info);
	virtual	bool				QuitRequested();

			void				SetDirectPaused(bool paused);

private:
	struct Stream {
		BString		url;
		bigtime_t	found;
	};

			void				_Search(const char* query,
									const char* provider = NULL);
			BString				_ProviderOf(const BString& id) const;
			void				_UpdateSources();
			void				_AddTorrent(const char* source);
			void				_AddFolder(const char* path);
			void				_AddTvList(const char* address,
									const char* name = NULL);
			void				_OfferTv();
			void				_UpdateTopBar();
			void				_ToggleFavorite(BMessage* message);
			void				_Dropped(BMessage* message);
			BString				_DownloadStatus(const BMessage& download) const;
			void				_DownloadCancelled(BMessage* message);
			void				_SavePending();
			void				_OpenPlayer(const char* address,
									const char* title, const char* id);
			void				_ClosePlayer(bool save = true);
			void				_RequestSubtitle(const char* language,
									bool automatic);
			void				_RequestSelectedSubtitle();
			void				_SavePosition();
			void				_ShowSettings();
			void				_SendFolders();
			void				_AddLibraryTabs();
			void				_ShowAbout();
			void				_PlayInMediaPlayer(const char* address,
									const char* title, bool followed = false);
			void				_SetProviders(BMessage* message);
			void				_ShowCategory(int32 index);
			void				_ShowChannel(const char* name,
									const char* id);
			void				_AddChannel(const char* name, const char* id);
			void				_RemoveCategory(int32 index);
			void				_ShowDownloads();
			void				_ShowHome();
			void				_FocusCategoryBar();
			void				_FocusContent();
			void				_ToggleCategoryFocus();
			void				_LoadHome();
			void				_StartHomeRequests();
			void				_HomeResults(BMessage* message, int32 shelf);
			void				_HomeFailed(BMessage* message, int32 shelf);
			std::vector<MediaItem> _Videos(BMessage* message);
			void				_Thumbnails(
								const std::vector<MediaItem>& videos,
								int32 generation,
								float largest = Pictures::kTile);
			bool				_ShowingDownloads() const;
			void				_ShowDetails(BMessage* message);
			void				_Details(BMessage* message);
			void				_Picture(BMessage* message);
			void				_CloseDetails();
			void				_Download(BMessage* message);
			void				_Downloaded(BMessage* message);
			void				_DownloadFailed(BMessage* message);
			void				_Trash(BMessage* message);
			void				_ShowResults(BMessage* message);
			bool				_Ready(const BString& id) const;
			void				_HomeChanged();
			void				_ApplyOverlay();
			void				_LowerQuality(int32 height);
			void				_ChangeQuality(bool up);
			void				_FindAtHeight(int32 height);
			void				_HidePlayer();
			void				_ShowHiddenPlayer();
			void				_PlayerOpened();
			void				_OfferInstall();
			void				_Install(const char* packages);
			BString				_OverlayCard() const;
			int32				_OverlayDecision() const;
			void				_RememberOverlay(bool works);
			int32				_Followed(const MediaItem& item) const;
			void				_SetChannelHeader(const char* name,
									const char* id, const char* provider);
			void				_Find(const BString& id);
			void				_PrefetchWanted();
			void				_Play(const char* url, const char* title,
									const char* id = NULL);
			void				_SetFullScreen(bool fullScreen);
			void				_DirectSync();
			void				_DirectDraw();
			void				_DirectDrawLocked();
			void				_DirectCreateLayerLocked();
			void				_DirectFillLocked(uint32 pixel);
			void				_DirectBlitLocked(BBitmap* picture);
			void				_DirectCompositeLocked();

			std::vector<Category> fCategories;
			std::vector<Provider> fProviders;	// as the helper says
			BString				fProvider;	// the one searched
			CategoryBar*		fCategoryBar;
			BTextControl*		fSearchField;
			BMenuField*			fSourceField;
			BString				fSearchProvider;	// chosen in fSourceField
			VideoGrid*			fGrid;
			DetailView*			fDetail;
			HomeView*			fHome;
			BCardLayout*		fCards;
			Player*				fPlayer;
			PlayerView*			fPlayerView;
			bool				fPlayerShown;
			int32				fPlayerReturn;	// the card before it
			BString				fPlayingId;
			BString				fPlayingAddress;
			BString				fPlayingTitle;
			MediaItem			fPlayingItem;	// as asked to play
			BString				fPlayingKind;	// "episode": the next follows
			BString				fNextKind;		// of the video about to play
			BMessageRunner*		fPlayerSaver;
			Settings			fSettings;
			BMessenger			fSettingsWindow;
			BString				fDetailsId;		// what the detail view shows
			bool				fDetailsShown;
			Helper*				fHelper;
			Gamepad*			fGamepad;
			Worker*				fWorker;
			int32				fGeneration;
			int32				fHomeGeneration;
			bool				fHomeLoaded;
			std::map<int32, int32> fHomeRequests;
			struct HomeRequest {
				int32			generation;
				int32			shelf;
				int32			category;
			};
			std::deque<HomeRequest> fHomeQueue;
			int32				fHomeActive;
			int32				fDetailsReturn;
			int32				fCard;

			std::map<BString, Stream> fStreams;	// the files found, by id
			std::set<BString>	fFinding;		// asked to the helper
			BString				fWaiting;		// clicked, not found yet
			BString				fWaitingTitle;
			BString				fRetried;	// found again after a failure
			bool				fHomeStale = false;
			bool				fAskOverlay = false;	// once the video plays
			BString				fLowered;		// found again smaller
			bigtime_t			fOpenAt = -1;
			BMessageRunner*		fOverlayAsker = NULL;	// made again when the details close
			int32				fHelperRestarts = 0;
			bool				fInstallOffered = false;
			// Esc while the video got ready: it goes on behind, muted, and
			// paused once open, until its notification is clicked
			bool				fPlayerHidden = false;
			float				fHiddenVolume = 1.0f;
			BString				fPendingTorrent;	// once aria2c is there
			bigtime_t			fHelperRestarted = 0;
			BString				fWanted;		// chosen: worth finding
			BMessageRunner*		fPrefetchRunner;
			bool				fFullScreen;
			bool				fDirectActive;
			bool				fDirectFailed;
			bigtime_t			fDirectStart;
			bool				fDirectFallbackAsked;
			bool				fDirectPaused;
			BLocker				fDirectLock;
			bool				fDirectConnected;
			void*				fDirectBits;
			int32				fDirectRowBytes;
			uint32				fDirectBpp;
			color_space			fDirectFormat;
			clipping_rect		fDirectBounds;
			std::vector<clipping_rect> fDirectClips;
			BBitmap*			fDirectUi;
			BView*				fDirectUiView;
			BMessageRunner*	fDirectRunner;
			BView*				fTopBar;	// the tabs and the search
			BString				fChannelName;	// shown in a tab of its own
			BString				fChannelId;
			BString				fChannelProvider;
			std::map<BString, BString> fItemProviders;
									// where each video shown comes from
			std::map<BString, BString> fChannelProviders;
			std::map<BString, BString> fDownloaded;
									// the files in the folder, by id
			std::set<BString>	fSubtitlesAsked;
			std::map<BString, int32> fSubtitleRetries;	// failed fetches
									// the subtitle tracks already asked
			std::map<BString, BMessage> fDownloading;
									// kMsgDownload, with "percent"
			BRect				fWindowFrame;	// to go back to
};


#endif	// CINEMA_WINDOW_H
