/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMING_H
#define TIMING_H


#include <OS.h>

#include <stdio.h>
#include <stdlib.h>


/*!	CINEMA_TIMING=1: when each step of the start and of a video happens, in
	seconds since Cinema started, on stderr. What is measured to make it
	quicker.
*/
extern bigtime_t gTimingStart;
	// when Cinema started (App.cpp): one for all the files

static inline void
TimingMark(const char* what, const char* detail = "")
{
	static const bool on = getenv("CINEMA_TIMING") != NULL;
	if (on)
		fprintf(stderr, "timing %8.3f  %s %s\n",
			(system_time() - gTimingStart) / 1e6, what, detail);
}


#endif	// TIMING_H
