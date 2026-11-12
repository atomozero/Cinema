/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Bitmap.h>
#include <Entry.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <OS.h>
#include <SoundPlayer.h>
#include <Url.h>

#include <algorithm>
#include <vector>

#include "Deliver.h"
#include "LiveAddress.h"
#include "Messages.h"


static const bigtime_t kLate = 120000;
	// a picture later than this is not shown
static const bigtime_t kRingTime = 2000000;
	// the sound decoded ahead
static const bigtime_t kStill = 250000;
	// a picture too late is shown all the same when none was for this
	// long: on a machine too slow for the video, it goes on by jerks
	// instead of standing still


static bool
no_overlay()
{
	const char* value = getenv("CINEMA_NO_OVERLAY");
	return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}


Player::Player(const BMessenger& target)
	:
	fTarget(target),
	fStartAt(0),
	fRelay(false),
	fRelayDuration(0),
	fFirstTime(-1),
	fLive(false),
	fNoOverlay(no_overlay()),
	fOverlayAllowed(true),
	fLiveFormat(NULL),
	fLiveVideo(NULL),
	fLiveAudio(NULL),
	fLiveHwOpaque(NULL),
	fLiveResampler(NULL),
	fLiveScaler(NULL),
	fLiveVideoIndex(-1),
	fLiveAudioIndex(-1),
	fLiveSkipLevel(0),
	fLiveLate(0),
	fLiveOnTime(0),
	fLivePixels(-1),
	fLiveBase(0),
	fLiveSoundEnd(0),
	fLiveStopRead(false),
	fFile(NULL),
	fVideoTrack(NULL),
	fAudioTrack(NULL),
	fVideoLock("player video"),
	fAudioLock("player audio"),
	fDuration(0),
	fFront(NULL),
	fBack(NULL),
	fOverlay(NULL),
	fFrameLock("player frame"),
	fHasFrame(false),
	fSoundPlayer(NULL),
	fFrameSize(0),
	fLatency(0),
	fLastFill(0),
	fFillTime(0),
	fRingRead(0),
	fRingFill(0),
	fRingLock("player ring"),
	fClockBase(0),
	fPlayed(0),
	fClockStart(0),
	fPausedClock(0),
	fPaused(false),
	fQuit(false),
	fSession(0),
	fOpened(false),
	fShowNext(false),
	fGeneration(0),
	fVideoEnded(false),
	fAudioEnded(false),
	fClockFree(false),
	fEndSent(false),
	fFramePending(false),
	fVolume(1.0f),
	fOpenThread(-1),
	fVideoThread(-1),
	fAudioThread(-1)
{
	fStatsOn = getenv("CINEMA_PLAYER_DEBUG") != NULL;
}


Player::~Player()
{
	Close();
	_DropPausedPicture();
}


/*!	A video of its own: the overlay is tried again, even when the last one
	had to go without it (full screen with the direct screen, or the screen
	refusing it), unless CINEMA_NO_OVERLAY turns it off for good.
*/
void
Player::Open(const char* address, bigtime_t startAt)
{
	fNoOverlay = no_overlay() || !fOverlayAllowed;
	_Start(address, startAt);
}


void
Player::_Start(const char* address, bigtime_t startAt, bool sameVideo)
{
	// the same video opened again (without the overlay): its subtitles, the
	// tracks the helper gave and their text, stay
	fSameVideo = sameVideo;
	_Close(sameVideo);
	// another video: its loudness is measured again; the same one opened
	// again at another place keeps it, without a jump
	if (!sameVideo)
		fLoudness.Reset();
	// its threads are gone: what they sent is of a session before
	fSession++;
	fStatStart = 0;
	fAddress = address;
	fStartAt = startAt;
	// the relay: how long it lasts is the last part of the address
	fLive = LiveAddress::Is(address);
	if (fLive)
		fStartAt = 0;
	fRelay = fAddress.StartsWith("http://127.0.0.1:");
	fRelayDuration = 0;
	fFirstTime = -1;
	if (fRelay) {
		int32 slash = fAddress.FindLast('/');
		fRelayDuration = (bigtime_t)atoll(fAddress.String() + slash + 1)
			* 1000000;
		if (fRelayDuration <= 0)
			fStartAt = 0;
	}
	fQuit = false;
	fPaused = false;
	fEndSent = false;
	// a picture announced to the last session counts for nothing now
	fFramePending = false;
	fOpenThread = spawn_thread(&_OpenEntry, "player open", B_NORMAL_PRIORITY,
		this);
	if (fOpenThread >= 0)
		resume_thread(fOpenThread);
}


void
Player::Close()
{
	_Close(false);
}


