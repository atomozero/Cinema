/*
 * Copyright 2026, Andrea Bernardi
 * Copyright 2026, Lee JunHaeng
 * SPDX-License-Identifier: MIT
 *
 * Live streams played with FFmpeg rather than the Media Kit, after
 * FFmpegMediaPlayer.cpp of R Television (https://github.com/rainygirl/
 * rtelevision, MIT license): the choice of the picture of an HLS stream
 * a slow processor keeps up with, the streams not played left alone, the
 * loop filter of H.264 skipped on such a processor and the frames nothing
 * refers to while it is behind, the pictures decoded only near their time,
 * the holes and the jumps of the sound.
 */


#include "Player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Bitmap.h>
#include <OS.h>
#include <SoundPlayer.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>

#include "Deliver.h"
#include "LiveAddress.h"
#include "Messages.h"
#include "VideoLimits.h"


#if defined(__arm__) && !defined(__aarch64__)
	// a Cortex-A9 without NEON, as the Toshiba AC100's, decodes H.264 in C
static const bool kSlowProcessor = true;
#else
static const bool kSlowProcessor = false;
#endif

static const bigtime_t kSoundAhead = 4000000;
	// the sound decoded ahead: segments of HLS come in bursts
static const int64 kDecodeAhead = 500000;
	// pictures decoded this much before their time, for the reordering
static const int64 kMaxQueuedVideo = 8000000;
	// then decoded all the same
static const size_t kMaxDecodedFrames = 12;
static const int64 kLateMicros = 60000;
	// a picture this late counts as late
static const size_t kMaxReadPackets = 1500;
	// read ahead of the playing, about fifteen seconds
static const int64 kSettling = 3000000;
	// the first pictures are often late, while everything starts


int
Player::_LiveInterrupt(void* data)
{
	Player* player = (Player*)data;
	return player->fQuit || player->fLiveStopRead ? 1 : 0;
}


