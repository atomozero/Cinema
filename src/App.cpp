/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include <Application.h>
#include <Entry.h>
#include <Mime.h>
#include <Path.h>
#include <String.h>

#include "LiveAddress.h"
#include "CinemaWindow.h"
#include "Messages.h"
#include "Provider.h"


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
			BString id = YouTubeVideoId(argv[i]);
			if (!id.IsEmpty()) {
				BMessage play(kMsgPlay);
				play.AddString("id", id);
				play.AddString("title", argv[i]);
				_Window()->PostMessage(&play);
				continue;
			}
			BString address(argv[i]);
			// a magnet link, or the address or path of a .torrent: downloaded,
			// and played while it comes
			if (address.IStartsWith("magnet:")
				|| address.IEndsWith(".torrent")) {
				BMessage torrent(kMsgAddTorrent);
				torrent.AddString("source", address);
				_Window()->PostMessage(&torrent);
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
				_Window()->PostMessage(&add);
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
				_Window()->PostMessage(&torrent);
			} else
				_PlayAddress(path.Path(), path.Leaf());
		}
	}

	virtual void ReadyToRun()
	{
		_RegisterTypes();
		_Window()->Show();
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
		_Window()->PostMessage(&play);
	}

	CinemaWindow* _Window()
	{
		if (fWindow == NULL)
			fWindow = new CinemaWindow;
		return fWindow;
	}

	CinemaWindow*	fWindow = NULL;
};


int
main()
{
	App app;
	app.Run();
	return 0;
}
