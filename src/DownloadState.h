/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef DOWNLOAD_STATE_H
#define DOWNLOAD_STATE_H


// The states of a download, in its BMessage ("state"), in MediaItem's
// downloadState and in what scripting is told: one spelling for all.

// running now
static const char* const kDownloadActive = "active";
// waiting for its turn
static const char* const kDownloadQueued = "queued";
// stopped by the user, kept
static const char* const kDownloadPaused = "paused";
// asked to stop, the helper not answered yet
static const char* const kDownloadPausing = "pausing";
// to stop once the video it is played from is closed
static const char* const kDownloadPauseLater = "pause later";


#endif	// DOWNLOAD_STATE_H