static enum AVPixelFormat
get_hw_format(AVCodecContext* context, const enum AVPixelFormat* formats)
{
	const enum AVPixelFormat wanted =
		*(const enum AVPixelFormat*)context->opaque;
	for (const enum AVPixelFormat* format = formats;
		format != NULL && *format != AV_PIX_FMT_NONE; format++) {
		if (*format == wanted)
			return *format;
	}
	// the hardware does not take this stream (its codec or its profile):
	// decoded in software then, rather than not at all
	for (const enum AVPixelFormat* format = formats;
		format != NULL && *format != AV_PIX_FMT_NONE; format++) {
		const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(*format);
		if (description != NULL
			&& (description->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
			if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
				fprintf(stderr, "live: hardware refused, software %s\n",
					description->name);
			return *format;
		}
	}
	return AV_PIX_FMT_NONE;
}


/*!	The hardware the decoder may use, if FFmpeg was built with one and
	the machine has it. CINEMA_HWACCEL=0 turns it off, a name such as
	vaapi or vdpau asks for that one only.
*/
static bool
try_hardware(AVCodecContext* decoder, const AVCodec* codec, void** opaque)
{
	const char* requested = getenv("CINEMA_HWACCEL");
	if (requested != NULL && strcmp(requested, "0") == 0)
		return false;
	enum AVHWDeviceType only = AV_HWDEVICE_TYPE_NONE;
	if (requested != NULL && requested[0] != '\0'
		&& strcmp(requested, "1") != 0)
		only = av_hwdevice_find_type_by_name(requested);

	enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
	while ((type = av_hwdevice_iterate_types(type))
		!= AV_HWDEVICE_TYPE_NONE) {
		if (only != AV_HWDEVICE_TYPE_NONE && type != only)
			continue;
		for (int i = 0; ; i++) {
			const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
			if (config == NULL)
				break;
			if (config->device_type != type
				|| (config->methods
					& AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0)
				continue;
			AVBufferRef* device = NULL;
			if (av_hwdevice_ctx_create(&device, type, NULL, NULL, 0)
				< 0) {
				av_buffer_unref(&device);
				continue;
			}
			AVPixelFormat* wanted = (AVPixelFormat*)av_malloc(
				sizeof(*wanted));
			if (wanted == NULL) {
				av_buffer_unref(&device);
				return false;
			}
			*wanted = config->pix_fmt;
			decoder->hw_device_ctx = device;
			decoder->opaque = wanted;
			decoder->get_format = get_hw_format;
			*opaque = wanted;
			if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
				fprintf(stderr, "live: hardware %s\n",
					av_hwdevice_get_type_name(type));
			}
			return true;
		}
	}
	return false;
}


static AVCodecContext*
make_decoder(AVCodecParameters* parameters, const AVCodec* codec, bool video)
{
	AVCodecContext* decoder = avcodec_alloc_context3(codec);
	if (decoder == NULL)
		return NULL;
	avcodec_parameters_to_context(decoder, parameters);
	decoder->thread_count = 0;
	if (video) {
		decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		if (kSlowProcessor)
			decoder->flags2 |= AV_CODEC_FLAG2_FAST;
	}
	return decoder;
}


static AVCodecContext*
open_decoder(AVFormatContext* format, int index, bool video, void** opaque)
{
	if (index < 0)
		return NULL;
	AVCodecParameters* parameters = format->streams[index]->codecpar;
	const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
	if (codec == NULL)
		return NULL;
	AVCodecContext* decoder = make_decoder(parameters, codec, video);
	if (decoder == NULL)
		return NULL;
	if (video && opaque != NULL && try_hardware(decoder, codec, opaque)) {
		if (avcodec_open2(decoder, codec, NULL) >= 0)
			return decoder;
		avcodec_free_context(&decoder);
		av_free(*opaque);
		*opaque = NULL;
		decoder = make_decoder(parameters, codec, video);
		if (decoder == NULL)
			return NULL;
	}
	if (avcodec_open2(decoder, codec, NULL) < 0) {
		avcodec_free_context(&decoder);
		return NULL;
	}
	if (video && getenv("CINEMA_PLAYER_DEBUG") != NULL && *opaque == NULL)
		fprintf(stderr, "live: hardware none\n");
	return decoder;
}


/*!	The bitmaps the pictures go to: YCbCr422 for the overlay when it is
	free, which scales them by itself, or 32 bits RGB the view draws.

	The overlay of the AC100 takes memory all in one piece, which after a
	while is no longer there for a big picture: then a smaller one, the
	size of the screen or half the picture, that swscale fills.
*/
bool
Player::_SetUpBitmaps(int32 width, int32 height)
{
	if (width <= 0 || height <= 0)
		return false;

	float scale = 1.0f;
	int32 limit = VideoHeightLimit();
	if (height > limit)
		scale = (float)limit / height;
	BScreen screen(B_MAIN_SCREEN_ID);
	if (screen.IsValid()) {
		BRect frame = screen.Frame();
		float screenScale = (frame.Width() + 1) / width;
		if (screenScale < scale)
			scale = screenScale;
		screenScale = (frame.Height() + 1) / height;
		if (screenScale < scale)
			scale = screenScale;
	}
	if (scale < 1.0f) {
		width = std::max((int32)2,
			((int32)(width * scale + 0.5f)) & ~1);
		height = std::max((int32)2, (int32)(height * scale + 0.5f));
	}

	BAutolock locker(fFrameLock);

	if (fNoOverlay)
		_DropKeptBitmaps();

	if (!fNoOverlay) {
		int32 sizes[3][2] = { { width, height } };
		int32 count = 1;
		if (width > 1024 || height > 600) {
			float scale = std::min(1024.0f / width, 600.0f / height);
			sizes[count][0] = (int32)(width * scale) & ~1;
			sizes[count][1] = (int32)(height * scale);
			count++;
		}
		if (width >= 640) {
			sizes[count][0] = (width / 2) & ~1;
			sizes[count][1] = height / 2;
			count++;
		}
		// only YCbCr422: FFmpeg's YUYV422 is the same packed layout, while
		// Haiku's B_YCbCr420 is not FFmpeg's planar YUV420P
		for (int32 i = 0; i < count; i++) {
			int32 width = sizes[i][0] & ~1;
			int32 height = sizes[i][1];
			if (width < 2 || height < 2)
				continue;
			// the overlay of the same video before a seek, rather than
			// freeing it and finding the contiguous memory for another
			if (fKeptOverlay != NULL
				&& fKeptOverlay->Bounds().IntegerWidth() + 1 == width
				&& fKeptOverlay->Bounds().IntegerHeight() + 1 == height) {
				fOverlay = fKeptOverlay;
				fFront = fKeptFront;
				fBack = fKeptBack;
				fKeptOverlay = fKeptFront = fKeptBack = NULL;
				fLivePixels = AV_PIX_FMT_YUYV422;
				if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
					fprintf(stderr, "live: %dx%d 422 overlay kept\n",
						(int)width, (int)height);
				}
				return true;
			}
			BRect bounds(0, 0, width - 1, height - 1);
			BBitmap* overlay = new BBitmap(bounds, B_BITMAP_WILL_OVERLAY
				| B_BITMAP_RESERVE_OVERLAY_CHANNEL, B_YCbCr422);
			if (overlay->InitCheck() != B_OK) {
				delete overlay;
				continue;
			}
			BBitmap* front = new BBitmap(bounds, B_BITMAP_NO_SERVER_LINK,
				B_YCbCr422);
			BBitmap* back = new BBitmap(bounds, B_BITMAP_NO_SERVER_LINK,
				B_YCbCr422);
			if (front->InitCheck() != B_OK || back->InitCheck() != B_OK) {
				delete front;
				delete back;
				delete overlay;
				continue;
			}
			fOverlay = overlay;
			fFront = front;
			fBack = back;
			fLivePixels = AV_PIX_FMT_YUYV422;
			// a bigger one was found: the kept one goes
			_DropKeptBitmaps();
			if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
				fprintf(stderr, "live: %dx%d 422 overlay\n", (int)width,
					(int)height);
			return true;
		}
	}

	_DropKeptBitmaps();
	BRect bounds(0, 0, width - 1, height - 1);
	fFront = new BBitmap(bounds, 0, B_RGB32);
	fBack = new BBitmap(bounds, 0, B_RGB32);
	fLivePixels = AV_PIX_FMT_BGRA;
	if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
		fprintf(stderr, "live: %dx%d bgra\n", (int)width, (int)height);
	return fFront->InitCheck() == B_OK && fBack->InitCheck() == B_OK;
}


