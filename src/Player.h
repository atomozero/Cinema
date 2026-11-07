/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef PLAYER_H
#define PLAYER_H


#include <Locker.h>
#include <MediaDefs.h>
#include <Messenger.h>
#include <String.h>

#include <atomic>
#include <vector>

#include "Loudness.h"
#include "Subtitles.h"


class BBitmap;
class BMediaFile;
class BMediaTrack;
class BSoundPlayer;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct SwrContext;
struct SwsContext;


/*!	Plays a file or an address with the Media Kit: a thread decodes the
	sound into a ring the sound player takes it from, another the pictures,
	shown when the sound gets there (the sound is the clock; without sound,
	the system time is). Pictures too late are not shown, which keeps up on
	slow machines.

	It tells the target kMsgPlayerOpened ("duration", "width", "height"),
	kMsgPlayerFrame when there is a new picture to show, kMsgPlayerEnded,
	and kMsgPlayerError when it cannot play the file at all.

	A stream of the relay of the helper (http://127.0.0.1:<port>/<token>/
	<seconds>) cannot go back and on: it starts where it is asked
	(?t=<seconds>), and Seek() asks the target with kMsgPlayerReopen
	("time") to open it again from there.

	A live stream (LiveAddress, "cinema-live:?url=...") is played with
	FFmpeg instead (PlayerLive.cpp): it opens HLS by itself, with its
	sound apart or not, and keeps up on a slow machine by decoding less.
	It goes out the same way, by the ring of the sound and the bitmaps.
*/
class Player {
public:
								Player(const BMessenger& target);
								~Player();

			void				Open(const char* address,
									bigtime_t startAt = 0);
									// in a thread of its own: an address
									// may take a while
			void				Close();
			int32				Session() const { return fSession; }
									// in its messages: the window drops
									// those of a video closed since

			void				SetPaused(bool paused);
			bool				IsPaused() const { return fPaused; }
			void				Seek(bigtime_t time);
			void				Reopen(bigtime_t time);
									// the same video from there, keeping
									// its subtitles: how a video played
									// with FFmpeg seeks (kMsgPlayerReopen
									// asks the window, for the overlay)
			bigtime_t			Position() const;
			bigtime_t			Duration() const { return fDuration; }
			bool				IsOpen() const { return fOpened; }

			void				SetVolume(float volume);
			float				Volume() const { return fVolume; }
			void				SetNormalized(bool normalized)
									{ fLoudness.SetEnabled(normalized); }
									// the loudness evened (Loudness)

			int32				SubtitleCount() const;
			bool				SubtitleTrack(int32 index,
									SubtitleTrackInfo* info) const;
			int32				SelectedSubtitle() const;
			void				SelectSubtitle(int32 index);
			void				ToggleSubtitles();
			void				SetSubtitlePreference(const char* language)
									{ fSubtitles.SetPreference(language); }
			BString				SubtitleChoice() const
									{ return fSubtitles.Choice(); }
			BString				SubtitleText(bigtime_t time) const;
			int32				AddSubtitleTrack(const char* language,
									const char* name, bool automatic);
			void				LoadSubtitleText(const char* language,
									bool automatic, const char* text);

