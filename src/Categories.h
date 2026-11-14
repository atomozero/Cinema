/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef CATEGORIES_H
#define CATEGORIES_H


#include <String.h>

#include <vector>


struct Category {
	BString	name;		// shown
	BString	provider;	// "archive"..., empty for the default one
	BString	query;		// searched on YouTube
	BString	channel;	// or the id of a channel, whose latest videos are
						// shown
	BString	order;		// of a search: "newest" first, "relevant" first,
						// or "" as the source gives it (the news newest)

	bool	IsChannel() const { return !channel.IsEmpty(); }
};


/*!	The tabs of the top bar, in a text file in the settings folder
	(Cinema/categories), one per line: "Name = search", or
	"Name = channel:<id>" for a channel. "@provider " before them takes
	another source than the default one, as in
	"Films = @archive channel:feature_films". A search may end with
	"[newest]" or "[relevant]", its order. The file is written with a few
	examples the first time.
*/
std::vector<Category>	LoadCategories();
void					SaveCategories(
							const std::vector<Category>& categories);
								// keeps the comments of the file


#endif	// CATEGORIES_H
