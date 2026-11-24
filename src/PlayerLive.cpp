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
#include <libavutil/cpu.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Bitmap.h>
#include <OS.h>
#include <MediaRoster.h>
#include <SoundPlayer.h>

#include <algorithm>
#include <deque>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
		// a multiple of 8 wide: libswscale 6.1 converts other widths in
		// slices with two grey pixels at the start of each slice, and the
		// screen fits the bitmap to the window anyway
		width = std::max((int32)8,
			((int32)(width * scale + 0.5f)) & ~7);
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
		// a torrent the helper serves as it downloads waits for its pieces:
		// minutes, rather than the 15 seconds of a server gone quiet
		av_dict_set(&options, "rw_timeout",
			live.url.StartsWith("http://localhost:") ? "600000000" : "15000000",
			0);
		// not to the relay of the helper: asked again, it would start the
		// video over from where it was opened, the picture waiting for a
		// time gone by; the window finds it again instead
		if (!fRelay) {
			av_dict_set(&options, "reconnect", "1", 0);
			av_dict_set(&options, "reconnect_streamed", "1", 0);
			av_dict_set(&options, "reconnect_on_network_error", "1", 0);
			// a few seconds of trying again, not FFmpeg's two minutes: the
			// window finds the video again after that, and closing it does
			// not wait meanwhile
			av_dict_set(&options, "reconnect_delay_max", "5", 0);
		}
		// the name of the server looked up here first, for 8 s at most and
		// giving up as soon as the video is closed: FFmpeg's own lookup
		// cannot be interrupted, and with a DNS that does not answer it
		// held the window closing the video for tens of seconds
		if (!_LiveHostKnown(live.url)) {
			av_dict_free(&options);
			avformat_free_context(format);
			format = NULL;
			break;
		}
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
	if (fLiveVideoIndex >= 0) {
		const AVStream* stream = format->streams[fLiveVideoIndex];
		fLiveVideoBase[0] = stream->time_base.num;
		fLiveVideoBase[1] = stream->time_base.den;
		fLiveVideoRate[0] = stream->avg_frame_rate.num;
		fLiveVideoRate[1] = stream->avg_frame_rate.den;
	}
	if (fLiveAudioIndex >= 0) {
		fLiveAudioBase[0] = format->streams[fLiveAudioIndex]->time_base.num;
		fLiveAudioBase[1] = format->streams[fLiveAudioIndex]->time_base.den;
	}
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
	fLiveStarveSince = 0;
	fLiveKeyWait = 10000000;
	fStruggleSent = false;
	{
		BAutolock locker(fStatsLock);
		fHasStats = false;
	}
	// a video heavier than most processors decode whole without help
	// (HEVC above 1080p, anything above 1440p): without the deblocking
	// filter from the start, rather than late pictures first
	fLiveFloor = kSlowProcessor ? 1 : 0;
	if (fLiveVideo != NULL) {
		uint64 pixels = (uint64)fLiveVideo->width * fLiveVideo->height;
		if ((fLiveVideo->codec_id == AV_CODEC_ID_HEVC
				&& pixels > 1920ULL * 1080 * 11 / 10)
			|| pixels > 2560ULL * 1440 * 11 / 10)
			fLiveFloor = 1;
	}
	_LiveSkip(fLiveFloor);

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
	fHasVideo = fLiveVideo != NULL;
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
	if (candidates > 1 && chosen >= 0)
		return chosen;
	// the picture of an album or a podcast is no video: only sound, shown
	// with its picture by the window (a single frame at time 0 would be
	// dropped after a seek, leaving the screen black)
	if (best >= 0 && (format->streams[best]->disposition
			& AV_DISPOSITION_ATTACHED_PIC) != 0)
		return chosen >= 0 ? chosen : -1;
	return best;
}