bool
Player::_OpenLive()
{
	// live TV from its address, or a file, a stream of YouTube or the relay.
	// False when the Media Kit may try it instead.
	LiveAddress live;
	if (fLive) {
		if (!LiveAddress::Parse(fAddress.String(), live)) {
			_Failed();
			return false;
		}
	} else {
		live.url = fAddress;
		if (fRelay && fStartAt > 0)
			live.url << "?t=" << (double)fStartAt / 1000000;
	}
	// set when the last stream stopped: it would stop this one opening
	fLiveStopRead = false;
	static bool sNetwork = false;
	if (!sNetwork) {
		avformat_network_init();
		av_log_set_level(AV_LOG_ERROR);
		sNetwork = true;
	}

	// twice: opening reads a segment of every size of the stream, and one
	// of the many connections may fail for a moment
	AVFormatContext* format = NULL;
	// the server said no: an address that expired, or was given to another
	// network, which the Media Kit would not open either
	bool refused = false;
	for (int attempt = 0; attempt < 2 && format == NULL && !fQuit
			&& !refused; attempt++) {
		format = avformat_alloc_context();
		if (format == NULL)
			break;
		format->interrupt_callback.callback = &_LiveInterrupt;
		format->interrupt_callback.opaque = this;
		AVDictionary* options = NULL;
		av_dict_set(&options, "user_agent", live.agent.IsEmpty()
			? "Cinema (Haiku)" : live.agent.String(), 0);
		if (!live.referrer.IsEmpty())
			av_dict_set(&options, "referer", live.referrer.String(), 0);
		av_dict_set(&options, "rw_timeout", "15000000", 0);
		av_dict_set(&options, "reconnect", "1", 0);
		av_dict_set(&options, "reconnect_streamed", "1", 0);
		av_dict_set(&options, "reconnect_on_network_error", "1", 0);
		int status = avformat_open_input(&format, live.url.String(), NULL,
			&options);
		av_dict_free(&options);
		if (status < 0 && getenv("CINEMA_PLAYER_DEBUG") != NULL) {
			char text[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(status, text, sizeof(text));
			fprintf(stderr, "player: ffmpeg open: %s\n", text);
		}
		refused = status == AVERROR_HTTP_FORBIDDEN
			|| status == AVERROR_HTTP_NOT_FOUND
			|| status == AVERROR_HTTP_UNAUTHORIZED
			|| status == AVERROR_HTTP_BAD_REQUEST
			|| status == AVERROR_HTTP_OTHER_4XX;
		// the context is gone with a failure
		if (status < 0)
			format = NULL;
		else if (avformat_find_stream_info(format, NULL) < 0)
			avformat_close_input(&format);
	}
	if (format == NULL || fQuit) {
		if (format != NULL)
			avformat_close_input(&format);
		// a file the Media Kit may open instead
		if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
			fprintf(stderr, "player: ffmpeg could not open %s%s\n",
				live.url.String(), fQuit ? " (closing)" : "");
		if (!fQuit && (fLive || refused)) {
			_Failed();
			// nothing more to try
			return true;
		}
		return false;
	}
	fLiveFormat = format;

	fLiveVideoIndex = _LiveVideoStream(live.height);
	fLiveAudioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1,
		fLiveVideoIndex, NULL, 0);
	if (fLiveAudioIndex < 0)
		fLiveAudioIndex = -1;
	// an HLS stream shows all its sizes at once, and every one not left
	// alone would be downloaded all along
	for (unsigned i = 0; i < format->nb_streams; i++) {
		format->streams[i]->discard = (int)i == fLiveVideoIndex
			|| (int)i == fLiveAudioIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	}
	fLiveVideo = open_decoder(format, fLiveVideoIndex, true,
		&fLiveHwOpaque);
	if (fLiveVideo == NULL)
		fLiveVideoIndex = -1;
	fLiveAudio = open_decoder(format, fLiveAudioIndex, false, NULL);
	if (fLiveAudio == NULL)
		fLiveAudioIndex = -1;
	fLiveLate = fLiveOnTime = 0;
	_LiveSkip(kSlowProcessor ? 1 : 0);

	// the pictures
	if (fLiveVideo != NULL && !_SetUpBitmaps(fLiveVideo->width & ~1,
			fLiveVideo->height)) {
		avcodec_free_context(&fLiveVideo);
		av_free(fLiveHwOpaque);
		fLiveHwOpaque = NULL;
		fLiveVideoIndex = -1;
	}

	// the sound, as 16 bit samples of the machine's order, two channels
	// at most
	if (fLiveAudio != NULL) {
		int rate = fLiveAudio->sample_rate;
		int channels = std::min(2, fLiveAudio->ch_layout.nb_channels);
		if (rate > 0 && channels > 0) {
			AVChannelLayout layout;
			av_channel_layout_default(&layout, channels);
			// float: without the normalization of the matrix swresample
			// does for 16-bit samples, which turned 5.1 down by 7.6 dB,
			// dialogue included; Loudness makes them 16-bit ones
			swr_alloc_set_opts2(&fLiveResampler, &layout, AV_SAMPLE_FMT_FLT,
				rate, &fLiveAudio->ch_layout, fLiveAudio->sample_fmt,
				fLiveAudio->sample_rate, 0, NULL);
			av_channel_layout_uninit(&layout);
			if (fLiveResampler != NULL && swr_init(fLiveResampler) < 0)
				swr_free(&fLiveResampler);
		}
		if (fLiveResampler != NULL) {
			fSound = media_raw_audio_format::wildcard;
			fSound.frame_rate = rate;
			fSound.channel_count = channels;
			fSound.format = media_raw_audio_format::B_AUDIO_SHORT;
			fSound.byte_order = B_MEDIA_HOST_ENDIAN;
			fSound.buffer_size = 1024 * channels * 2;
			fFrameSize = channels * 2;
			fRing.resize((size_t)((int64)rate * kSoundAhead / 1000000)
				* fFrameSize);
			fRingRead = fRingFill = 0;
			// a new sound player: the clock counts from its start again
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
		}
		if (fSoundPlayer == NULL) {
			if (fLiveResampler != NULL)
				swr_free(&fLiveResampler);
			avcodec_free_context(&fLiveAudio);
			fLiveAudioIndex = -1;
		}
	}

	if (fLiveVideoIndex < 0 && fLiveAudioIndex < 0) {
		if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
			fprintf(stderr, "player: ffmpeg found no stream to play (video "
				"%p, audio %p, sound player %p)\n", fLiveVideo, fLiveAudio,
				fSoundPlayer);
		}
		if (!fLive) {
			// for the Media Kit to try
			delete fSoundPlayer;
			fSoundPlayer = NULL;
			_CloseLive();
			return false;
		}
		_Failed();
		return false;
	}

	fVideoEnded = fLiveVideoIndex < 0;
	fAudioEnded = fLiveAudioIndex < 0;
	fClockFree = false;
	fClockBase = 0;
	fPlayed = 0;
	fLastFill = 0;
	fClockStart = system_time();
	fLiveBase = AV_NOPTS_VALUE;
	fLiveSoundEnd = AV_NOPTS_VALUE;
	fLiveSkipBefore = INT64_MIN;
	fDuration = 0;
	if (!fLive) {
		// the clock counts the time of the video, from where it starts
		fClockBase = fStartAt;
		fPausedClock = fStartAt;
		if (fRelay)
			fDuration = fRelayDuration;
		else {
			if (format->duration > 0)
				fDuration = format->duration;
			// the times of the streams count from the start of the file
			int64 start = format->start_time != AV_NOPTS_VALUE
				? format->start_time : 0;
			fLiveBase = start;
			fLiveSoundEnd = start + fStartAt;
			if (fStartAt > 0) {
				// from the key frame before, what comes before is not played
				if (av_seek_frame(format, -1, start + fStartAt,
						AVSEEK_FLAG_BACKWARD) >= 0)
					fLiveSkipBefore = start + fStartAt;
				else {
					// it cannot be moved in: from the start, rather than
					// decoding all that comes before in the dark
					fClockBase = fPausedClock = 0;
					fLiveSoundEnd = start;
				}
			}
		}
		if (fAddress.ByteAt(0) == '/' && !fSameVideo) {
			fSubtitles.LoadSidecars(fAddress.String());
			fSubtitles.StartEmbedded(fAddress.String());
		}
	}
	fShowNext = true;
	fOpened = true;
	if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
		fprintf(stderr, "player: ffmpeg %s, %.1f s, from %.1f s\n",
			fLive ? "live" : fRelay ? "relay" : "file", fDuration / 1e6,
			fStartAt / 1e6);
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
		return true;
	fVideoThread = spawn_thread(&_LiveEntry, "player live", B_DISPLAY_PRIORITY,
		this);
	resume_thread(fVideoThread);
	if (fSoundPlayer != NULL) {
		// as BSoundPlayer::Start() starts its node (see _Clock())
		bigtime_t performance = fSoundPlayer->PerformanceTime();
		bigtime_t latency = fSoundPlayer->Latency();
		fSoundPlayer->Start();
		if (performance > 0)
			fPerformanceStart = performance + latency + 5000;
		fSoundPlayer->SetHasData(true);
	}
	return true;
}


