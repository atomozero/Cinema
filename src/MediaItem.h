/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef MEDIA_ITEM_H
#define MEDIA_ITEM_H


#include <String.h>


class BBitmap;


/*!	One thing a provider can play. A video today; films and episodes of a
	series later, when kind is "movie" or "episode". The fields above the
	line come from the provider, the ones below are local: the file, when it
	was downloaded, and its picture.
*/
struct MediaItem {
	// from the provider
	BString		provider;		// "youtube"
	BString		id;				// within the provider
	BString		kind;			// "video", "movie", "episode", "live" (TV),
								// "series" (a folder or a series, opened
								// as a channel), "channel" (a star given)
	BString		title;
	BString		description;
	BString		channel;		// who made it
	BString		channelId;
	int32		duration;		// seconds
	int32		year;
	BString		date;			// when it was published, YYYY-MM-DD
	int64		views;
	double		rating;			// 0 to 10
	BString		poster;			// address of a big picture
	BString		thumbnail;		// address of a small one

	// local
	BString		path;			// the file, when downloaded
	BBitmap*	picture;		// may be NULL
	int32		progress;		// of its download, or -1
	BString		status;			// of its download: done, speed, time left
	BString		downloadState;	// "active", "queued", "paused", "pausing"
	bool		watched;		// played to the end
	int32		resume;			// how far it was played, in thousandths
	bool		downloaded;
	bool		favorite;		// given a star

			// something the helper can download: not live TV, a folder or a
			// channel, nor a file that is on this computer already
			bool		Downloadable() const
						{ return kind != "live" && kind != "series"
							&& kind != "channel" && provider != "local"
							&& provider != "library" && provider != "iptv"; }

				MediaItem() : duration(0), year(0), views(0), rating(0),
					picture(NULL), progress(-1), watched(false), resume(0),
					downloaded(false), favorite(false) {}
};


#endif	// MEDIA_ITEM_H