/*!	Whether nothing refers to a picture of H.264, so that it can be left
	out alone: all its slices have a nal_ref_idc of 0. lengthSize is the
	size of the lengths before its units (an MP4), 0 for start codes (a
	transport stream).
*/
static bool
h264_disposable(const AVPacket* packet, int lengthSize)
{
	const uint8* data = packet->data;
	int size = packet->size;
	bool slices = false;
	int at = 0;
	while (at < size) {
		int start;
		int end;
		if (lengthSize > 0) {
			if (at + lengthSize > size)
				break;
			int length = 0;
			for (int i = 0; i < lengthSize; i++)
				length = (length << 8) | data[at + i];
			start = at + lengthSize;
			end = std::min(size, start + length);
			if (length <= 0)
				break;
		} else {
			// the next start code, 00 00 01
			int code = at;
			while (code + 2 < size && !(data[code] == 0
					&& data[code + 1] == 0 && data[code + 2] == 1))
				code++;
			if (code + 2 >= size)
				break;
			start = code + 3;
			end = start;
			while (end + 2 < size && !(data[end] == 0 && data[end + 1] == 0
					&& (data[end + 2] == 1 || data[end + 2] == 0)))
				end++;
			if (end + 2 >= size)
				end = size;
		}
		if (start < end) {
			int type = data[start] & 0x1f;
			int referred = (data[start] >> 5) & 3;
			if (type == 1 || type == 5) {
				if (referred != 0 || type == 5)
					return false;
				slices = true;
			}
		}
		at = end;
	}
	return slices;
}


/*!	0: all decoded. 1: without the loop filter of H.264, a good part of
	the time for a little blockiness (on a slow processor, always). 2: one
	of every two pictures nothing refers to left out too (H.264): 20 of 30
	frames a second in most films, where 3 shows 10. 3: all of those left
	out, while it is behind. 4: only the key frames.
*/
void
Player::_LiveSkip(int level)
{
	if (fLiveVideo == NULL)
		return;
	level = std::max(0, std::min(4, level));
	// only H.264 says which pictures nothing refers to before they are
	// decoded: for the others there is no 2
	if (level == 2 && fLiveVideo->codec_id != AV_CODEC_ID_H264)
		level = fLiveSkipLevel > 2 ? 1 : 3;
	if (level != fLiveSkipLevel)
		fLiveLevelSince = system_time();
	fLiveSkipLevel = level;
	fLiveVideo->skip_loop_filter = fLiveSkipLevel >= 1
		? AVDISCARD_ALL : AVDISCARD_DEFAULT;
	fLiveVideo->skip_frame = fLiveSkipLevel >= 4 ? AVDISCARD_NONKEY
		: fLiveSkipLevel >= 3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
	// too much for this machine: the window may want less of it
	if (fLiveSkipLevel >= 4)
		_Struggling();
}


/*!	The pictures cost more than this machine has: told once to the window,
	which plays a video of YouTube at a smaller size then.
*/
void
Player::_Struggling()
{
	if (fStruggleSent.exchange(true))
		return;
	BMessage struggling(kMsgPlayerStruggling);
	struggling.AddInt32("height", fLiveVideo != NULL ? fLiveVideo->height : 0);
	_Tell(&struggling);
}


/*!	Each turn of the loop: the sound running out while the pictures are
	decoded means the processor cannot keep up, as the clock follows the
	sound and the pictures seem on time; then only the key frames, the sound
	whole. After a while there, the frames that are referred to again, a
	longer while each time it was too much.
*/
void
Player::_LiveLoad()
{
	if (fLiveVideo == NULL || _LiveClock() - fClockBase < kSettling)
		return;
	bigtime_t now = system_time();
	// the sound player found the ring empty in the last second
	int32 underruns = fSoundUnderruns;
	if (underruns != fLiveUnderrunsSeen) {
		fLiveUnderrunsSeen = underruns;
		fLiveUnderrunAt = now;
	}
	// only what ran out since the level began: the sound that ran out at
	// the level before, while it fell behind, is not this one's
	bool starving = now - fLiveUnderrunAt < 1000000
		&& fLiveUnderrunAt > fLiveLevelSince + 500000;
	if (!starving || fLiveSkipLevel < 3)
		fLiveStarveSince = 0;
	else if (fLiveStarveSince == 0)
		fLiveStarveSince = now;
	if (fLiveSkipLevel == 3 && fLiveStarveSince != 0
		&& now - fLiveStarveSince > 3000000) {
		fLiveStarveSince = 0;
		_LiveSkip(4);
		return;
	}
	if (fLiveSkipLevel == 3 && now - fLiveLevelSince > 15000000)
		_Struggling();
	// CINEMA_FORCE_STRUGGLE=1: as if the processor could not keep up, 20 s
	// in, to try what follows on a machine that can
	static const bool force = getenv("CINEMA_FORCE_STRUGGLE") != NULL
		&& strcmp(getenv("CINEMA_FORCE_STRUGGLE"), "1") == 0;
	if (force && _LiveClock() - fClockBase > 20000000)
		_Struggling();
	if (fLiveSkipLevel == 4 && now - fLiveLevelSince > fLiveKeyWait) {
		fLiveKeyWait = std::min(fLiveKeyWait * 2, (bigtime_t)60000000);
		_LiveSkip(3);
	}
}


