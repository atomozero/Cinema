/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef HISTORY_H
#define HISTORY_H


#include <String.h>

#include <vector>

#include "MediaItem.h"


namespace History {

std::vector<MediaItem>	Load(const char* cache,
							int32 pictures = INT32_MAX);
						// the pictures of the first ones only
void					Remember(const MediaItem& item, const char* cache);

}	// namespace History


#endif	// HISTORY_H