void
Player::_Close(bool keepSubtitles)
{
	fQuit = true;
	status_t status;
	if (fOpenThread >= 0) {
		wait_for_thread(fOpenThread, &status);
		fOpenThread = -1;
	}
	if (fSoundPlayer != NULL)
		fSoundPlayer->Stop();
	if (fVideoThread >= 0) {
		wait_for_thread(fVideoThread, &status);
		fVideoThread = -1;
	}
	if (fAudioThread >= 0) {
		wait_for_thread(fAudioThread, &status);
		fAudioThread = -1;
	}
	// after the threads that ask it the time
	delete fSoundPlayer;
	fSoundPlayer = NULL;
	_CloseLive();
	if (!keepSubtitles)
		fSubtitles.Clear();
	if (fFile != NULL) {
		if (fVideoTrack != NULL)
			fFile->ReleaseTrack(fVideoTrack);
		if (fAudioTrack != NULL)
			fFile->ReleaseTrack(fAudioTrack);
		delete fFile;
	}
	fFile = NULL;
	fVideoTrack = NULL;
	fAudioTrack = NULL;

	_DropPausedPicture();
	BAutolock locker(fFrameLock);
	_DropKeptBitmaps();
	if (keepSubtitles && fFFmpeg && fOverlay != NULL) {
		// the same video again (a seek): its overlay may be taken again, as
		// tegra_dc may not find the contiguous memory for a new one
		fKeptOverlay = fOverlay;
		fKeptFront = fFront;
		fKeptBack = fBack;
	} else {
		delete fFront;
		delete fBack;
		delete fOverlay;
	}
	fFront = NULL;
	fBack = NULL;
	fOverlay = NULL;
	fHasFrame = false;
	fOpened = false;
	fDuration = 0;
	fRing.clear();
	fRingRead = fRingFill = 0;
}


status_t
Player::_OpenEntry(void* data)
{
	((Player*)data)->_Open();
	return B_OK;
}