/*!	Skips more after a few late pictures in a row, less after about two
	seconds on time; not while it starts.
*/
void
Player::_LiveLateness(bool late, int64 behind)
{
	if (_LiveClock() - fClockBase < kSettling)
		return;
	int base = fLiveFloor;
	if (late) {
		// a late picture now and then, after the network held the stream
		// a moment say, does not keep the decoding low with time to spare:
		// it only sets the way back further
		fLiveOnTime = std::max(0, fLiveOnTime - 10);
		// first one of two of the pictures nothing refers to, then all
		if (++fLiveLate >= 4 && fLiveSkipLevel < 3) {
			_LiveSkip(fLiveSkipLevel < 2 ? 2 : 3);
			fLiveLate = 0;
		}
		// the pictures fallen well behind the sound with the reference ones
		// only: the key frames, until they catch up. Pictures a little late
		// still move, where the key frames show one every second or two;
		// the sound, on a thread of its own, no longer runs out to say it
		// (_LiveLoad())
		if (fLiveSkipLevel == 3 && behind > 1500000
			&& system_time() - fLiveLevelSince > 3000000) {
			_LiveSkip(4);
			fLiveLate = 0;
		}
	} else {
		fLiveLate = 0;
		// from the key frames only, the time says when to try again
		if (++fLiveOnTime >= 60 && fLiveSkipLevel > base
			&& fLiveSkipLevel < 4) {
			_LiveSkip(fLiveSkipLevel - 1);
			fLiveOnTime = 0;
		}
	}
}


/*!	Whether the host of an address can be found, looked up on a thread
	of its own: false when it cannot in 8 s, or the video is closed
	meanwhile. The lookup goes on alone then, and ends by itself.
*/
bool
Player::_LiveHostKnown(const BString& address)
{
	BString url(address);
	int32 scheme = url.FindFirst("://");
	if (scheme < 0 || !url.IStartsWith("http"))
		return true;
	BString host;
	url.CopyInto(host, scheme + 3, url.Length() - scheme - 3);
	int32 end = host.FindFirst('/');
	if (end >= 0)
		host.Truncate(end);
	int32 at = host.FindLast('@');
	if (at >= 0)
		host.Remove(0, at + 1);
	if (host.ByteAt(0) == '[')
		return true;	// an address of IPv6, nothing to look up
	int32 colon = host.FindFirst(':');
	if (colon >= 0)
		host.Truncate(colon);
	if (host.IsEmpty() || host == "localhost" || host == "127.0.0.1")
		return true;

	struct Lookup {
		std::atomic<int> state{0};	// 0 going, 1 found, 2 not
	};
	std::shared_ptr<Lookup> lookup = std::make_shared<Lookup>();
	std::string name(host.String());
	try {
		std::thread([lookup, name]() {
			struct addrinfo hints = {};
			hints.ai_socktype = SOCK_STREAM;
			struct addrinfo* found = NULL;
			int status = getaddrinfo(name.c_str(), NULL, &hints, &found);
			if (found != NULL)
				freeaddrinfo(found);
			lookup->state = status == 0 ? 1 : 2;
		}).detach();
	} catch (...) {
		// no thread for it: FFmpeg looks it up as before
		return true;
	}
	bigtime_t until = system_time() + 8000000;
	while (lookup->state == 0 && !fQuit && system_time() < until)
		snooze(50000);
	if (lookup->state == 1)
		return true;
	if (getenv("CINEMA_PLAYER_DEBUG") != NULL) {
		fprintf(stderr, "player: %s not found%s\n", name.c_str(),
			fQuit ? " (closing)" : "");
	}
	return false;
}