void
Player::_CloseLive()
{
	if (fLiveScaler != NULL)
		sws_freeContext(fLiveScaler);
	fLiveScaler = NULL;
	if (fLiveResampler != NULL)
		swr_free(&fLiveResampler);
	if (fLiveAudio != NULL)
		avcodec_free_context(&fLiveAudio);
	if (fLiveVideo != NULL)
		avcodec_free_context(&fLiveVideo);
	av_free(fLiveHwOpaque);
	fLiveHwOpaque = NULL;
	if (fLiveFormat != NULL)
		avformat_close_input(&fLiveFormat);
	fLiveVideoIndex = fLiveAudioIndex = -1;
	fLivePcm.clear();
}


/*!	The picture to play. With several sizes (adaptive HLS), the tallest
	not taller than maxHeight, or the smallest; the lowest rate between
	two of a size. maxHeight 0: the one FFmpeg finds best.
*/
int
Player::_LiveVideoStream(int32 maxHeight)
{
	AVFormatContext* format = fLiveFormat;
	int best = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL,
		0);
	if (best < 0)
		return best;
	maxHeight = maxHeight > 0 ? LimitVideoHeight(maxHeight)
		: VideoHeightLimit();

	int chosen = -1;
	int chosenHeight = 0;
	int64 chosenRate = 0;
	int candidates = 0;
	for (unsigned i = 0; i < format->nb_streams; i++) {
		const AVStream* stream = format->streams[i];
		const AVCodecParameters* parameters = stream->codecpar;
		if (parameters->codec_type != AVMEDIA_TYPE_VIDEO
			|| parameters->height <= 0
			|| (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0
			|| avcodec_find_decoder(parameters->codec_id) == NULL)
			continue;
		candidates++;
		int64 rate = parameters->bit_rate;
		const AVDictionaryEntry* entry = av_dict_get(stream->metadata,
			"variant_bitrate", NULL, 0);
		if (entry != NULL)
			rate = strtoll(entry->value, NULL, 10);
		int height = parameters->height;
		bool better;
		if (chosen < 0)
			better = true;
		else {
			bool fits = height <= maxHeight;
			bool chosenFits = chosenHeight <= maxHeight;
			if (fits != chosenFits)
				better = fits;
			else if (height != chosenHeight)
				better = fits ? height > chosenHeight : height < chosenHeight;
			else
				better = rate > 0 && (chosenRate <= 0 || rate < chosenRate);
		}
		if (better) {
			chosen = i;
			chosenHeight = height;
			chosenRate = rate;
		}
	}
	return candidates > 1 && chosen >= 0 ? chosen : best;
}


/*!	0: all decoded. 1: without the loop filter of H.264, a good part of
	the time for a little blockiness (on a slow processor, always). 2: the
	frames nothing refers to skipped too, while it is behind.
*/
void
Player::_LiveSkip(int level)
{
	if (fLiveVideo == NULL)
		return;
	fLiveSkipLevel = std::max(0, std::min(2, level));
	fLiveVideo->skip_loop_filter = fLiveSkipLevel >= 1
		? AVDISCARD_ALL : AVDISCARD_DEFAULT;
	fLiveVideo->skip_frame = fLiveSkipLevel >= 2
		? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}


/*!	Skips more after a few late pictures in a row, less after about two
	seconds on time; not while it starts.
*/
void
Player::_LiveLateness(bool late)
{
	if (_LiveClock() - fClockBase < kSettling)
		return;
	int base = kSlowProcessor ? 1 : 0;
	if (late) {
		// a late picture now and then, after the network held the stream
		// a moment say, does not keep the decoding low with time to spare:
		// it only sets the way back further
		fLiveOnTime = std::max(0, fLiveOnTime - 10);
		if (++fLiveLate >= 4 && fLiveSkipLevel < 2) {
			_LiveSkip(2);
			fLiveLate = 0;
		}
	} else {
		fLiveLate = 0;
		if (++fLiveOnTime >= 60 && fLiveSkipLevel > base) {
			_LiveSkip(fLiveSkipLevel - 1);
			fLiveOnTime = 0;
		}
	}
}


int64
Player::_LiveFrameMicros(int index, const AVFrame* frame) const
{
	int64 pts = frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;
	return av_rescale_q(pts, fLiveFormat->streams[index]->time_base,
		AVRational{ 1, 1000000 });
}