void
Player::_Open()
{
	// FFmpeg plays files and streams too (YouTube, the relay): it gives
	// YCbCr pictures for the overlay and times that can be trusted, where
	// the Media Kit's add-on gives RGB32 only. The Media Kit if FFmpeg
	// cannot open it, or with CINEMA_MEDIA_KIT=1.
	const char* mediaKit = getenv("CINEMA_MEDIA_KIT");
	bool tryFFmpeg = !fLive && (mediaKit == NULL || strcmp(mediaKit, "0") == 0)
		&& (fAddress.ByteAt(0) == '/' || fAddress.StartsWith("http://")
			|| fAddress.StartsWith("https://"));
	fFFmpeg = tryFFmpeg;
	if (fLive || tryFFmpeg) {
		if (_OpenLive() || fLive || fQuit)
			return;
		fFFmpeg = false;
		// what FFmpeg cannot get from the network the Media Kit, which uses
		// it too, cannot either, and it could not be interrupted
		if (fAddress.ByteAt(0) != '/') {
			_Failed();
			return;
		}
	}
	if (fAddress.ByteAt(0) == '/') {
		entry_ref ref;
		if (get_ref_for_path(fAddress.String(), &ref) == B_OK)
			fFile = new BMediaFile(&ref);
	} else if (fRelay) {
		// from where it starts, which the times of the stream count from
		BString address(fAddress);
		if (fStartAt > 0)
			address << "?t=" << (double)fStartAt / 1000000;
		fFile = new BMediaFile(BUrl(address.String(), true));
	} else
		fFile = new BMediaFile(BUrl(fAddress.String(), true));
	if (fFile == NULL || fFile->InitCheck() != B_OK) {
		_Failed();
		return;
	}
	if (fQuit)
		return;

	for (int32 i = 0; i < fFile->CountTracks(); i++) {
		BMediaTrack* track = fFile->TrackAt(i);
		if (track == NULL)
			continue;
		media_format encoded;
		track->EncodedFormat(&encoded);
		if (encoded.IsVideo() && fVideoTrack == NULL)
			fVideoTrack = track;
		else if (encoded.IsAudio() && fAudioTrack == NULL)
			fAudioTrack = track;
		else
			fFile->ReleaseTrack(track);
	}

	if (fVideoTrack != NULL && !_SetUpVideo()) {
		fFile->ReleaseTrack(fVideoTrack);
		fVideoTrack = NULL;
	}
	if (fVideoTrack != NULL && !fRelay && fStartAt <= 0 && fFormatsAsked > 1) {
		bigtime_t start = 0;
		fVideoTrack->SeekToTime(&start, B_MEDIA_SEEK_CLOSEST_BACKWARD);
	}
	fFrameDuration = 0;
	fFrameBase = fRelay ? fStartAt : 0;
	fFrameCount = 0;
	// the times the add-on gives for the first sound and pictures are not
	// even in one unit: a file opened from its start needs nothing, and
	// the relay starts the sound at the picture's key frame
	if (fVideoTrack != NULL) {
		media_format encoded;
		if (fVideoTrack->EncodedFormat(&encoded) == B_OK) {
			float rate = encoded.u.encoded_video.output.field_rate;
			if (rate >= 1 && rate <= 240)
				fFrameDuration = (bigtime_t)(1000000.0 / rate + 0.5);
		}
		if (fStatsOn) {
			fprintf(stderr, "player: %" B_PRIdBIGTIME " us a picture\n",
				fFrameDuration);
		}
	}

	// the sound, as 16 bit samples of the machine's order
	if (fAudioTrack != NULL) {
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		format.u.raw_audio = media_raw_audio_format::wildcard;
		format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
		format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
		if (fAudioTrack->DecodedFormat(&format) == B_OK
			&& format.u.raw_audio.channel_count > 0
			&& format.u.raw_audio.frame_rate > 0) {
			fSound = format.u.raw_audio;
			fFrameSize = (fSound.format & 0x0f) * fSound.channel_count;
			fRing.resize((size_t)(fSound.frame_rate * kRingTime / 1000000)
				* fFrameSize);
			fRingRead = fRingFill = 0;
			fPerformanceStart = 0;
			{
				BAutolock locker(fFillLock);
				fFillCount = fFillNext = 0;
				fSentFrames = 0;
			}
			fSoundPlayer = new BSoundPlayer(&fSound, "Cinema", &_PlayBuffer,
				NULL, this);
			if (fSoundPlayer->InitCheck() != B_OK) {
				delete fSoundPlayer;
				fSoundPlayer = NULL;
			} else {
				_ApplyVolume();
				fLatency = fSoundPlayer->Latency();
			}
			fDuration = std::max(fDuration, fAudioTrack->Duration());
		}
		if (fSoundPlayer == NULL) {
			fFile->ReleaseTrack(fAudioTrack);
			fAudioTrack = NULL;
		}
	}

	if (fVideoTrack == NULL && fAudioTrack == NULL) {
		_Failed();
		return;
	}

	fVideoEnded = fVideoTrack == NULL;
	fAudioEnded = fAudioTrack == NULL;
	fClockFree = false;
	fClockBase = 0;
	fPlayed = 0;
	fClockStart = system_time();
	if (fRelay) {
		fDuration = fRelayDuration;
		fClockBase = fStartAt;
		fPausedClock = fStartAt;
	} else if (fStartAt > 0)
		_SeekTracks(fStartAt);
	fShowNext = true;
	fOpened = true;

	if (fAddress.ByteAt(0) == '/' && !fSameVideo) {
		fSubtitles.LoadSidecars(fAddress.String());
		fSubtitles.StartEmbedded(fAddress.String());
	}

	BMessage opened(kMsgPlayerOpened);
	opened.AddInt64("duration", fDuration);
	opened.AddBool("overlay", fOverlay != NULL);
	if (fFront != NULL) {
		opened.AddInt32("width", fFront->Bounds().IntegerWidth() + 1);
		opened.AddInt32("height", fFront->Bounds().IntegerHeight() + 1);
	}
	_Tell(&opened);

	if (fQuit)
		return;
	if (fVideoTrack != NULL) {
		fVideoThread = spawn_thread(&_VideoEntry, "player pictures",
			B_DISPLAY_PRIORITY, this);
		resume_thread(fVideoThread);
	}
	if (fAudioTrack != NULL) {
		fAudioThread = spawn_thread(&_AudioEntry, "player sound",
			B_URGENT_DISPLAY_PRIORITY, this);
		resume_thread(fAudioThread);
		// as BSoundPlayer::Start() starts its node
		bigtime_t performance = fSoundPlayer->PerformanceTime();
		bigtime_t latency = fSoundPlayer->Latency();
		fSoundPlayer->Start();
		if (performance > 0)
			fPerformanceStart = performance + latency + 5000;
		fSoundPlayer->SetHasData(true);
	}
}


/*!	A message to the window, with the session it belongs to. */
status_t
Player::_Tell(BMessage* message)
{
	message->AddInt32("session", fSession);
	return Deliver(fTarget, message, &fQuit);
}


status_t
Player::_Tell(uint32 what)
{
	BMessage message(what);
	return _Tell(&message);
}


