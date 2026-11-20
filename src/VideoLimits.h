/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef VIDEO_LIMITS_H
#define VIDEO_LIMITS_H


#include <OS.h>
#include <Screen.h>
#include <SupportDefs.h>

#include <stdlib.h>


static inline int32
VideoHeightLimit()
{
	// above 1080p YouTube only has VP9, decoded by the processor: 1440p and
	// 2160p for four of them or more, on a screen as high
	int32 limit = 1080;
	system_info info;
	if (get_system_info(&info) == B_OK && info.cpu_count >= 4)
		limit = 2160;
#if defined(__arm__) && !defined(__aarch64__)
	limit = 480;
#endif
	BScreen screen(B_MAIN_SCREEN_ID);
	if (screen.IsValid()) {
		// a tenth above the screen still: 1080p on 1050 lines is scaled by
		// 3%, while the next quality down would be a whole step worse
		int32 screenHeight = (int32)screen.Frame().Height() + 1;
		int32 fits = screenHeight + screenHeight / 10;
		if (screenHeight > 0 && fits < limit)
			limit = fits;
	}
	const char* wanted = getenv("CINEMA_MAX_HEIGHT");
	if (wanted != NULL) {
		int32 height = atoi(wanted);
		if (height > 0 && height < limit)
			limit = height;
	}
	return limit;
}


static inline int32
LimitVideoHeight(int32 height)
{
	if (height <= 0)
		return height;
	int32 limit = VideoHeightLimit();
	return height < limit ? height : limit;
}


#endif	// VIDEO_LIMITS_H