			BBitmap*			LockFrame();
									// the picture to show, NULL if none yet;
									// UnlockFrame() afterwards in any case
			void				UnlockFrame();
			BBitmap*			Overlay() const { return fOverlay; }
			BBitmap*			PausedPicture();
									// paused with the overlay: the picture
									// in RGB, for the view to draw instead
									// of the key color, so that a screenshot
									// shows it. NULL otherwise. In the
									// thread of the window only.
									// the picture the screen shows by
									// itself, when the overlay was free:
									// LockFrame() is not needed then
			void				DisableOverlay();
			void				NoteDrawn();
									// the view drew a picture of its own
									// (not the overlay): counted for the
									// statistics of CINEMA_PLAYER_DEBUG

private:
			void				_Start(const char* address,
									bigtime_t startAt,
									bool sameVideo = false);
			void				_Close(bool keepSubtitles);
			void				_DropKeptBitmaps();
			BBitmap*			fPausedPicture = NULL;
			int64				fPausedPictureOf = -1;	// fShownCount
			std::atomic<int64>	fShownCount{0};
			SwsContext*			fPauseScaler = NULL;
			void				_DropPausedPicture();
			BBitmap*			fKeptOverlay = NULL;	// see _Close()
			BBitmap*			fKeptFront = NULL;
			BBitmap*			fKeptBack = NULL;
			bool				fSameVideo = false;
	static	status_t			_OpenEntry(void* data);
			void				_Open();
	static	status_t			_VideoEntry(void* data);
			void				_VideoLoop();
	static	status_t			_AudioEntry(void* data);
			void				_AudioLoop();
	static	void				_PlayBuffer(void* cookie, void* buffer,
									size_t size,
									const media_raw_audio_format& format);
			void				_FillSound(void* buffer, size_t size);
			bigtime_t			_Clock() const;
			void				_SeekTracks(bigtime_t time);
			void				_CheckEnded();
			void				_FreeClock();
			void				_Failed();
			status_t			_Tell(BMessage* message);
			status_t			_Tell(uint32 what);
			bool				_SetUpVideo();
			void				_ShowFrame();
			bigtime_t			_StreamTime(bigtime_t time);
			bool				_SetUpBitmaps(int32 width, int32 height);

			// live, with FFmpeg (PlayerLive.cpp)
			bool				_OpenLive();
			void				_CloseLive();
		static	status_t			_LiveEntry(void* data);
			void				_LiveLoop();
			int					_LiveVideoStream(int32 maxHeight);
			void				_LiveSkip(int level);
			void				_LiveLateness(bool late);
			bool				_LiveRender(AVFrame* frame);
			void				_LiveQueueSound(const void* data,
									size_t bytes);
			void				_LiveAlignSound(int64 micros);
			void				_LivePushSound(AVFrame* frame);
			int64				_LiveFrameMicros(int index,
									const AVFrame* frame) const;
			int64				_LiveClock();
		static	int					_LiveInterrupt(void* data);

			BMessenger			fTarget;
			BString				fAddress;
			bigtime_t			fStartAt;
			bool				fRelay;			// a stream of the relay
			bigtime_t			fRelayDuration;
			std::atomic<bigtime_t> fFirstTime;	// of the stream, or -1
			// the time of the pictures, counted from where the video starts or
			// was sought to: the ffmpeg add-on gives start_time in the time
			// base of the stream, not in microseconds (its packets carry the
			// raw pts and dts), so header.start_time cannot be trusted
			bigtime_t			fFrameDuration = 0;	// 0 if the rate is unknown
			std::atomic<bigtime_t> fFrameBase{0};
			int64				fFrameCount = 0;	// with fVideoLock
			bigtime_t			fStatFrameTime = 0;
			int32				fFormatsAsked = 0;

			bool				fLive;			// live TV, played with FFmpeg
			bool				fFFmpeg = false;	// a file, YouTube or the
										// relay played with FFmpeg too:
										// its times can be trusted, and the
										// overlay comes in YCbCr422
			int64				fLiveSkipBefore = 0;	// stream microseconds:
										// what comes before, after a seek,
										// is not played; INT64_MIN for none
			std::atomic<bool>	fNoOverlay;

			AVFormatContext*	fLiveFormat;
			AVCodecContext*		fLiveVideo;
			AVCodecContext*		fLiveAudio;
			void*				fLiveHwOpaque;
			SwrContext*			fLiveResampler;
			SwsContext*			fLiveScaler;
			int					fLiveVideoIndex;
			int					fLiveAudioIndex;
			int					fLiveSkipLevel;
			int					fLiveLate;
			int					fLiveOnTime;
			int					fLivePixels;	// AVPixelFormat of the bitmaps
			int64				fLiveBase;		// stream time of the clock's 0
			int64				fLiveSoundEnd;	// stream time of the sound queued
			std::vector<uint8>	fLivePcm;
			std::vector<float>	fLiveFloat;	// out of the resampler
			Loudness			fLoudness;
			std::atomic<bool>	fLiveStopRead;	// the reader of the stream stops

