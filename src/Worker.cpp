/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Worker.h"

#include <Autolock.h>
#include <Bitmap.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <MessageQueue.h>
#include <Node.h>
#include <Path.h>

#include <algorithm>

#include <ctype.h>
#include <vector>

#include "Deliver.h"
#include "Messages.h"
#include "Pictures.h"
#include "Process.h"
#include "PictureName.h"


Worker::Worker(BMessenger target)
	:
	BLooper("worker"),
	fTarget(target),
	fStopping(false),
	fPictureLock("worker pictures")
{
	BPath path;
	if (find_directory(B_USER_CACHE_DIRECTORY, &path) == B_OK) {
		path.Append("Cinema");
		create_directory(path.Path(), 0755);
		fCache = path.Path();
	}
}


void
Worker::Stop()
{
	fStopping = true;
	// kept until the team goes, so that no other picture is begun
	fPictureLock.Lock();
}


void
Worker::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgThumbnails:
			_Thumbnails(message);
			break;
		case kMsgFetchPicture:
			_Picture(message);
			break;
		case kMsgFollow:
			_Follow(message);
			break;
		default:
			BLooper::MessageReceived(message);
	}
}


/*!	One picture, to the file asked; false if it could not be got. */
static const char* kSourceAttribute = "Cinema:source";


/*!	An address curl may be given: one starting with "-" would be taken for
	an option of it.
*/
static bool
youtube_id(const char* id)
{
	// eleven letters, digits, - and _
	int32 length = 0;
	for (const char* c = id; *c != '\0'; c++, length++) {
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_')
			return false;
	}
	return length == 11;
}


static bool
fetchable(const BString& url)
{
	return url.StartsWith("http://") || url.StartsWith("https://")
		|| url.StartsWith("file://");
}


/*!	Whether the picture kept at path is still the one at url: the address
	it came from is kept in an attribute. One kept before that, of a film
	or a series of the library (its id a path or "series:..."), may be a
	frame of the file got before TMDB had a poster for it: got again.
*/
static bool
kept_picture(const BString& path, const BString& url, const char* id)
{
	BNode node(path.String());
	if (node.InitCheck() != B_OK)
		return false;
	BString source;
	if (node.ReadAttrString(kSourceAttribute, &source) == B_OK)
		return url.IsEmpty() || source == url;
	BString name(id);
	return url.IsEmpty() || !url.StartsWith("http")
		|| (name.FindFirst('/') < 0 && name.FindFirst(':') < 0);
}


static void
keep_source(const BString& path, const BString& url)
{
	if (url.IsEmpty())
		return;
	BNode node(path.String());
	if (node.InitCheck() == B_OK)
		node.WriteAttrString(kSourceAttribute, &url);
}


bool
Worker::_Get(const char* url, const char* path)
{
	BString temporary(path);
	temporary << ".part";
	// a picture: never more than 10 MB
	if (!fetchable(url) || RunProgram({ "curl", "-sfL", "--max-time", "30",
			"--max-filesize", "10M", "-o", temporary,
			url }) != 0 || BEntry(temporary.String()).Rename(path, true)
				!= B_OK) {
		BEntry(temporary.String()).Remove();
		return false;
	}
	return true;
}