void
Player::_Failed()
{
	// closing: the window waits for us, and does not want to know
	if (!fQuit)
		_Tell(kMsgPlayerError);
}


bigtime_t
Player::_Clock() const
{
	if (fClockFree)
		return fPaused ? fPausedClock : fClockBase + system_time() - fClockStart;
	if (fSoundPlayer != NULL && fPerformanceStart > 0) {
		// the frame heard now, and the sound of the video it carries
		double rate = std::max(1.0f, fSound.frame_rate);
		bigtime_t now = fSoundPlayer->PerformanceTime();
		int64 heard = (int64)((now - fPerformanceStart) * rate / 1000000.0);
		int64 content = 0;
		{
			BAutolock locker(fFillLock);
			for (int32 i = 1; i <= fFillCount; i++) {
				const SoundFill& fill = fFills[(fFillNext - i + 64) % 64];
				if (fill.sent > heard)
					continue;
				content = fill.content
					+ std::min(heard - fill.sent, fill.frames);
				break;
			}
		}
		return fClockBase + (bigtime_t)(content * 1000000.0 / rate);
	}
	if (fSoundPlayer != NULL) {
		bigtime_t played = fPlayed * 1000000LL
			/ (int64)std::max(1.0f, fSound.frame_rate);
		// between two buffers too, so that the pictures go on evenly
		if (!fPaused && fLastFill > 0)
			played += std::min(system_time() - fLastFill, fFillTime);
		return fClockBase + std::max((bigtime_t)0, played - fLatency);
	}
	if (fPaused)
		return fPausedClock;
	return fClockBase + system_time() - fClockStart;
}


bigtime_t
Player::Position() const
{
	if (!fOpened)
		return fStartAt;
	bigtime_t position = _Clock();
	if (fDuration > 0)
		position = std::min(position, fDuration);
	return std::max((bigtime_t)0, position);
}


void
Player::SetPaused(bool paused)
{
	if (paused == fPaused)
		return;
	if (fSoundPlayer == NULL || fClockFree) {
		if (paused)
			fPausedClock = _Clock();
		else {
			fClockBase = fPausedClock;
			fClockStart = system_time();
		}
	}
	fPaused = paused;
	if (!paused)
		_DropPausedPicture();
}


/*!	The overlay allowed or not, from the settings or the O key: on the video
	playing at once, from where it is.
*/
void
Player::SetOverlayAllowed(bool allowed)
{
	if (fOverlayAllowed.exchange(allowed) == allowed)
		return;
	if (!allowed) {
		DisableOverlay();
		return;
	}
	if (no_overlay() || !fNoOverlay.exchange(false) || !fOpened
		|| fAddress.IsEmpty())
		return;
	BString address(fAddress);
	_Start(address.String(), Position(), true);
}


void
Player::DisableOverlay()
{
	if (fNoOverlay.exchange(true) || !fOpened || fAddress.IsEmpty())
		return;
	// for this video only: Open() tries the overlay again for the next one
	BString address(fAddress);
	_Start(address.String(), Position(), true);
}


void
Player::SetVolume(float volume)
{
	// in tenths, so that the steps of the keys stay even
	volume = roundf(std::max(0.0f, std::min(1.0f, volume)) * 10) / 10;
	fVolume = volume;
	_ApplyVolume();
}


/*!	The volume is a level the ear hears in even steps: 3 dB a tenth, from
	0 dB at 1 down to -27 dB at 0.1, and silence at 0. As a plain gain, the
	steps of 10% of the arrow keys could hardly be heard.
*/
void
Player::SetNormalized(bool normalized)
{
	// CINEMA_EVEN_VOLUME=0 or 1 overrides the setting, to compare the two
	const char* forced = getenv("CINEMA_EVEN_VOLUME");
	if (forced != NULL && forced[0] != '\0')
		normalized = strcmp(forced, "0") != 0;
	fLoudness.SetEnabled(normalized);
}


void
Player::_ApplyVolume()
{
	if (fSoundPlayer == NULL)
		return;
	// 40 log10: -4 dB at 80%, -12 at 50%, -28 at 20%, as the ear hears it;
	// 3 dB each tenth made 80% sound much softer than other players
	if (fVolume < 0.05f)
		fSoundPlayer->SetVolume(0);
	else
		fSoundPlayer->SetVolumeDB(40.0f * log10f(fVolume));
}


int32
Player::SubtitleCount() const
{
	return fSubtitles.Count();
}


bool
Player::SubtitleTrack(int32 index, SubtitleTrackInfo* info) const
{
	return fSubtitles.TrackAt(index, info);
}


int32
Player::SelectedSubtitle() const
{
	return fSubtitles.Selected();
}