/*!	Where the clock is, from fLiveBase: the sound played, or the system
	time without sound.
*/
int64
Player::_LiveClock()
{
	return _Clock();
}


bool
Player::_LiveRender(AVFrame* frame)
{
	if (fBack == NULL)
		return true;
	int width = fBack->Bounds().IntegerWidth() + 1;
	int height = fBack->Bounds().IntegerHeight() + 1;
	int flags = frame->width == width && frame->height == height
		? SWS_POINT : SWS_FAST_BILINEAR;
	fLiveScaler = sws_getCachedContext(fLiveScaler, frame->width,
		frame->height, (AVPixelFormat)frame->format, width, height,
		(AVPixelFormat)fLivePixels, flags, NULL, NULL, NULL);
	if (fLiveScaler == NULL)
		return false;
	uint8_t* planes[4] = { (uint8_t*)fBack->Bits(), NULL, NULL, NULL };
	int strides[4] = { (int)fBack->BytesPerRow(), 0, 0, 0 };
	sws_scale(fLiveScaler, frame->data, frame->linesize, 0, frame->height,
		planes, strides);
	_ShowFrame();
	fShowNext = false;
	if (!fFramePending.exchange(true)
		&& Deliver(fTarget, kMsgPlayerFrame, &fQuit) != B_OK)
		fFramePending = false;
	return true;
}


/*!	Into the ring the sound player takes from; the caller made room. */
void
Player::_LiveQueueSound(const void* data, size_t bytes)
{
	BAutolock ring(fRingLock);
	bytes = std::min(bytes, fRing.size() - fRingFill);
	size_t write = (fRingRead + fRingFill) % fRing.size();
	size_t first = std::min(bytes, fRing.size() - write);
	memcpy(&fRing[write], data, first);
	memcpy(&fRing[0], (const uint8*)data + first, bytes - first);
	fRingFill += bytes;
}


/*!	The clock counts the sound played, so sound that never comes, a hole
	in the stream, would leave it behind for good: short holes are filled
	with silence, a jump of the times is followed.
*/
void
Player::_LiveAlignSound(int64 micros)
{
	fLivePcm.clear();
	if (micros == AV_NOPTS_VALUE || fLiveSoundEnd == AV_NOPTS_VALUE)
		return;
	int64 gap = micros - fLiveSoundEnd;
	if (gap > 2000000 || gap < -2000000) {
		fLiveBase += gap;
		fLiveSoundEnd = micros;
		return;
	}
	if (gap < 20000)
		return;
	int64 samples = gap * (int64)fSound.frame_rate / 1000000;
	fLivePcm.assign(samples * fFrameSize, 0);
	fLiveSoundEnd += samples * 1000000 / (int64)fSound.frame_rate;
}


