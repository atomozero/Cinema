/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef LOUDNESS_H
#define LOUDNESS_H


#include <SupportDefs.h>

#include <atomic>


/*!	Evens the loudness of the sound, cheaply enough for the AC100 (no NEON):
	films and TV are mastered 10 to 15 dB below music, to leave room for their
	peaks, and their dialogue ends up faint.

	A slow automatic gain on the level of the last ~300 ms brings it towards
	the level of music: up to +16.6 dB, down to -3 dB, quick to go down (50 ms),
	slow to go up (1.5 s), held during silence so as not to raise its noise.
	A soft limiter then keeps the peaks from clipping, and the samples become
	16-bit ones with saturation. Turned off, only the limiter and the
	conversion are left.

	One per player, used by the one thread that decodes its sound.
*/
class Loudness {
public:
								Loudness();

			void				SetEnabled(bool enabled);
			bool				IsEnabled() const { return fEnabled; }
			void				Reset();
									// another video: its level is not known
			float				GainDB() const;
			float				LevelDB() const;
									// now, for the reports of the player

			void				Process(const float* in, int16* out,
									int32 frames, int32 channels, float rate);
									// interleaved samples, 1.0 full scale
			void				Process(int16* samples, int32 frames,
									int32 channels, float rate);
									// in place

private:
			void				_Block(const float* in, int16* out,
									int32 frames, int32 channels, float rate);

			std::atomic<bool>	fEnabled;
			float				fPower;		// mean square, smoothed
			float				fGain;		// applied now
			float				fElapsed;	// seconds of sound since Reset()
			bool				fPrimed;
};


#endif	// LOUDNESS_H
