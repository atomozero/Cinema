/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef DOWNLOADS_H
#define DOWNLOADS_H


#include <Message.h>
#include <SupportDefs.h>
#include <String.h>

#include <map>
#include <vector>

#include "MediaItem.h"


class BMessage;


/*!	The videos downloaded to watch offline, in the Cinema folder of the home
	folder. What they are (provider, kind, id, title, channel, length) is kept
	in attributes of the files, their pictures in its .pictures folder.
*/
namespace Downloads {

BString				Folder();
						// made if needed; empty if it can't be
std::vector<MediaItem>	Load(std::map<BString, BString>* files,
							int32 pictures = INT32_MAX);
						// the newest first, with their files and pictures;
						// files: their paths, by id. The pictures of the
						// first ones only: decoding them all would hold a
						// slow machine with many files
void				KeepPicture(const char* cache, const char* id);
void				SetPicture(const char* id, const char* file);
						// a picture found for it (a poster, a frame): its
						// own, in place of any before
void				Describe(const char* path, const BMessage& video);
						// "id", "title", "channel", "channel id", "duration"
status_t			MoveToTrash(const char* path);

std::vector<BMessage>	LoadPending();
void				SavePending(const std::vector<BMessage>& downloads);
						// the torrents on their way, kept in the settings to
						// go on with them when Cinema starts again

}	// namespace Downloads


#endif	// DOWNLOADS_H