void
Player::SelectSubtitle(int32 index)
{
	fSubtitles.Select(index);
}


void
Player::ToggleSubtitles()
{
	fSubtitles.Toggle();
}


BString
Player::SubtitleText(bigtime_t time) const
{
	return fSubtitles.TextAt(time);
}


int32
Player::AddSubtitleTrack(const char* language, const char* name,
	bool automatic)
{
	return fSubtitles.AddTrack(language, name, automatic, false);
}


void
Player::LoadSubtitleText(const char* language, bool automatic,
	const char* text)
{
	fSubtitles.Load(language, automatic, text);
}


/*!	Both tracks to time, or rather to the picture that starts there or
	before, so that the picture is whole and the sound goes with it.
*/
void
Player::_SeekTracks(bigtime_t time)
{
	BAutolock video(fVideoLock);
	BAutolock audio(fAudioLock);
	fGeneration++;
	if (fDuration > 0)
		time = std::min(time, fDuration - 1000000);
	time = std::max((bigtime_t)0, time);
	bigtime_t wanted = time;
	if (fVideoTrack != NULL)
		fVideoTrack->SeekToTime(&time, B_MEDIA_SEEK_CLOSEST_BACKWARD);
	bigtime_t soundTime = time;
	if (fAudioTrack != NULL)
		fAudioTrack->SeekToTime(&soundTime);
	if (fStatsOn) {
		fprintf(stderr, "player: seek wanted %.3f video %.3f audio %.3f\n",
			wanted / 1e6, time / 1e6, soundTime / 1e6);
	}
	fFrameBase = time;
	fFrameCount = 0;
	{
		BAutolock ring(fRingLock);
		fRingRead = fRingFill = 0;
	}
	fClockBase = time;
	fPlayed = 0;
	fLastFill = 0;
	{
		// the buffers queued still have the sound of before: until the
		// first one of now is heard, the clock stays at the new base
		BAutolock locker(fFillLock);
		fFillCount = 0;
	}
	fClockStart = system_time();
	fPausedClock = time;
	fVideoEnded = fVideoTrack == NULL;
	fAudioEnded = fAudioTrack == NULL;
	fClockFree = false;
	fEndSent = false;
	fShowNext = true;
}


void
Player::Seek(bigtime_t time)
{
	if (!fOpened || fLive)
		return;
	if (fRelay || fFFmpeg) {
		if (fDuration <= 0)
			return;
		// opened again from there, by the window: the bitmap of the
		// overlay goes with it
		time = std::max((bigtime_t)0, std::min(time, fDuration - 1000000));
		BMessage reopen(kMsgPlayerReopen);
		reopen.AddInt64("time", time);
		reopen.AddInt32("session", fSession);
		// from the window to itself: without waiting, it would wait for
		// itself
		fTarget.SendMessage(&reopen, (BHandler*)NULL, 0);
		return;
	}
	_SeekTracks(time);
}


/*!	The bitmaps of the overlay kept by _Close(), when they are not taken
	again. With fFrameLock.
*/
void
Player::_DropKeptBitmaps()
{
	delete fKeptFront;
	delete fKeptBack;
	delete fKeptOverlay;
	fKeptFront = fKeptBack = fKeptOverlay = NULL;
}


void
Player::Reopen(bigtime_t time)
{
	if (fAddress.IsEmpty())
		return;
	BString address(fAddress);
	_Start(address.String(), std::max((bigtime_t)0, time), true);
}


/*!	The time of a picture or a sound in the video: for a stream of the
	relay, which counts from where it starts as it likes, from fStartAt.
*/
bigtime_t
Player::_StreamTime(bigtime_t time)
{
	if (!fRelay)
		return time;
	bigtime_t first = -1;
	fFirstTime.compare_exchange_strong(first, time);
	return time - fFirstTime + fStartAt;
}


status_t
Player::_AudioEntry(void* data)
{
	((Player*)data)->_AudioLoop();
	return B_OK;
}