int64
Player::_LiveFrameMicros(int index, const AVFrame* frame) const
{
	int64 pts = frame->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
		return AV_NOPTS_VALUE;
	const int* base = index == fLiveVideoIndex ? fLiveVideoBase
		: fLiveAudioBase;
	return av_rescale_q(pts, AVRational{ base[0], base[1] },
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


/*!	The scaler for this frame and the bitmap, made again only when they
	change: with threads > 1 it converts in slices on that many threads
	(sws_scale_frame()).
*/
SwsContext*
Player::_LiveScaler(const AVFrame* frame, int width, int height, int flags,
	int threads)
{
	if (fLiveScaler != NULL && fScalerFrom[0] == frame->width
		&& fScalerFrom[1] == frame->height
		&& fScalerFrom[2] == frame->format && fScalerTo[0] == width
		&& fScalerTo[1] == height && fScalerTo[2] == fLivePixels
		&& fScalerThreads == threads)
		return fLiveScaler;
	if (fLiveScaler != NULL)
		sws_freeContext(fLiveScaler);
	fLiveScaler = sws_alloc_context();
	if (fLiveScaler == NULL)
		return NULL;
	av_opt_set_int(fLiveScaler, "srcw", frame->width, 0);
	av_opt_set_int(fLiveScaler, "srch", frame->height, 0);
	av_opt_set_int(fLiveScaler, "src_format", frame->format, 0);
	av_opt_set_int(fLiveScaler, "dstw", width, 0);
	av_opt_set_int(fLiveScaler, "dsth", height, 0);
	av_opt_set_int(fLiveScaler, "dst_format", fLivePixels, 0);
	av_opt_set_int(fLiveScaler, "sws_flags", flags, 0);
	av_opt_set_int(fLiveScaler, "threads", threads, 0);
	if (sws_init_context(fLiveScaler, NULL, NULL) < 0) {
		sws_freeContext(fLiveScaler);
		fLiveScaler = NULL;
		return NULL;
	}
	fScalerFrom[0] = frame->width;
	fScalerFrom[1] = frame->height;
	fScalerFrom[2] = frame->format;
	fScalerTo[0] = width;
	fScalerTo[1] = height;
	fScalerTo[2] = fLivePixels;
	fScalerThreads = threads;
	return fLiveScaler;
}


static void
keep_bitmap_bits(void*, uint8_t*)
{
	// the bits are the bitmap's
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
	// the colours of a large picture in slices, on the processors free
	// of the decoding as much as they can be: the Sony drew 1536x2048 on
	// one; a small one is quicker on one than waking the others
	int threads = 1;
	if (fThreadedScaling && width % 8 == 0
		&& (int64)frame->width * frame->height > 1280LL * 720)
		threads = std::max(1, std::min(4, av_cpu_count()));
	if (_LiveScaler(frame, width, height, flags, threads) == NULL)
		return false;
	bool done = false;
	if (threads > 1) {
		// into the bitmap itself, through a frame that only points at it
		AVFrame* target = av_frame_alloc();
		AVBufferRef* bits = av_buffer_create((uint8_t*)fBack->Bits(),
			fBack->BitsLength(), &keep_bitmap_bits, NULL, 0);
		if (target != NULL && bits != NULL) {
			target->buf[0] = bits;
			bits = NULL;
			target->data[0] = (uint8_t*)fBack->Bits();
			target->linesize[0] = (int)fBack->BytesPerRow();
			target->width = width;
			target->height = height;
			target->format = fLivePixels;
			done = sws_scale_frame(fLiveScaler, target, frame) >= 0;
		}
		av_buffer_unref(&bits);
		av_frame_free(&target);
		if (!done) {
			// not this scaler: as before, on one thread, from now on
			fThreadedScaling = false;
			if (_LiveScaler(frame, width, height, flags, 1) == NULL)
				return false;
		}
	}
	if (!done) {
		uint8_t* planes[4] = { (uint8_t*)fBack->Bits(), NULL, NULL, NULL };
		int strides[4] = { (int)fBack->BytesPerRow(), 0, 0, 0 };
		sws_scale(fLiveScaler, frame->data, frame->linesize, 0, frame->height,
			planes, strides);
	}
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


/*!	A new sound output, the old one gone silent (the media server started
	again): the ring emptied of what the old one did not play, the clock
	counting again from the sound at micros, which comes next. The old
	output is kept until the close, as other threads may still ask it the
	time. False if none can be made yet.
*/
bool
Player::_LiveReviveSound(int64 micros)
{
	if (micros == AV_NOPTS_VALUE)
		return false;
	BSoundPlayer* player = new(std::nothrow) BSoundPlayer(&fSound, "Cinema",
		&_PlayBuffer, NULL, this);
	if (player == NULL)
		return false;
	if (player->InitCheck() != B_OK) {
		delete player;
		return false;
	}
	{
		BAutolock ring(fRingLock);
		fRingRead = fRingFill = 0;
	}
	fPerformanceStart = 0;
	{
		BAutolock locker(fFillLock);
		fFillCount = fFillNext = 0;
		fSentFrames = 0;
	}
	fPlayed = 0;
	fLiveSoundEnd = micros;
	fClockBase = micros - fLiveBase;
	if (fSoundPlayer != NULL)
		fOldSoundPlayers.push_back(fSoundPlayer);
	fSoundPlayer = player;
	fLatency = player->Latency();
	_ApplyVolume();
	bigtime_t performance = player->PerformanceTime();
	bigtime_t latency = player->Latency();
	player->Start();
	if (performance > 0)
		fPerformanceStart = performance + latency + 5000;
	player->SetHasData(true);
	fClockFree = false;
	return true;
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
	// out of memory: ended at once, with an error rather than a crash
	bool failed = packet == NULL || frame == NULL;
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
	int decoded = 0, shown = 0, dropped = 0, lateCount = 0;
	std::atomic<int> audioFrames(0);
	bigtime_t nextReport = system_time() + 5000000;
	// for the statistics, every second
	int droppedAll = 0, shownSecond = 0;
	bigtime_t nextStats = system_time() + 1000000;
	// H.264 in an MP4 says the size of the lengths before its units in
	// avcC (1 at its start); in a transport stream they have start codes
	int lengthSize = 0;
	if (fLiveVideo != NULL && fLiveVideo->codec_id == AV_CODEC_ID_H264
		&& fLiveVideo->extradata != NULL && fLiveVideo->extradata_size > 4
		&& fLiveVideo->extradata[0] == 1)
		lengthSize = (fLiveVideo->extradata[4] & 3) + 1;
	uint32 disposables = 0;
	bigtime_t statsCpuAt = system_time(), statsTeamCpu = 0, statsSystemCpu = 0;
	{
		int32 cpus;
		_CpuTimes(statsTeamCpu, statsSystemCpu, cpus);
	}
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
		return av_rescale_q(time,
			AVRational{ fLiveVideoBase[0], fLiveVideoBase[1] },
			AVRational{ 1, 1000000 });
	};
	// when the last sound was read: a file whose sound ends before its
	// pictures has the clock go on without it, two seconds of nothing
	// later, or the pictures would wait for it and the reading for them
	std::atomic<bigtime_t> lastSound(system_time());
	// packets of sound read and not handled yet: with them waiting, the
	// sound is not over, only late, on a processor that cannot keep up
	std::atomic<int32> soundQueued(0);
	auto soundOver = [&]() {
		if (audio < 0 || fAudioEnded || system_time() - lastSound < 2000000
			|| soundQueued > 0)
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
				_LiveLateness(late, now - at);
			if (late)
				lateCount++;
			if (drop)
				dropped++, droppedAll++;
			else
				shown++, shownSecond++;
			bool ok = drop || _LiveRender(next);
			av_frame_free(&next);
			if (!ok)
				return false;
			if (!drop)
				started = true;
			// the key frames only were to catch up with the sound: one shown
			// on time, it has, and the reference pictures come again. A
			// film may have a key frame every eight seconds, which is all
			// that level shows
			if (!drop && !late && known && fLiveSkipLevel >= 4)
				_LiveSkip(3);
		}
		return true;
	};

	// how many pictures decoded may wait: one with the key frames only,
	// which are seconds apart, or the decoder runs that far ahead of the
	// clock, and going back to more pictures holds the video as long
	auto mostFrames = [&]() -> size_t {
		return fLiveSkipLevel >= 4 ? 1 : maxDecodedFrames;
	};

	// decodes the pictures near the clock, or any once the queue is long
	auto pumpVideo = [&](bool all) -> bool {
		while (!packets.empty() && frames.size() < mostFrames()
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
			// one of two of the pictures nothing refers to, not decoded
			if (fLiveSkipLevel == 2 && h264_disposable(next, lengthSize)
				&& (++disposables & 1) != 0) {
				av_packet_free(&next);
				dropped++, droppedAll++;
				continue;
			}
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


	// the network apart: reading a segment of HLS may hold av_read_frame()
	// for a second or more, while the pictures have to go on
	std::mutex readLock;
	std::deque<AVPacket*> read;
	// the sound read, for the thread of the sound: it never waits for the
	// pictures, which a slow processor decodes late
	std::deque<AVPacket*> soundRead;
	std::condition_variable soundCome;
	std::atomic<bool> soundStop(false);
	bool readEnded = false;
	// the network failed, rather than the video ending
	bool readFailed = false;
	fLiveStopRead = false;
	auto readAll = [&]() {
		AVPacket* got = av_packet_alloc();
		while (got != NULL && !fQuit && !fLiveStopRead) {
			bool full;
			{
				// not when one queue alone is full: an MP4 has a second of
				// each in turn, and the sound waiting for room in the ring
				// would hold the pictures. Both enough, or far too much
				std::lock_guard<std::mutex> locker(readLock);
				size_t soundEnough = audio >= 0 ? 150 : 0;
				full = (read.size() >= maxReadPackets
						&& soundRead.size() >= soundEnough)
					|| read.size() + soundRead.size() >= maxReadPackets * 2;
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
			if (copy == NULL) {
				av_packet_unref(got);
				std::lock_guard<std::mutex> locker(readLock);
				readFailed = true;
				break;
			}
			av_packet_move_ref(copy, got);
			std::lock_guard<std::mutex> locker(readLock);
			if (copy->stream_index == audio) {
				soundQueued++;
				soundRead.push_back(copy);
				soundCome.notify_one();
			} else
				read.push_back(copy);
		}
		av_packet_free(&got);
	};
	// no thread, or no memory for the queue: the video ends with an error
	// rather than Cinema
	std::thread reader;
	try {
		reader = std::thread([&]() {
			try {
				readAll();
			} catch (...) {
				std::lock_guard<std::mutex> locker(readLock);
				readFailed = true;
			}
			std::lock_guard<std::mutex> locker(readLock);
			readEnded = true;
			soundCome.notify_one();
		});
	} catch (...) {
		failed = true;
		readEnded = true;
	}

	// the sound, on a thread of its own above the decoders of the pictures:
	// decoded, evened and put in the ring as it is read, whatever the
	// pictures do. It owns the decoder of the sound, the resampler, and
	// fLivePcm; done once all that was read is in the ring
	std::atomic<bool> soundDone(audio < 0);
	// the sound output stopped taking the sound (the media server gone, a
	// driver stuck): the pictures go on by the time, the sound is let go
	std::atomic<bool> soundDead(false);
	// the media server is there again: the thread of the sound makes a new
	// output at its next frame, and the clock follows it again
	std::atomic<bool> soundRevive(false);
	std::atomic<bigtime_t> soundBackAt(0);
	bigtime_t stillClock = -1, stillSince = system_time();
	bigtime_t lastRevive = 0;
	// not before the clock moved once: the output takes a while to start
	bool clockMoved = false;
	auto soundLoop = [&]() {
		rename_thread(find_thread(NULL), "player sound");
		set_thread_priority(find_thread(NULL), B_URGENT_DISPLAY_PRIORITY);
		AVFrame* got = av_frame_alloc();
		while (got != NULL && !fQuit && !soundStop) {
			AVPacket* next = NULL;
			{
				std::unique_lock<std::mutex> locker(readLock);
				if (soundRead.empty() && !readEnded)
					soundCome.wait_for(locker, std::chrono::milliseconds(20));
				if (!soundRead.empty()) {
					next = soundRead.front();
					soundRead.pop_front();
				} else if (readEnded)
					break;
			}
			if (next == NULL)
				continue;
			lastSound = system_time();
			int status = avcodec_send_packet(fLiveAudio, next);
			av_packet_free(&next);
			soundQueued--;
			while (status >= 0 && !fQuit && !soundStop
				&& avcodec_receive_frame(fLiveAudio, got) == 0) {
				int64 micros = _LiveFrameMicros(audio, got);
				if (micros != AV_NOPTS_VALUE && micros < fLiveSkipBefore) {
					// before where it was asked to start
					av_frame_unref(got);
					continue;
				}
				if (fLiveBase == AV_NOPTS_VALUE && micros != AV_NOPTS_VALUE) {
					// the clock starts where the sound does (the relay
					// from fStartAt)
					fLiveBase = micros - fClockBase;
					fLiveSoundEnd = micros;
				}
				audioFrames++;
				if (soundDead) {
					// no output: the sound kept with the pictures, not ahead
					// of them, and let go; a new output starts from here
					while (!soundRevive && !fQuit && !soundStop
						&& micros != AV_NOPTS_VALUE
						&& micros - fLiveBase > _LiveClock() + 100000)
						snooze_until(system_time() + 10000, B_SYSTEM_TIMEBASE);
					if (!soundRevive || !_LiveReviveSound(micros)) {
						soundRevive = false;
						av_frame_unref(got);
						continue;
					}
					soundRevive = false;
					soundBackAt = system_time();
					soundDead = false;
				}
				_LiveAlignSound(micros);
				_LivePushSound(got);
				av_frame_unref(got);
				// waiting for room in the ring, which the sound player
				// empties as it plays (not while paused)
				size_t offset = 0;
				while (offset < fLivePcm.size() && !fQuit && !soundStop) {
					size_t room;
					{
						BAutolock ring(fRingLock);
						room = fRing.size() - fRingFill;
					}
					room -= room % fFrameSize;
					if (room == 0) {
						if (soundDead)
							break;
						snooze_until(system_time() + 5000, B_SYSTEM_TIMEBASE);
						continue;
					}
					size_t bytes = std::min(room, fLivePcm.size() - offset);
					_LiveQueueSound(fLivePcm.data() + offset, bytes);
					offset += bytes;
				}
				fLivePcm.clear();
			}
		}
		av_frame_free(&got);
		soundDone = true;
	};
	std::thread sound;
	if (audio >= 0 && !failed) {
		try {
			sound = std::thread(soundLoop);
		} catch (...) {
			failed = true;
			soundDone = true;
		}
	}

	// a file read from the network (a server of the home network, the
	// web): some seconds of it first, so that a network barely fast enough
	// does not stop the pictures at once; not the relay or a torrent of
	// the helper, which wait for their pieces themselves, nor live TV
	if (!fLive && !fRelay && fAddress.IStartsWith("http")
		&& !fAddress.StartsWith("http://localhost:")) {
		double perSecond = audio >= 0 ? 50 : 0;
		if (video >= 0) {
			AVRational rate = { fLiveVideoRate[0], fLiveVideoRate[1] };
			perSecond += rate.num > 0 && rate.den > 0
				? std::min(120.0, av_q2d(rate)) : 30;
		}
		size_t wanted = std::min((size_t)(perSecond * 5),
			maxReadPackets * 9 / 10);
		bigtime_t until = system_time() + 10000000;
		while (!fQuit && system_time() < until) {
			{
				std::lock_guard<std::mutex> locker(readLock);
				if (readEnded || read.size() >= wanted)
					break;
			}
			snooze_until(system_time() + 50000, B_SYSTEM_TIMEBASE);
		}
	}

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
			// the end: what is still there. An empty packet: the decoder
			// gives the pictures it still holds, the last ones of the video
			if (video >= 0) {
				if (AVPacket* flush = av_packet_alloc())
					packets.push_back(flush);
			}
			while (!fQuit && !failed && (!packets.empty() || !frames.empty())) {
				if (!pumpVideo(true) || !showFrames(true))
					failed = true;
			}
			// the sound all in the ring, then the clock goes on without it
			while (!fQuit && !failed && !soundDone && !soundDead)
				snooze_until(system_time() + 10000, B_SYSTEM_TIMEBASE);
			fAudioEnded = true;
			break;
		}

		if (packet->stream_index == video && video >= 0) {
			if (AVPacket* copy = av_packet_clone(packet))
				packets.push_back(copy);
			av_packet_unref(packet);
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
				lateCount, audioFrames.load(), fLiveSkipLevel, _LiveClock() / 1e6,
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

		if (system_time() >= nextStats) {
			bigtime_t now = system_time(), team, system;
			int32 cpus;
			_CpuTimes(team, system, cpus);
			double capacity = std::max((bigtime_t)1, now - statsCpuAt)
				* (double)std::max((int32)1, cpus);
			PlayerStatistics stats;
			if (fLiveVideo != NULL) {
				stats.width = fLiveVideo->width;
				stats.height = fLiveVideo->height;
				stats.videoCodec = avcodec_get_name(fLiveVideo->codec_id);
			}
			if (fLiveAudio != NULL) {
				stats.audioCodec = avcodec_get_name(fLiveAudio->codec_id);
				stats.sampleRate = fLiveAudio->sample_rate;
				stats.channels = fLiveAudio->ch_layout.nb_channels;
			}
			stats.fps = shownSecond * 1e6f / std::max((bigtime_t)1,
				now - (nextStats - 1000000));
			stats.bitRate = format->bit_rate > 0 ? format->bit_rate : 0;
			stats.dropped = droppedAll;
			stats.skipLevel = fLiveSkipLevel;
			{
				BAutolock ring(fRingLock);
				stats.soundAhead = fFrameSize > 0 && fSound.frame_rate > 0
					? fRingFill / (double)fFrameSize / fSound.frame_rate : 0;
			}
			stats.overlay = fOverlay != NULL;
			stats.cinemaLoad = 100.0 * (team - statsTeamCpu) / capacity;
			stats.systemLoad = 100.0 * (system - statsSystemCpu) / capacity;
			stats.gainDB = fLoudness.GainDB();
			stats.evenVolume = fLoudness.IsEnabled();
			stats.source = fLive ? "stream" : fRelay ? "relay"
				: fAddress.StartsWith("http://localhost:") ? "torrent"
				: fAddress.ByteAt(0) == '/' ? "file" : "web";
			{
				BAutolock locker(fStatsLock);
				fStats = stats;
				fHasStats = true;
			}
			statsCpuAt = now;
			statsTeamCpu = team;
			statsSystemCpu = system;
			shownSecond = 0;
			nextStats = now + 1000000;
		}

		// the sound of the file over before its pictures: their queue full
		if (started && video >= 0 && queuedSpan() >= maxQueuedVideo)
			soundOver();

		// a processor that cannot keep up: the sound runs out while the
		// pictures are decoded
		if (started && video >= 0 && audio >= 0 && !fAudioEnded)
			_LiveLoad();

		// the clock still for two seconds, not paused, with sound waiting
		// in the ring: the output no longer takes it. The clock follows
		// what is heard, so the pictures would stop for good; they go on
		// by the time instead, without the sound
		if (audio >= 0 && soundDead && !soundRevive
			&& system_time() - stillSince > 3000000
			&& system_time() - lastRevive > 3000000
			&& BMediaRoster::IsRunning()) {
			lastRevive = system_time();
			soundRevive = true;
		}
		if (audio >= 0 && !soundDead && !fClockFree
			&& system_time() - soundBackAt > 3000000) {
			bigtime_t clockNow = _LiveClock();
			bigtime_t now = system_time();
			size_t fill;
			{
				BAutolock ring(fRingLock);
				fill = fRingFill;
			}
			if (fPaused || clockNow != stillClock
				|| fill < (size_t)fFrameSize * (size_t)fSound.frame_rate / 2) {
				if (stillClock >= 0 && clockNow != stillClock)
					clockMoved = true;
				stillClock = clockNow;
				stillSince = now;
			} else if (clockMoved && now - stillSince > 2000000) {
				if (debug)
					fprintf(stderr, "live: the sound output stopped at %.2f: "
						"the pictures go on without it\n", clockNow / 1e6);
				soundDead = true;
				_FreeClock();
			}
		}

		// without sound nothing else paces the reading
		if (!failed && video >= 0 && frames.size() >= mostFrames()
			&& queuedSpan() >= maxQueuedVideo && !showFrames(true))
			failed = true;
	}

	fLiveStopRead = true;
	soundStop = true;
	soundCome.notify_all();
	if (reader.joinable())
		reader.join();
	if (sound.joinable())
		sound.join();
	for (AVPacket* queued : read)
		av_packet_free(&queued);
	for (AVPacket* queued : soundRead)
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
	// (the relay ends its stream without saying why)
	bool broken = (readFailed || fRelay) && !fLive && fAddress.ByteAt(0) != '/'
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