/*!	The pictures, a few at a time: one curl for several, so that the
	connection to the server and its TLS are made once. Each is read and
	made as small as it is shown ("largest") here rather than in the
	window, which then only shows it ("picture", its own to delete). When
	the pictures of another search wait, those not got yet go after them:
	the shelves of the home all ask at once.
*/
void
Worker::_Thumbnails(BMessage* message)
{
	static const int32 kBatch = 6;
	int32 generation = message->GetInt32("generation", 0);
	float largest = message->GetFloat("largest", Pictures::kTile);
	int32 count = 0;
	type_code type;
	message->GetInfo("id", &type, &count);

	for (int32 first = 0; first < count && !fStopping; first += kBatch) {
		// whatever else waits goes first: the pictures of another search,
		// the big one of a page, the address of a video to play
		if (first > 0 && !MessageQueue()->IsEmpty()) {
			BMessage rest(kMsgThumbnails);
			rest.AddInt32("generation", generation);
			rest.AddFloat("largest", largest);
			for (int32 i = first; i < count; i++) {
				rest.AddString("id", message->GetString("id", i, ""));
				rest.AddString("url", message->GetString("url", i, ""));
			}
			PostMessage(&rest);
			return;
		}

		int32 last = std::min(count, first + kBatch);
		std::vector<BString> paths;
		std::vector<BString> sources;
		std::vector<BString> arguments = { "curl", "-sfL", "--max-time", "60",
			"--max-filesize", "10M" };
		for (int32 i = first; i < last; i++) {
			const char* id = message->GetString("id", i, "");
			BString path(fCache);
			path << "/" << PictureName(id) << ".jpg";
			paths.push_back(path);
			// the address the provider gave, or YouTube's from the id
			BString url = message->GetString("url", i, "");
			sources.push_back(url);
			if (BEntry(path.String()).Exists()) {
				if (kept_picture(path, url, id))
					continue;
				BEntry(path.String()).Remove();
			}
			// YouTube's own picture only for the id of a video of YouTube:
			// a path, an address or an id of the home network is not sent
			// to it
			if (url.IsEmpty() && youtube_id(id))
				url.SetToFormat("https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
			if (!fetchable(url))
				continue;
			BString temporary(path);
			temporary << ".part";
			arguments.push_back("-o");
			arguments.push_back(temporary);
			arguments.push_back(url);
		}
		if (arguments.size() > 6) {
			// curl goes on after a picture that fails: each is looked at.
			// Out of time, the last may be half there: none is kept then.
			bool late = RunProgram(arguments) == 28;
			for (size_t i = 0; i < paths.size(); i++) {
				const BString& path = paths[i];
				BString temporary(path);
				temporary << ".part";
				BEntry part(temporary.String());
				if (!part.Exists())
					continue;
				if (late || part.Rename(path.String(), true) != B_OK)
					part.Remove();
				else
					keep_source(path, sources[i]);
			}
		}

		for (int32 i = first; i < last && !fStopping; i++) {
			const BString& path = paths[i - first];
			if (!BEntry(path.String()).Exists())
				continue;
			BMessage thumbnail(kMsgThumbnail);
			thumbnail.AddInt32("generation", generation);
			thumbnail.AddString("id", message->GetString("id", i, ""));
			thumbnail.AddString("path", path);
			BBitmap* picture = NULL;
			{
				BAutolock locker(fPictureLock);
				if (fStopping)
					return;
				// without app_server: the window makes it one of its own
				picture = Pictures::Read(path.String(), largest);
			}
			// not a picture, or a broken one: got again the next time
			if (picture == NULL)
				BEntry(path.String()).Remove();
			if (picture != NULL)
				thumbnail.AddPointer("picture", picture);
			if (Deliver(fTarget, &thumbnail, &fStopping) != B_OK)
				delete picture;
		}
	}
}


/*!	The big picture of a video, for the detail view. It goes to a file of its
	own, so that the small one in the grid is not replaced.
*/
void
Worker::_Picture(BMessage* message)
{
	BString id = message->GetString("id", "");
	BString url = message->GetString("url", "");
	if (id.IsEmpty() || url.IsEmpty())
		return;
	// a path or an address gives a hash, as for the small pictures
	BString path(fCache);
	path << "/poster-" << PictureName(id) << ".jpg";
	if (BEntry(path.String()).Exists() && !kept_picture(path, url, id))
		BEntry(path.String()).Remove();
	if (!BEntry(path.String()).Exists()) {
		if (_Get(url.String(), path.String()))
			keep_source(path, url);
		else {
			// the biggest picture may not be there for an old video of
			// YouTube; the one every video has is (only for those)
			if (!youtube_id(id.String()))
				return;
			BString fallback;
			fallback.SetToFormat("https://i.ytimg.com/vi/%s/hqdefault.jpg",
				id.String());
			if (!_Get(fallback.String(), path.String()))
				return;
		}
	}

	BMessage picture(kMsgPicture);
	picture.AddString("id", id);
	picture.AddString("path", path);
	Deliver(fTarget, &picture, &fStopping);
}


/*!	The addresses of YouTube's files go through two redirects, which
	MediaPlayer's HTTP streaming does not follow.
*/
void
Worker::_Follow(BMessage* message)
{
	BString url = message->GetString("url", "");
	BString final;
	// the relay of the helper starts a stream for each request
	if (url.StartsWith("http") && !url.StartsWith("http://127.0.0.1:")
		&& fetchable(url)
		&& RunProgram({ "curl", "-s", "-o", "/dev/null", "-L", "-r", "0-0",
			"--max-time", "20", "-w", "%{url_effective}", url }, &final) == 0
		&& final.StartsWith("http"))
		url = final;

	BMessage ready(kMsgStreamReady);
	if (message->HasString("title")) {
		ready.what = kMsgFollowed;
		ready.AddString("title", message->GetString("title", ""));
	} else
		ready.AddString("id", message->GetString("id", ""));
	ready.AddString("url", url);
	Deliver(fTarget, &ready, &fStopping);
}
