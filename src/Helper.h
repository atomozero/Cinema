/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef HELPER_H
#define HELPER_H


#include <Locker.h>
#include <Messenger.h>
#include <String.h>

#include <atomic>


class Json;


/*!	cinema-helper, the Python program next to Cinema that keeps the providers
	loaded: today yt-dlp. It searches and finds the files of the videos, and
	talks one JSON object per line. Its answers are sent to the target as
	kMsgProviders, kMsgResults or kMsgSearchFailed, kMsgDetails, kMsgFound or
	kMsgLost, and kMsgDownloadProgress then kMsgDownloaded or
	kMsgDownloadFailed.
*/
class Helper {
public:
								Helper(BMessenger target);
								~Helper();

			status_t			Start(const char* script, const char* cache);
			status_t			Restart();
									// after kMsgHelperGone: a new helper
			bool				WasReady() const { return fReady; }
									// whether the last one ever worked
			void				SetProvider(const char* id);
									// the one the next requests are for
			void				SetHeight(int32 height);
									// of the pictures found and downloaded
									// next, 0 for what suits the machine
			void				Search(int32 generation, int32 count,
									const char* query,
									const char* provider = NULL,
									const char* order = NULL);
									// order: "newest" first, or as the
									// source gives them
			void				Channel(int32 generation, int32 count,
									const char* id,
									const char* provider = NULL);
			void				Details(const char* provider, const char* id,
									const char* kind);
			void				Find(const char* id,
									const char* provider = NULL);
			void				Subtitles(const char* id,
									const char* provider = NULL,
									const char* kind = NULL);
			void				Subtitle(const char* id, const char* language,
									bool automatic,
									const char* provider = NULL,
									const char* kind = NULL);
			void				Download(const char* id, const char* folder,
									const char* provider = NULL);
			void				Cancel(const char* id);
			void				Pause(const char* id);
									// answered with kMsgDownloadPaused
			void				Discard(const char* id, const char* folder,
									const char* provider);
									// what a download paused had got
			void				Upload(const char* id, const char* path,
									const char* target, const char* user,
									const char* password);
									// kMsgUploadProgress, then kMsgUploaded
			void				CheckUpload(const char* target,
									const char* user, const char* password);
									// answered with kMsgUploadChecked
			void				Organize(const char* id, const char* path,
									const char* root, const BMessage& video,
									const BMessage& names);
									// "provider", "kind", "title",
									// "channel"; names: "movies", "series",
									// "videos", "season" (with %d);
									// answered with kMsgOrganized
			void				SetYouTubeAccount(bool enabled);
									// its pages read with the cookies of
									// WebPositive; kept for a new helper
			void				CheckYouTube();
									// answered with kMsgYouTubeChecked
			void				CheckTmdb();
									// answered with kMsgTmdbChecked
									// NULL or empty: the default provider

private:
	static	status_t			_ReaderEntry(void* data);
			void				_Read();
			void				_Handle(const BString& line);
			void				_Send(const Json& request);
			BString				_Provider(const char* provider);
			void				_Stop();

			BMessenger			fTarget;
			BLocker				fLock;
			BString				fProvider;	// "youtube" until it says
			int32				fHeight;
			bool				fYouTubeAccount = false;
			int					fInput;		// its standard input
			int					fOutput;	// its standard output
			pid_t				fProcess;
			thread_id			fReader;
			BString				fScript;
			BString				fCache;
			volatile bool		fReady;		// it said "ready"
			std::atomic<bool>	fStopping;
};


#endif	// HELPER_H