void
Player::_AudioLoop()
{

	size_t size = std::max((size_t)fSound.buffer_size, (size_t)65536);
	uint8* buffer = (uint8*)malloc(size * 2);
	if (buffer == NULL)
		return;

	while (!fQuit) {
		size_t room;
		{
			BAutolock ring(fRingLock);
			room = fRing.size() - fRingFill;
		}
		if (fAudioEnded || room < size) {
			snooze(10000);
			continue;
		}

		int32 generation;
		int64 frames = 0;
		media_header header;
		status_t status;
		{
			BAutolock locker(fAudioLock);
			generation = fGeneration;
			status = fAudioTrack->ReadFrames(buffer, &frames, &header);
		}
		if (status != B_OK) {
			// the end, or nothing more to read
			fAudioEnded = true;
			_CheckEnded();
			continue;
		}
		if (generation != fGeneration)
			continue;
		_StreamTime(header.start_time);
		// evened and limited, as the sound of FFmpeg
		if (fSound.format == media_raw_audio_format::B_AUDIO_SHORT) {
			fLoudness.Process((int16*)buffer, (int32)frames,
				(int32)fSound.channel_count, fSound.frame_rate);
		}

		BAutolock ring(fRingLock);
		size_t bytes = std::min((size_t)frames * fFrameSize,
			fRing.size() - fRingFill);
		size_t write = (fRingRead + fRingFill) % fRing.size();
		size_t first = std::min(bytes, fRing.size() - write);
		memcpy(&fRing[write], buffer, first);
		memcpy(&fRing[0], buffer + first, bytes - first);
		fRingFill += bytes;
	}
	free(buffer);
}


void
Player::_PlayBuffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	((Player*)cookie)->_FillSound(buffer, size);
}


void
Player::_FillSound(void* buffer, size_t size)
{
	uint8* out = (uint8*)buffer;
	if (fPaused || fRing.empty()) {
		memset(out, 0, size);
		_NoteFill(size, 0);
		return;
	}
	BAutolock ring(fRingLock);
	size_t bytes = std::min(size, fRingFill);
	bytes -= bytes % std::max((size_t)1, fFrameSize);
	size_t first = std::min(bytes, fRing.size() - fRingRead);
	memcpy(out, &fRing[fRingRead], first);
	memcpy(out + first, &fRing[0], bytes - first);
	fRingRead = (fRingRead + bytes) % fRing.size();
	fRingFill -= bytes;
	if (bytes < size) {
		memset(out + bytes, 0, size - bytes);
		// the sound ran out: counted, while there is more to come
		if (!fAudioEnded)
			fSoundUnderruns++;
	}
	_NoteFill(size, bytes);
	fPlayed += bytes / std::max((size_t)1, fFrameSize);
	fFillTime = (bigtime_t)(size / std::max((size_t)1, fFrameSize)) * 1000000LL
		/ (int64)std::max(1.0f, fSound.frame_rate);
	fLastFill = system_time();
	if (fRingFill == 0 && fAudioEnded) {
		_FreeClock();
		_CheckEnded();
	}
}


/*!	The sound is over, before the pictures maybe: the clock goes on from
	where the sound left it, or the last pictures would wait for it.
*/
void
Player::_FreeClock()
{
	if (fClockFree)
		return;
	bigtime_t now = _Clock();
	fClockStart = system_time();
	fClockBase = now;
	fPausedClock = now;
	fClockFree = true;
}


/*!	A buffer given to the sound player: requested bytes in all, content
	bytes of the video's sound at its start, silence after.
*/
void
Player::_NoteFill(size_t requested, size_t content)
{
	size_t frameSize = std::max((size_t)1, fFrameSize);
	BAutolock locker(fFillLock);
	SoundFill& fill = fFills[fFillNext];
	fill.sent = fSentFrames;
	fill.content = fPlayed;
	fill.frames = content / frameSize;
	fFillNext = (fFillNext + 1) % 64;
	fFillCount = std::min(fFillCount + 1, (int32)64);
	fSentFrames += requested / frameSize;
}


status_t
Player::_VideoEntry(void* data)
{
	((Player*)data)->_VideoLoop();
	return B_OK;
}


void
Player::_VideoLoop()
{
	bigtime_t lastShown = system_time();
	while (!fQuit) {
		if (fVideoEnded || (fPaused && !fShowNext)) {
			snooze(10000);
			continue;
		}

		int32 generation;
		int64 frames = 1;
		media_header header;
		status_t status;
		bigtime_t frameTime = -1;
		{
			BAutolock locker(fVideoLock);
			generation = fGeneration;
			bigtime_t start = system_time();
			status = fVideoTrack->ReadFrames(fBack->Bits(), &frames, &header);
			fStatDecodeTime += system_time() - start;
			if (status == B_OK && fFrameDuration > 0)
				frameTime = fFrameBase + fFrameCount++ * fFrameDuration;
		}
		fStatDecoded++;
		if (status != B_OK) {
			fVideoEnded = true;
			_CheckEnded();
			continue;
		}
		if (generation != fGeneration)
			continue;

		bigtime_t time = frameTime >= 0 ? frameTime
			: _StreamTime(header.start_time);
		fStatFrameTime = time;

		bool show = fShowNext;
		if (!show) {
			// waits for the sound to get there
			while (!fQuit && generation == fGeneration && !fShowNext) {
				if (fPaused) {
					snooze(10000);
					continue;
				}
				bigtime_t wait = time - _Clock();
				if (wait <= 0)
					break;
				bigtime_t sleep = std::min(wait, (bigtime_t)50000);
				snooze_until(system_time() + sleep, B_SYSTEM_TIMEBASE);
			}
			if (fQuit || generation != fGeneration)
				continue;
			show = fShowNext || _Clock() - time < kLate
				|| system_time() - lastShown > kStill;
		}
		if (!show) {
			fStatLate++;
			_Stats();
			continue;
		}

		lastShown = system_time();
		_ShowFrame();
		fStatCopyTime += system_time() - lastShown;
		fStatShown++;
		fShowNext = false;
		if (!fFramePending.exchange(true)) {
			if (Deliver(fTarget, kMsgPlayerFrame, &fQuit) != B_OK)
				fFramePending = false;
		} else
			fStatMerged++;
		_Stats();
	}
}


