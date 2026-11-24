/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include <Application.h>
#include <Catalog.h>
#include <Roster.h>
#include <string.h>
#include <Entry.h>
#include <LocaleRoster.h>
#include <Mime.h>
#include <Path.h>
#include <String.h>

#include "Timing.h"


bigtime_t gTimingStart = system_time();
#include "Feeds.h"
#include "LiveAddress.h"
#include "CinemaWindow.h"
#include "Messages.h"
#include "Provider.h"
#include "Scripting.h"


static const char* kSignature = "application/x-vnd.atomozero-Cinema";


class App : public BApplication {
public:
	App()
		:
		BApplication(kSignature)
	{
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		// a YouTube address, another address or a file: played right away
		for (int32 i = 1; i < argc; i++) {
			// the notification of a video got ready behind, clicked
			if (strcmp(argv[i], "--show-player") == 0) {
				_Post(kMsgShowPlayer);
				continue;
			}
			BString id = YouTubeVideoId(argv[i]);
			if (!id.IsEmpty()) {
				BMessage play(kMsgPlay);
				play.AddString("id", id);
				play.AddString("title", argv[i]);
				_Post(&play);
				continue;
			}
			BString address(argv[i]);
			// a magnet link, or the address or path of a .torrent: downloaded,
			// and played while it comes
			if (address.IStartsWith("magnet:")
				|| address.IEndsWith(".torrent")) {
				BMessage torrent(kMsgAddTorrent);
				torrent.AddString("source", address);
				_Post(&torrent);
				continue;
			}
			// the feed of a podcast: a tab of its episodes
			if (IsFeedAddress(address)) {
				BMessage podcast(kMsgAddPodcast);
				podcast.AddString("address", address);
				_Post(&podcast);
				continue;
			}
			// a live address too, which plays with FFmpeg: a file such as
			// cinema-live:?url=%2Fboot%2Fhome%2Fvideo.mp4 as well
			if (address.IStartsWith("http://")
				|| address.IStartsWith("https://")
				|| LiveAddress::Is(argv[i])) {
				_PlayAddress(argv[i], argv[i]);
				continue;
			}
			BEntry entry(argv[i], true);
			BPath path;
			if (entry.GetPath(&path) != B_OK)
				continue;
			if (entry.IsDirectory()) {
				// a folder: to the library
				BMessage add(kMsgAddFolder);
				add.AddString("path", path.Path());
				_Post(&add);
			} else if (entry.IsFile())
				_PlayAddress(path.Path(), path.Leaf());
		}
	}

	virtual void RefsReceived(BMessage* message)
	{
		// opened with Cinema from Tracker
		entry_ref ref;
		for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
			BEntry entry(&ref, true);
			BPath path;
			if (!entry.IsFile() || entry.GetPath(&path) != B_OK)
				continue;
			if (BString(path.Leaf()).IEndsWith(".torrent")) {
				BMessage torrent(kMsgAddTorrent);
				torrent.AddString("source", path.Path());
				_Post(&torrent);
			} else
				_PlayAddress(path.Path(), path.Leaf());
		}
	}

	virtual BHandler* ResolveSpecifier(BMessage* message, int32 index,
		BMessage* specifier, int32 what, const char* property)
	{
		// the properties of Cinema: answered here, by its window locked
		// (forwarded to it, they lost their specifier on the way)
		if (what == B_DIRECT_SPECIFIER && is_cinema_property(property))
			return this;
		return BApplication::ResolveSpecifier(message, index, specifier,
			what, property);
	}

	virtual void MessageReceived(BMessage* message)
	{
		// other languages chosen in the preferences: Cinema follows them,
		// for what it is sent (titles, dates, plots) at once
		if (message->what == B_LOCALE_CHANGED) {
			BLocaleRoster::Default()->Refresh();
			// the words of Cinema in the language now preferred: what is
			// written from now on, and what the window writes again
			app_info info;
			if (GetAppInfo(&info) == B_OK)
				B_CATALOG->SetTo(info.ref);
			_Post(kMsgLanguagesChanged);
			return;
		}
		if (message->what == B_GET_PROPERTY || message->what == B_SET_PROPERTY
			|| message->what == B_EXECUTE_PROPERTY) {
			CinemaWindow* window = _Window();
			bool handled = false;
			// locked through its messenger: only while it is there
			if (fWindowMessenger.LockTarget()) {
				handled = window->Scripting(message);
				window->Unlock();
			}
			if (handled)
				return;
		}
		BApplication::MessageReceived(message);
	}

	virtual status_t GetSupportedSuites(BMessage* data)
	{
		data->AddString("suites", kCinemaSuite);
		BPropertyInfo info(const_cast<property_info*>(kCinemaProperties));
		data->AddFlat("messages", &info);
		return BApplication::GetSupportedSuites(data);
	}

	virtual void ReadyToRun()
	{
		_RegisterTypes();
		TimingMark("window made");
		_Window()->Show();
		TimingMark("window shown");
	}

private:
	/*!	Cinema opens the magnet links of the browser and the .torrent files
		of Tracker, unless another program already does: it takes no type
		from it.
	*/
	void _RegisterTypes()
	{
		static const struct {
			const char* type;
			const char* description;
			const char* extension;
		} kTypes[] = {
			{ "application/x-vnd.Be.URL.magnet", "Magnet link", NULL },
			{ "application/x-bittorrent", "BitTorrent file", "torrent" },
		};
		for (const auto& known : kTypes) {
			BMimeType type(known.type);
			if (type.InitCheck() != B_OK)
				continue;
			if (!type.IsInstalled()) {
				if (type.Install() != B_OK)
					continue;
				type.SetShortDescription(known.description);
				if (known.extension != NULL) {
					BMessage extensions;
					extensions.AddString("extensions", known.extension);
					type.SetFileExtensions(&extensions);
				}
			}
			char preferred[B_MIME_TYPE_LENGTH];
			if (type.GetPreferredApp(preferred) != B_OK
				|| preferred[0] == '\0'
				|| !BMimeType(preferred).IsInstalled())
				type.SetPreferredApp(kSignature);
		}
	}

	void _PlayAddress(const char* address, const char* title)
	{
		BMessage play(kMsgPlay);
		play.AddString("id", address);
		play.AddString("title", title);
		play.AddString("provider", "local");
		play.AddString("path", address);
		_Post(&play);
	}

	CinemaWindow* _Window()
	{
		if (fWindow == NULL) {
			fWindow = new CinemaWindow;
			fWindowMessenger = BMessenger(fWindow);
		}
		return fWindow;
	}

	// to the window through its messenger: once it quit, nothing, rather
	// than a deleted window
	void _Post(BMessage* message)
	{
		_Window();
		fWindowMessenger.SendMessage(message);
	}

	void _Post(uint32 what)
	{
		BMessage message(what);
		_Post(&message);
	}

	CinemaWindow*	fWindow = NULL;
	BMessenger		fWindowMessenger;
};


int
main()
{
	TimingMark("start");
	App app;
	app.Run();
	return 0;
}
