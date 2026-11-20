/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef FEEDS_H
#define FEEDS_H


#include <String.h>


/*!	Whether an address is likely the feed of a podcast: one place for the
	window (the search field, a drop) and the application (arguments).
*/
static inline bool
IsFeedAddress(const BString& text)
{
	if (!text.IStartsWith("http://") && !text.IStartsWith("https://")
		&& !text.IStartsWith("feed:"))
		return false;
	if (text.FindFirst(' ') >= 0 || text.FindFirst('\n') >= 0)
		return false;
	BString lower(text);
	lower.ToLower();
	int32 query = lower.FindFirst('?');
	if (query >= 0)
		lower.Truncate(query);
	return lower.StartsWith("feed:") || lower.EndsWith(".xml")
		|| lower.EndsWith(".rss") || lower.EndsWith(".atom")
		|| lower.FindFirst("/feed") >= 0 || lower.FindFirst("/rss") >= 0
		|| lower.FindFirst("podcast") >= 0;
}


#endif	// FEEDS_H