void
Player::NoteDrawn()
{
	fStatDrawn++;
}


/*static*/ void
Player::_CpuTimes(bigtime_t& team, bigtime_t& system, int32& cpus)
{
	team_usage_info usage;
	team = 0;
	if (get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &usage)
			== B_OK)
		team = usage.user_time + usage.kernel_time;
	system_info info;
	get_system_info(&info);
	cpus = std::max((uint32)1, info.cpu_count);
	std::vector<cpu_info> perCpu(cpus);
	system = 0;
	if (get_cpu_info(0, cpus, perCpu.data()) == B_OK) {
		for (const cpu_info& cpu : perCpu)
			system += cpu.active_time;
	}
}


/*!	What the video loop did in the last 5 seconds: pictures decoded, shown,
	thrown away as too late, shown but never drawn because the one before
	was still waiting (merged), drawn by the view (RGB only); the time to
	decode one and to copy it to the overlay; how busy Cinema and the whole
	system (app_server included, which scales the RGB pictures) were.
*/
void
Player::_Stats()
{
	if (!fStatsOn)
		return;
	bigtime_t now = system_time();
	if (fStatStart != 0 && now - fStatStart < 5000000)
		return;
	bigtime_t team, system;
	int32 cpus;
	_CpuTimes(team, system, cpus);
	if (fStatStart != 0) {
		double seconds = (now - fStatStart) / 1e6;
		int32 decoded = fStatDecoded, shown = fStatShown;
		double capacity = (now - fStatStart) * (double)cpus;
		fprintf(stderr, "player: stats %s %.1f s decoded %" B_PRId32
			" shown %" B_PRId32 " late %" B_PRId32 " merged %" B_PRId32
			" drawn %" B_PRId32 " fps %.1f decode %.1f ms copy %.2f ms"
			" cinema %.0f%% system %.0f%% cpus %" B_PRId32
			" | picture %.3f clock %.3f\n",
			fOverlay != NULL ? "overlay" : "rgb", seconds, decoded, shown,
			(int32)fStatLate, (int32)fStatMerged, (int32)fStatDrawn,
			shown / seconds,
			decoded > 0 ? fStatDecodeTime / 1000.0 / decoded : 0.0,
			shown > 0 ? fStatCopyTime / 1000.0 / shown : 0.0,
			100.0 * (team - fStatTeamCpu) / capacity,
			100.0 * (system - fStatSystemCpu) / capacity, cpus,
			fStatFrameTime / 1e6, _Clock() / 1e6);
	}
	fStatStart = now;
	fStatTeamCpu = team;
	fStatSystemCpu = system;
	fStatDecoded = fStatShown = fStatLate = fStatMerged = fStatDrawn = 0;
	fStatDecodeTime = fStatCopyTime = 0;
}


void
Player::_CheckEnded()
{
	if (!fVideoEnded || !fAudioEnded || fEndSent)
		return;
	{
		BAutolock ring(fRingLock);
		if (fRingFill > 0)
			return;
	}
	if (!fEndSent.exchange(true))
		_Tell(kMsgPlayerEnded);
}


BBitmap*
Player::LockFrame()
{
	fFrameLock.Lock();
	fFramePending = false;
	return fHasFrame ? fFront : NULL;
}


void
Player::UnlockFrame()
{
	fFrameLock.Unlock();
}