/*!	The samples of a frame of sound, after the silence _LiveAlignSound()
	may have put in fLivePcm.
*/
void
Player::_LivePushSound(AVFrame* frame)
{
	// a channel may go from 5.1 to stereo, or AAC double its rate: the
	// resampler made again for what comes now, to the same sound out
	AVChannelLayout in;
	int64_t inRate = 0;
	AVSampleFormat inFormat = AV_SAMPLE_FMT_NONE;
	memset(&in, 0, sizeof(in));
	bool same = fLiveResampler != NULL
		&& av_opt_get_chlayout(fLiveResampler, "in_chlayout", 0, &in) >= 0
		&& av_opt_get_int(fLiveResampler, "in_sample_rate", 0, &inRate) >= 0
		&& av_opt_get_sample_fmt(fLiveResampler, "in_sample_fmt", 0,
			&inFormat) >= 0
		&& av_channel_layout_compare(&in, &frame->ch_layout) == 0
		&& inRate == frame->sample_rate && inFormat == frame->format;
	av_channel_layout_uninit(&in);
	if (!same) {
		if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0)
			return;
		AVChannelLayout out;
		av_channel_layout_default(&out, fSound.channel_count);
		swr_free(&fLiveResampler);
		swr_alloc_set_opts2(&fLiveResampler, &out, AV_SAMPLE_FMT_FLT,
			(int)fSound.frame_rate, &frame->ch_layout,
			(AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
		av_channel_layout_uninit(&out);
		if (fLiveResampler == NULL || swr_init(fLiveResampler) < 0) {
			swr_free(&fLiveResampler);
			return;
		}
		if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
			fprintf(stderr, "player: sound now %d channels at %d Hz\n",
				frame->ch_layout.nb_channels, frame->sample_rate);
	}
	int most = swr_get_out_samples(fLiveResampler, frame->nb_samples);
	if (most <= 0)
		return;
	int channels = fSound.channel_count;
	fLiveFloat.resize((size_t)most * channels);
	uint8_t* planes[1] = { (uint8_t*)fLiveFloat.data() };
	int written = swr_convert(fLiveResampler, planes, most,
		(const uint8_t**)frame->extended_data, frame->nb_samples);
	if (written <= 0)
		return;
	// evened, limited and made 16-bit, at the end of what waits for the ring
	size_t before = fLivePcm.size();
	fLivePcm.resize(before + written * fFrameSize);
	fLoudness.Process(fLiveFloat.data(), (int16*)(fLivePcm.data() + before),
		written, channels, fSound.frame_rate);
	if (fLiveSoundEnd != AV_NOPTS_VALUE)
		fLiveSoundEnd += (int64)written * 1000000 / (int64)fSound.frame_rate;
}


status_t
Player::_LiveEntry(void* data)
{
	((Player*)data)->_LiveLoop();
	return B_OK;
}


/*!	Reads the stream: the sound is decoded as it comes, the pictures wait
	compressed until the clock is near them. A stream may carry its
	pictures a second ahead of their sound; decoding them at once would
	wait on a clock only the sound further on moves.
*/
void
Player::_LiveLoop()
{
	AVFormatContext* format = fLiveFormat;
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	std::deque<AVPacket*> packets;
	std::deque<AVFrame*> frames;
	bool started = false;
	bool failed = false;
	const int video = fLiveVideoIndex;
	const int audio = fLiveAudioIndex;

	uint64 pixels = fLiveVideo != NULL
		? (uint64)fLiveVideo->width * fLiveVideo->height : 0;
	bool large = pixels > 1280ULL * 720;
	bool huge = pixels > 1920ULL * 1080;
	uint64 memory = 0;
	system_info info;
	if (get_system_info(&info) == B_OK)
		memory = (uint64)info.max_pages * B_PAGE_SIZE;
	bool lowMemory = memory > 0 && memory < 1024ULL * 1024 * 1024;
	size_t maxDecodedFrames = kMaxDecodedFrames;
	int64 maxQueuedVideo = kMaxQueuedVideo;
	size_t maxReadPackets = kMaxReadPackets;
	if (large) {
		maxDecodedFrames = 8;
		maxQueuedVideo = 4000000;
		maxReadPackets = 1000;
	}
	if (huge) {
		maxDecodedFrames = 6;
		maxQueuedVideo = 2500000;
		maxReadPackets = 700;
	}
	if (lowMemory) {
		maxDecodedFrames = std::min(maxDecodedFrames, (size_t)4);
		maxQueuedVideo = std::min(maxQueuedVideo, (int64)2500000);
		maxReadPackets = std::min(maxReadPackets, (size_t)600);
	}

	// CINEMA_LIVE_DEBUG=1: what happens, every five seconds
	const bool debug = getenv("CINEMA_LIVE_DEBUG") != NULL;
	int decoded = 0, shown = 0, dropped = 0, lateCount = 0, audioFrames = 0;
	bigtime_t nextReport = system_time() + 5000000;
	bigtime_t lastCpuAt = system_time(), lastTeamCpu = 0, lastSystemCpu = 0;
	{
		int32 cpus;
		_CpuTimes(lastTeamCpu, lastSystemCpu, cpus);
	}
	if (debug) {
		fprintf(stderr, "live: video %d (%dx%d), audio %d of %u streams\n",
			video, fLiveVideo != NULL ? fLiveVideo->width : 0,
			fLiveVideo != NULL ? fLiveVideo->height : 0, audio,
			format->nb_streams);
	}

	auto packetMicros = [&](const AVPacket* p) -> int64 {
		int64 time = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
		if (time == AV_NOPTS_VALUE)
			return AV_NOPTS_VALUE;
		return av_rescale_q(time, format->streams[video]->time_base,
			AVRational{ 1, 1000000 });
	};
	// when the last sound was read: a file whose sound ends before its
	// pictures has the clock go on without it, two seconds of nothing
	// later, or the pictures would wait for it and the reading for them
	bigtime_t lastSound = system_time();
	auto soundOver = [&]() {
		if (audio < 0 || fAudioEnded || system_time() - lastSound < 2000000)
			return;
		BAutolock ring(fRingLock);
		if (fRingFill == 0)
			fAudioEnded = true;
	};

	auto queuedSpan = [&]() -> int64 {
		if (packets.size() < 2)
			return 0;
		int64 first = packetMicros(packets.front());
		int64 last = packetMicros(packets.back());
		return first == AV_NOPTS_VALUE || last == AV_NOPTS_VALUE
			? 0 : last - first;
	};
	// when a picture is due on the clock, or AV_NOPTS_VALUE if not known
	auto dueAt = [&](int64 micros) -> int64 {
		if (micros == AV_NOPTS_VALUE || fLiveBase == AV_NOPTS_VALUE)
			return AV_NOPTS_VALUE;
		return micros - fLiveBase;
	};

	// shows the pictures that are due; with wait, holds the first until
	// it is. A late one is dropped unconverted when the next is due too.
	auto showFrames = [&](bool wait) -> bool {
		while (!frames.empty() && !fQuit) {
			AVFrame* next = frames.front();
			int64 at = dueAt(_LiveFrameMicros(video, next));
			int64 now = _LiveClock();
			if (at != AV_NOPTS_VALUE && at > now) {
				if (!wait)
					break;
				for (int guard = 0; guard < 250 && !fQuit; guard++) {
					if (fPaused) {
						guard--;
						snooze_until(system_time() + 20000,
							B_SYSTEM_TIMEBASE);
						continue;
					}
					int64 delay = at - _LiveClock();
					if (delay <= 0)
						break;
					if (started)
						soundOver();
					bigtime_t sleep = (bigtime_t)std::min(delay,
						(int64)50000);
					snooze_until(system_time() + sleep, B_SYSTEM_TIMEBASE);
				}
				wait = false;
				now = _LiveClock();
			}
			frames.pop_front();
			bool known = at != AV_NOPTS_VALUE;
			bool late = known && now - at > kLateMicros;
			bool drop = false;
			if (late && started && !frames.empty()) {
				int64 after = dueAt(_LiveFrameMicros(video, frames.front()));
				drop = after != AV_NOPTS_VALUE && after <= now;
			}
			if (known)
				_LiveLateness(late);
			if (late)
				lateCount++;
			if (drop)
				dropped++;
			else
				shown++;
			bool ok = drop || _LiveRender(next);
			av_frame_free(&next);
			if (!ok)
				return false;
			if (!drop)
				started = true;
		}
		return true;
	};

	// decodes the pictures near the clock, or any once the queue is long
	auto pumpVideo = [&](bool all) -> bool {
		while (!packets.empty() && frames.size() < maxDecodedFrames
			&& !fQuit) {
			AVPacket* next = packets.front();
			if (!all) {
				int64 at = dueAt(packetMicros(next));
				bool near = at == AV_NOPTS_VALUE
					|| at <= _LiveClock() + kDecodeAhead;
				if (!near && queuedSpan() < maxQueuedVideo)
					break;
			}
			packets.pop_front();
			int status = avcodec_send_packet(fLiveVideo, next);
			av_packet_free(&next);
			if (status < 0)
				continue;
			while (avcodec_receive_frame(fLiveVideo, frame) == 0) {
				int64 micros = _LiveFrameMicros(video, frame);
				if (micros != AV_NOPTS_VALUE && fLiveSkipBefore != INT64_MIN
					&& micros < fLiveSkipBefore - 20000) {
					// between the key frame and where it was asked to start
					dropped++;
					av_frame_unref(frame);
					continue;
				}
				if (audio < 0 && fLiveBase == AV_NOPTS_VALUE
					&& micros != AV_NOPTS_VALUE) {
					// without sound the clock starts with the pictures
					fLiveBase = micros - fClockBase;
					fClockStart = system_time();
				}
				AVFrame* decodedFrame = frame;
				AVFrame* hardwareFrame = NULL;
				const AVPixFmtDescriptor* description =
					av_pix_fmt_desc_get((AVPixelFormat)frame->format);
				if (description != NULL
					&& (description->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
					hardwareFrame = av_frame_alloc();
					if (hardwareFrame != NULL
						&& av_hwframe_transfer_data(hardwareFrame,
							frame, 0) == 0) {
						av_frame_copy_props(hardwareFrame, frame);
						hardwareFrame->best_effort_timestamp =
							frame->best_effort_timestamp;
						hardwareFrame->pts = frame->pts;
						decodedFrame = hardwareFrame;
					} else {
						av_frame_free(&hardwareFrame);
						decodedFrame = NULL;
					}
				}
				decoded++;
				if (decodedFrame != NULL) {
					AVFrame* copy = decodedFrame == frame
						? av_frame_clone(frame) : hardwareFrame;
					if (copy != NULL)
						frames.push_back(copy);
					else
						av_frame_free(&hardwareFrame);
				}
				av_frame_unref(frame);
			}
			if (!showFrames(false))
				return false;
		}
		return true;
	};

	// the sound waiting for room in the ring, the pictures shown meanwhile
	auto queueSound = [&]() -> bool {
		size_t offset = 0;
		while (offset < fLivePcm.size() && !fQuit) {
			size_t room;
			{
				BAutolock ring(fRingLock);
				room = fRing.size() - fRingFill;
			}
			room -= room % fFrameSize;
			if (room == 0) {
				if (video >= 0 && (!pumpVideo(false) || !showFrames(false)))
					return false;
				snooze_until(system_time() + 5000,
					B_SYSTEM_TIMEBASE);
				continue;
			}
			size_t bytes = std::min(room, fLivePcm.size() - offset);
			_LiveQueueSound(fLivePcm.data() + offset, bytes);
			offset += bytes;
		}
		fLivePcm.clear();
		return true;
	};

	// the network apart: reading a segment of HLS may hold av_read_frame()
	// for a second or more, while the pictures have to go on
	std::mutex readLock;
	std::deque<AVPacket*> read;
	bool readEnded = false;
	// the network failed, rather than the video ending
	bool readFailed = false;
	fLiveStopRead = false;
	std::thread reader([&]() {
		AVPacket* got = av_packet_alloc();
		while (!fQuit && !fLiveStopRead) {
			bool full;
			{
				std::lock_guard<std::mutex> locker(readLock);
				full = read.size() >= maxReadPackets;
			}
			if (full) {
				snooze_until(system_time() + 20000,
					B_SYSTEM_TIMEBASE);
				continue;
			}
			int status = av_read_frame(format, got);
			if (status < 0) {
				if (status != AVERROR_EOF && !fQuit && !fLiveStopRead) {
					std::lock_guard<std::mutex> locker(readLock);
					readFailed = true;
				}
				break;
			}
			if (got->stream_index != video && got->stream_index != audio) {
				av_packet_unref(got);
				continue;
			}
			AVPacket* copy = av_packet_alloc();
			av_packet_move_ref(copy, got);
			std::lock_guard<std::mutex> locker(readLock);
			read.push_back(copy);
		}
		av_packet_free(&got);
		std::lock_guard<std::mutex> locker(readLock);
		readEnded = true;
	});

	while (!fQuit && !failed) {
		if (fPaused) {
			snooze_until(system_time() + 40000,
				B_SYSTEM_TIMEBASE);
			continue;
		}
		AVPacket* next = NULL;
		bool ended;
		{
			std::lock_guard<std::mutex> locker(readLock);
			if (!read.empty()) {
				next = read.front();
				read.pop_front();
			}
			ended = next == NULL && readEnded;
		}
		if (next == NULL && !ended) {
			// nothing read yet: the pictures that are due meanwhile
			if (video >= 0 && (!pumpVideo(false) || !showFrames(false)))
				failed = true;
			snooze_until(system_time() + 5000, B_SYSTEM_TIMEBASE);
			continue;
		}
		if (next != NULL) {
			av_packet_move_ref(packet, next);
			av_packet_free(&next);
		}
		if (ended) {
			// the end: what is still there. The sound too, and then the
			// clock goes on without it
			fAudioEnded = true;
			// an empty packet: the decoder gives the pictures it still
			// holds, the last ones of the video
			if (video >= 0) {
				if (AVPacket* flush = av_packet_alloc())
					packets.push_back(flush);
			}
			while (!fQuit && !failed && (!packets.empty() || !frames.empty())) {
				if (!pumpVideo(true) || !showFrames(true))
					failed = true;
			}
			break;
		}

		if (packet->stream_index == video && video >= 0) {
			if (AVPacket* copy = av_packet_clone(packet))
				packets.push_back(copy);
			av_packet_unref(packet);
		} else if (packet->stream_index == audio && audio >= 0) {
			lastSound = system_time();
			int status = avcodec_send_packet(fLiveAudio, packet);
			av_packet_unref(packet);
			while (status >= 0 && !fQuit
				&& avcodec_receive_frame(fLiveAudio, frame) == 0) {
				int64 micros = _LiveFrameMicros(audio, frame);
				if (micros != AV_NOPTS_VALUE && micros < fLiveSkipBefore) {
					// before where it was asked to start
					av_frame_unref(frame);
					continue;
				}
				if (fLiveBase == AV_NOPTS_VALUE && micros != AV_NOPTS_VALUE) {
					// the clock starts where the sound does (the relay
					// from fStartAt)
					fLiveBase = micros - fClockBase;
					fLiveSoundEnd = micros;
				}
				audioFrames++;
				_LiveAlignSound(micros);
				_LivePushSound(frame);
				av_frame_unref(frame);
				if (!queueSound()) {
					failed = true;
					break;
				}
			}
		} else
			av_packet_unref(packet);

		if (video >= 0 && (!pumpVideo(false) || !showFrames(false)))
			failed = true;

		if (debug && system_time() > nextReport) {
			// how busy Cinema and the whole system were since the last time
			bigtime_t now = system_time(), team, system;
			int32 cpus;
			_CpuTimes(team, system, cpus);
			double capacity = (now - lastCpuAt) * (double)cpus;
			double teamLoad = 100.0 * (team - lastTeamCpu) / capacity;
			double systemLoad = 100.0 * (system - lastSystemCpu) / capacity;
			lastCpuAt = now;
			lastTeamCpu = team;
			lastSystemCpu = system;
			size_t fill;
			{
				BAutolock ring(fRingLock);
				fill = fRingFill;
			}
			size_t readCount;
			{
				std::lock_guard<std::mutex> locker(readLock);
				readCount = read.size();
			}
			int64 head = frames.empty() ? 0
				: dueAt(_LiveFrameMicros(video, frames.front()));
			fprintf(stderr, "live: decoded %d shown %d dropped %d late %d "
				"audio %d skip %d | clock %.2f next %.2f | packets %zu "
				"(%.1fs) frames %zu read %zu sound %.2fs | %s cinema %.0f%% "
				"system %.0f%% | level %.1f dB gain %+.1f dB%s\n", decoded,
				shown, dropped,
				lateCount, audioFrames, fLiveSkipLevel, _LiveClock() / 1e6,
				head / 1e6, packets.size(), queuedSpan() / 1e6, frames.size(),
				readCount,
				fFrameSize > 0 && fSound.frame_rate > 0
					? fill / (double)fFrameSize / fSound.frame_rate : 0.0,
				fOverlay != NULL ? "overlay" : "bgra", teamLoad, systemLoad,
				fLoudness.LevelDB(), fLoudness.GainDB(),
				fLoudness.IsEnabled() ? "" : " (even volume off)");
			decoded = shown = dropped = lateCount = audioFrames = 0;
			nextReport += 5000000;
		}

		// the sound of the file over before its pictures: their queue full
		if (started && video >= 0 && queuedSpan() >= maxQueuedVideo)
			soundOver();

		// without sound nothing else paces the reading
		if (!failed && video >= 0 && frames.size() >= maxDecodedFrames
			&& queuedSpan() >= maxQueuedVideo && !showFrames(true))
			failed = true;
	}

	fLiveStopRead = true;
	reader.join();
	for (AVPacket* queued : read)
		av_packet_free(&queued);
	for (AVPacket* queued : packets)
		av_packet_free(&queued);
	for (AVFrame* queued : frames)
		av_frame_free(&queued);
	av_frame_free(&frame);
	av_packet_free(&packet);

	if (fQuit)
		return;
	// the sound queued plays out first
	while (!fQuit) {
		{
			BAutolock ring(fRingLock);
			if (fRingFill == 0 || fSoundPlayer == NULL)
				break;
		}
		snooze_until(system_time() + 20000, B_SYSTEM_TIMEBASE);
	}
	// a video from the network that stopped halfway is not over: the
	// window finds it again, or says so
	bool broken = readFailed && !fLive && fAddress.ByteAt(0) != '/'
		&& fDuration > 0 && Position() < fDuration - 5000000;
	if (!fQuit && !fEndSent.exchange(true))
		_Tell((started || !failed) && !broken ? kMsgPlayerEnded
			: kMsgPlayerError);
}


BBitmap*
Player::PausedPicture()
{
	if (!fPaused || fOverlay == NULL)
		return NULL;
	BAutolock locker(fFrameLock);
	if (!fHasFrame || fFront == NULL || fFront->ColorSpace() != B_YCbCr422)
		return NULL;
	if (fPausedPicture != NULL && fPausedPictureOf == fShownCount)
		return fPausedPicture;

	BRect bounds = fFront->Bounds();
	int width = bounds.IntegerWidth() + 1;
	int height = bounds.IntegerHeight() + 1;
	if (fPausedPicture == NULL || fPausedPicture->Bounds() != bounds) {
		delete fPausedPicture;
		fPausedPicture = new BBitmap(bounds, B_RGB32);
		if (fPausedPicture->InitCheck() != B_OK) {
			delete fPausedPicture;
			fPausedPicture = NULL;
			return NULL;
		}
	}
	// Haiku's B_YCbCr422 is FFmpeg's YUYV422
	fPauseScaler = sws_getCachedContext(fPauseScaler, width, height,
		AV_PIX_FMT_YUYV422, width, height, AV_PIX_FMT_BGRA, SWS_POINT, NULL,
		NULL, NULL);
	if (fPauseScaler == NULL)
		return NULL;
	const uint8_t* from[4] = { (const uint8_t*)fFront->Bits(), NULL, NULL,
		NULL };
	int fromStrides[4] = { (int)fFront->BytesPerRow(), 0, 0, 0 };
	uint8_t* to[4] = { (uint8_t*)fPausedPicture->Bits(), NULL, NULL, NULL };
	int toStrides[4] = { (int)fPausedPicture->BytesPerRow(), 0, 0, 0 };
	sws_scale(fPauseScaler, from, fromStrides, 0, height, to, toStrides);
	fPausedPictureOf = fShownCount;
	return fPausedPicture;
}


void
Player::_DropPausedPicture()
{
	delete fPausedPicture;
	fPausedPicture = NULL;
	fPausedPictureOf = -1;
	if (fPauseScaler != NULL)
		sws_freeContext(fPauseScaler);
	fPauseScaler = NULL;
}