			Subtitles			fSubtitles;

			BMediaFile*			fFile;
			BMediaTrack*		fVideoTrack;
			BMediaTrack*		fAudioTrack;
			BLocker				fVideoLock;		// around the reading of a track
			BLocker				fAudioLock;
			bigtime_t			fDuration;

			BBitmap*			fFront;			// shown
			BBitmap*			fBack;			// decoded into
			BBitmap*			fOverlay;		// what the screen shows, or NULL
			BLocker				fFrameLock;
			bool				fHasFrame;

			BSoundPlayer*		fSoundPlayer;
			media_raw_audio_format fSound;
			size_t				fFrameSize;		// of a sound frame, in bytes
			bigtime_t			fLatency;
			std::atomic<bigtime_t> fLastFill;	// when the sound was last taken
			bigtime_t			fFillTime;		// what a sound buffer lasts
			std::vector<uint8>	fRing;
			size_t				fRingRead;
			size_t				fRingFill;
			BLocker				fRingLock;

			// the clock: fClockBase when fPlayed sound frames are 0, or when
			// fClockStart was the system time without sound
			std::atomic<bigtime_t> fClockBase;
			std::atomic<int64>	fPlayed;
			// when each sound buffer is heard: the sound player's node plays
			// buffer n at fPerformanceStart plus the frames sent before it, in
			// the time of its time source (MediaPlayer's model). Latency()
			// alone left the sound 300 ms behind the pictures on a VAIO.
			struct SoundFill {
				int64		sent;		// frames sent before this buffer
				int64		content;	// fPlayed when it was filled
				int64		frames;		// frames of sound in it, not silence
			};
			mutable BLocker		fFillLock{"player fills"};
			SoundFill			fFills[64];
			int32				fFillCount = 0;	// with fFillLock
			int32				fFillNext = 0;
			int64				fSentFrames = 0;
			std::atomic<bigtime_t> fPerformanceStart{0};	// 0 if unknown
			void				_NoteFill(size_t requested, size_t content);
			void				_ApplyVolume();
			bigtime_t			fClockStart;
			bigtime_t			fPausedClock;

			std::atomic<bool>	fPaused;
			std::atomic<bool>	fQuit;
			std::atomic<int32>	fSession;	// one more each time it opens
			std::atomic<bool>	fOpened;
			std::atomic<bool>	fShowNext;		// even when paused: after a seek
			std::atomic<int32>	fGeneration;	// changes with a seek
			std::atomic<bool>	fVideoEnded;
			std::atomic<bool>	fAudioEnded;
			std::atomic<bool>	fClockFree;	// the sound is over: the clock
										// goes on by itself
			std::atomic<bool>	fEndSent;
			std::atomic<bool>	fFramePending;	// a kMsgPlayerFrame on its way
			float				fVolume;

			// statistics, every 5 seconds with CINEMA_PLAYER_DEBUG
			void				_Stats();
	static	void				_CpuTimes(bigtime_t& team, bigtime_t& system,
									int32& cpus);
									// the CPU time of Cinema and of all the
									// processors, and how many there are
			bool				fStatsOn = false;
			bigtime_t			fStatStart = 0;
			bigtime_t			fStatTeamCpu = 0;
			bigtime_t			fStatSystemCpu = 0;
			std::atomic<int32>	fStatDecoded{0};
			std::atomic<int32>	fStatShown{0};
			std::atomic<int32>	fStatLate{0};
			std::atomic<int32>	fStatMerged{0};
			std::atomic<int32>	fStatDrawn{0};
			bigtime_t			fStatDecodeTime = 0;
			bigtime_t			fStatCopyTime = 0;

			thread_id			fOpenThread;
			thread_id			fVideoThread;
			thread_id			fAudioThread;
};


#endif	// PLAYER_H
