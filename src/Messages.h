/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef MESSAGES_H
#define MESSAGES_H


enum {
	// from the helper
	kMsgProviders		= 'prvs',	// "provider" messages: "id", "name",
									// "kinds"
	kMsgResults			= 'rslt',	// "generation", then for each video
									// "provider", "kind", "id", "title",
									// "description", "channel", "channel id",
									// "duration", "year", "date", "views",
									// "rating", "poster", "thumbnail"
	kMsgDetails			= 'dtls',	// "id", "found", then the same fields
									// as above
	kMsgSearchFailed	= 'srfl',	// "generation"
	kMsgFound			= 'fnd ',	// "id", "url"
	kMsgLost			= 'lost',	// "id"
	kMsgHelperGone		= 'gone',
	kMsgDownloadProgress = 'dwpr',	// "id", "percent", and when known
									// "done", "total", "speed", "eta",
									// "peers"
	kMsgDownloaded		= 'dwok',	// "id", "path"
	kMsgDownloadFailed	= 'dwno',	// "id"
	kMsgDownloadCancelled = 'dwcx',	// "id"
	kMsgCancelDownload	= 'dwst',	// "id": stop it

	// to and from the worker
	kMsgThumbnails		= 'thms',	// "generation", "id"...
	kMsgThumbnail		= 'thmb',	// "generation", "id", "path"
	kMsgFetchPicture	= 'fpic',	// "id", "url": a big picture, for the
									// detail view
	kMsgPicture			= 'pict',	// "id", "path"
	kMsgFollow			= 'folw',	// "id", "url", or "title" and "url"
									// for MediaPlayer
	kMsgStreamReady		= 'strm',	// "id", "url"
	kMsgFollowed		= 'flwd',	// "title", "url": for MediaPlayer

	// in the window
	kMsgPlay			= 'play',	// "id", "title", "path" if downloaded,
									// and what the history needs
	kMsgDownload		= 'dwnl',	// "provider", "kind", "id", "title",
									// "channel", "channel id", "duration"
	kMsgShowDownloads	= 'dwsh',
	kMsgHome			= 'home',
	kMsgShelfMove		= 'shmv',
	kMsgShelfFocus		= 'shfc',
	kMsgTrash			= 'trsh',	// "id", "path": a download to the Trash
	kMsgSelected		= 'slct',	// "id": chosen, not played yet
	kMsgShowDetails		= 'dtsh',	// "provider", "kind", "id", "title",
									// "channel", "channel id", "duration",
									// "date", "views", "path", "downloaded",
									// "progress"
	kMsgCloseDetails	= 'dtcl',
	kMsgPrefetch		= 'pref',	// "id"
	kMsgCategory		= 'catg',	// "index"
	kMsgChannel			= 'chan',	// "channel", "channel id"
	kMsgAddChannel		= 'addc',	// the channel shown, to the tabs
	kMsgRemoveCategory	= 'rmct',	// "index"
	kMsgSearchField		= 'sfld',	// the search field was confirmed
	kMsgSearchSource	= 'ssrc',	// "provider": where to search

	// the player
	kMsgPlayerOpened	= 'plop',	// "duration", "width", "height"
	kMsgPlayerFrame		= 'plfr',	// a new picture to show
	kMsgPlayerEnded		= 'plen',
	kMsgPlayerError		= 'pler',	// it cannot play it
	kMsgPlayerReopen	= 'plro',	// "time": a stream of the relay, opened
									// again from there to go there
	kMsgPlayerTick		= 'pltk',	// the time shown, the controls hidden
	kMsgPlayerSave		= 'plsv',	// where it is, to go on later
	kMsgPlayerClose		= 'plcl',
	kMsgToggleOverlay	= 'plov',	// the O key: the overlay off or on
	kMsgPlayInMediaPlayer = 'plmp',	// the same address, in MediaPlayer
	kMsgSubtitles		= 'plts',	// "id", then for each track
									// "language", "name", "automatic"
	kMsgSubtitle		= 'pltx',	// "id", "language", "automatic",
									// "text"
	kMsgSubtitleRequest	= 'pltr',	// "language", "automatic": its text
	kMsgSubtitlePick	= 'pltp',	// "track": chosen in the player
	kMsgSubtitleChosen	= 'pltc',	// a track or none chosen: kept for
										// the next videos
	kMsgPlayerFavorite	= 'plfv',	// the star of the video playing

	// the settings
	kMsgShowSettings	= 'stng',
	kMsgShowAbout		= 'abou',
	kMsgSettingsChanged	= 'stch',
	kMsgSettingsClosed	= 'stcl',
	kMsgSettingsFolders	= 'stfd',	// "path": the folders of the library,
									// "tv": the lists of TV channels
	kMsgAddFolder		= 'fadd',	// "path"
	kMsgRemoveFolder	= 'frem',	// "path"
	kMsgFollowChannel	= 'folc',	// "channel", "channel id", "provider",
									// "follow": kept among the tabs or not
	kMsgToggleFavorite	= 'favt',	// the fields of the item, as for details
	kMsgAddTvList		= 'tvad',	// "address": of a list M3U, or its path
	kMsgRemoveTvList	= 'tvrm',	// "address"
	kMsgCheckTmdb		= 'tmck',
	kMsgTmdbChecked		= 'tmok',	// "ok"
	kMsgAnimate			= 'anim',
	kMsgFullScreen		= 'fuls',	// F: full screen, or back
	kMsgDirectDraw		= 'pdrd',	// draw the direct full screen
	kMsgDirectFallback	= 'pdfb',	// no direct connection: back to the view
	kMsgFocusContent	= 'fcnt',	// leave the tabs, back to the videos
};


#endif	// MESSAGES_H
