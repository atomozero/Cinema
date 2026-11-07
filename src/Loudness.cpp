/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Loudness.h"

#include <math.h>

#include <algorithm>


// the level aimed at: about that of mastered music (-14 dBFS RMS)
static const float kTarget = 0.2f;
static const float kMostGain = 5.62f;		// +15 dB
static const float kLeastGain = 0.708f;		// -3 dB
// below this (-50 dBFS) it is silence, or nearly: the gain stays
static const float kGate = 0.00316f;
static const float kLevelTime = 0.3f;		// s, of the level measured
static const float kAttackTime = 0.05f;		// s, the gain going down
static const float kReleaseTime = 1.5f;		// s, the gain going up
// the first seconds of a video go up sooner, so that it does not start
// faint and grow
static const float kStartReleaseTime = 0.3f;
static const float kStartTime = 3.0f;
// above this the peaks are bent towards full scale instead of clipped
static const float kKnee = 0.7f;

// a few milliseconds at a time: the gain follows the sound closely enough
static const int32 kBlockFrames = 256;


static inline int16
limit(float sample)
{
	float size = fabsf(sample);
	if (size > kKnee) {
		// t + (1 - t) z / (1 + z): smooth at the knee, never above 1
		float over = (size - kKnee) / (1.0f - kKnee);
		size = kKnee + (1.0f - kKnee) * over / (1.0f + over);
		sample = sample < 0 ? -size : size;
	}
	int32 value = (int32)lrintf(sample * 32767.0f);
	return (int16)std::max((int32)-32768, std::min((int32)32767, value));
}


Loudness::Loudness()
	:
	fEnabled(true)
{
	Reset();
}


void
Loudness::SetEnabled(bool enabled)
{
	fEnabled = enabled;
}


void
Loudness::Reset()
{
	fPower = 0;
	fGain = 1.0f;
	fElapsed = 0;
	fPrimed = false;
}


void
Loudness::Process(const float* in, int16* out, int32 frames, int32 channels,
	float rate)
{
	if (frames <= 0 || channels <= 0)
		return;
	rate = std::max(rate, 1.0f);
	for (int32 done = 0; done < frames; done += kBlockFrames) {
		int32 count = std::min(kBlockFrames, frames - done);
		_Block(in + done * channels, out + done * channels, count, channels,
			rate);
	}
}


void
Loudness::Process(int16* samples, int32 frames, int32 channels, float rate)
{
	if (frames <= 0 || channels <= 0 || channels > 8)
		return;
	rate = std::max(rate, 1.0f);
	float block[kBlockFrames * 8];
	for (int32 done = 0; done < frames; done += kBlockFrames) {
		int32 count = std::min(kBlockFrames, frames - done);
		int16* at = samples + done * channels;
		for (int32 i = 0; i < count * channels; i++)
			block[i] = at[i] * (1.0f / 32768.0f);
		_Block(block, at, count, channels, rate);
	}
}


void
Loudness::_Block(const float* in, int16* out, int32 frames, int32 channels,
	float rate)
{
	int32 samples = frames * channels;
	float seconds = frames / rate;
	float from = fGain;
	float to = fGain;

	if (fEnabled) {
		float sum = 0;
		for (int32 i = 0; i < samples; i++)
			sum += in[i] * in[i];
		float power = sum / samples;
		if (!fPrimed) {
			fPower = power;
			fPrimed = true;
		} else {
			// a block counts for at most 6 dB above the level: an explosion
			// of a moment barely moves it (the limiter takes it), and does
			// not leave the dialogue after it faint for seconds; a loud
			// scene that lasts still raises it in about half a second
			// (from the level below which the gain is the highest anyway,
			// so that loud music after a silence is followed in 0.4 s)
			float floor = (kTarget / kMostGain) * (kTarget / kMostGain);
			power = std::min(power, std::max(fPower, floor) * 4.0f);
			fPower += (1.0f - expf(-seconds / kLevelTime)) * (power - fPower);
		}

		float level = sqrtf(fPower);
		float wanted = fGain;
		if (level >= kGate) {
			wanted = std::max(kLeastGain, std::min(kMostGain,
				kTarget / level));
		}
		float time = wanted < fGain ? kAttackTime
			: (fElapsed < kStartTime ? kStartReleaseTime : kReleaseTime);
		to = fGain + (1.0f - expf(-seconds / time)) * (wanted - fGain);
		fElapsed += seconds;
	} else
		from = to = 1.0f;
	fGain = to;

	// from one gain to the next over the block, without steps
	float step = (to - from) / frames;
	float gain = from;
	for (int32 frame = 0; frame < frames; frame++) {
		for (int32 c = 0; c < channels; c++) {
			int32 i = frame * channels + c;
			out[i] = limit(in[i] * gain);
		}
		gain += step;
	}
}
