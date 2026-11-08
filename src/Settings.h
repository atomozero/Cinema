/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef SETTINGS_H
#define SETTINGS_H


#include <String.h>


/*!	What the settings window changes, kept in the settings folder
	(Cinema/settings). The key of TMDB has a file of its own
	(Cinema/tmdb_key), which the helper reads; the folders of the library
	are the local tabs of the categories file.
*/
struct Settings {
	bool		playInCinema;	// or in MediaPlayer
	bool		resume;			// from where it was left
	BString		searchSource;	// the provider searched first, or empty
	int32		videoHeight;	// the highest picture wanted, 0 for what
								// suits the machine
	bool		tvOffered;		// the free channels of the country were
								// given a tab once
	float		volume;			// of the player, 0 to 1, kept between runs
	BString		subtitleLanguage;	// as last chosen: a code, "off", or
								// empty for the languages of the system
	bool		evenVolume;		// films and TV brought to the level of
								// music, without jumps (Loudness)
	bool		overlay;		// the video overlay of the graphics card,
								// where it has one: off where it shows
								// only its key color, green

				Settings() : playInCinema(true), resume(true),
					videoHeight(0), tvOffered(false), volume(1.0f),
					evenVolume(true), overlay(true) {}

	static	Settings	Load();
			void		Save() const;

	static	BString		TmdbKey();
	static	void		SetTmdbKey(const char* key);
};


#endif	// SETTINGS_H