/*!	The pictures in YCbCr422 for the overlay, when it is free: the screen
	scales them and turns them into colors by itself, which the AC100
	could not do on time. Otherwise in 32 bits RGB, which the view draws.
*/
bool
Player::_SetUpVideo()
{
	{
		BAutolock locker(fFrameLock);
		_DropKeptBitmaps();
	}
	// each format the decoder is asked for may cost it pictures: it gives
	// RGB32 only, for Haiku's ffmpeg add-on, and the two tries took the
	// first 8 pictures of a video, 0.32 s ahead of the sound then. A file
	// is sought back to its start after; the relay's stream cannot be.
	fFormatsAsked = 0;
	const color_space spaces[] = { B_YCbCr420, B_YCbCr422 };
	for (color_space space : spaces) {
		if (fNoOverlay || fRelay)
			break;
		fFormatsAsked++;
		media_format format;
		format.type = B_MEDIA_RAW_VIDEO;
		format.u.raw_video = media_raw_video_format::wildcard;
		format.u.raw_video.display.format = space;
		status_t decoded = fVideoTrack->DecodedFormat(&format);
		if (fStatsOn) {
			fprintf(stderr, "player: decoder asked %#x: %s, gives %#x\n",
				space, strerror(decoded), format.u.raw_video.display.format);
		}
		if (decoded != B_OK || format.u.raw_video.display.format != space)
			continue;
		const media_video_display_info& display = format.u.raw_video.display;
		BRect bounds(0, 0, display.line_width - 1, display.line_count - 1);
		BBitmap* overlay = new BBitmap(bounds, B_BITMAP_WILL_OVERLAY
			| B_BITMAP_RESERVE_OVERLAY_CHANNEL, space);
		if (fStatsOn) {
			fprintf(stderr, "player: overlay %#x %dx%d: %s\n", space,
				(int)display.line_width, (int)display.line_count,
				strerror(overlay->InitCheck()));
		}
		if (overlay->InitCheck() != B_OK) {
			delete overlay;
			continue;
		}
		BBitmap* front = new BBitmap(bounds, B_BITMAP_NO_SERVER_LINK, space,
			display.bytes_per_row);
		BBitmap* back = new BBitmap(bounds, B_BITMAP_NO_SERVER_LINK, space,
			display.bytes_per_row);
		if (front->InitCheck() != B_OK || back->InitCheck() != B_OK) {
			delete front;
			delete back;
			delete overlay;
			continue;
		}
		BAutolock locker(fFrameLock);
		fOverlay = overlay;
		fFront = front;
		fBack = back;
		fDuration = std::max(fDuration, fVideoTrack->Duration());
		if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
			fprintf(stderr, "player: %dx%d %s overlay\n",
				(int)display.line_width, (int)display.line_count,
				space == B_YCbCr420 ? "420" : "422");
		}
		return true;
	}

	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;
	format.u.raw_video.display.format = B_RGB32;
	fFormatsAsked++;
	if (fVideoTrack->DecodedFormat(&format) != B_OK
		|| (format.u.raw_video.display.format != B_RGB32
			&& format.u.raw_video.display.format != B_RGBA32))
		return false;
	const media_video_display_info& display = format.u.raw_video.display;
	BRect bounds(0, 0, display.line_width - 1, display.line_count - 1);
	BBitmap* front = new BBitmap(bounds, 0, B_RGB32,
		display.bytes_per_row);
	BBitmap* back = new BBitmap(bounds, 0, B_RGB32, display.bytes_per_row);
	if (front->InitCheck() != B_OK || back->InitCheck() != B_OK) {
		delete front;
		delete back;
		return false;
	}
	BAutolock locker(fFrameLock);
	fFront = front;
	fBack = back;
	fDuration = std::max(fDuration, fVideoTrack->Duration());
	if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
		fprintf(stderr, "player: %dx%d rgb\n", (int)display.line_width,
			(int)display.line_count);
	}
	return true;
}


/*!	The picture decoded becomes the one shown; with the overlay, it is
	copied where the screen takes it from.
*/
void
Player::_ShowFrame()
{
	BAutolock locker(fFrameLock);
	std::swap(fFront, fBack);
	fHasFrame = true;
	fShownCount++;
	if (fOverlay == NULL || fOverlay->LockBits() != B_OK)
		return;
	const uint8* from = (const uint8*)fFront->Bits();
	uint8* to = (uint8*)fOverlay->Bits();
	int32 fromRow = fFront->BytesPerRow();
	int32 toRow = fOverlay->BytesPerRow();
	int32 row = std::min(fromRow, toRow);
	int32 rows = fFront->Bounds().IntegerHeight() + 1;
	if (fromRow == toRow)
		memcpy(to, from, (size_t)row * rows);
	else {
		for (int32 y = 0; y < rows; y++)
			memcpy(to + y * toRow, from + y * fromRow, row);
	}
	fOverlay->UnlockBits();
}
