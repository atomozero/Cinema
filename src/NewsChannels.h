/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef NEWS_CHANNELS_H
#define NEWS_CHANNELS_H


#include <LocaleRoster.h>
#include <Message.h>
#include <String.h>

#include <string.h>

#include <vector>


/*!	The news channels of YouTube the News tab can be made of: their latest
	videos together, the newest first, rather than a search. Italian ones,
	checked to be there (with their ids, which do not change); the first
	six are on by default on a system in Italian.
*/
struct NewsChannel {
	const char*	id;
	const char*	name;
	bool		byDefault;
};


static const NewsChannel kNewsChannels[] = {
	{ "UCYj2t05uU65UrlI1f0OGhlg", "TG La7", true },
	{ "UCz6E3lF72mb6uoJ-mOlNo2A", "Sky TG24", true },
	{ "UCxqR9g_1XlnfrqwHK9viwCw", "RaiNews", true },
	{ "UCmoMxFxfjHJIicCv1eV_tSA", "Tgcom24", true },
	{ "UC7dCZ28JQUZYno7TSlLKNoQ", "ANSA", true },
	{ "UCWJhBqnyVdHAuxexgjghNFw", "Fanpage.it", true },
	{ "UC9ePmjVRHLL8x8vq5fOKflg", "La Repubblica", false },
	{ "UCPfFXhCgc6cciu3aYm-9AdA", "Il Fatto Quotidiano", false },
};


/*!	The channels on by default: the Italian ones on a system in Italian,
	none elsewhere (the News tab then searches).
*/
static inline std::vector<BString>
DefaultNewsChannels()
{
	std::vector<BString> channels;
	BMessage languages;
	const char* language;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&languages) != B_OK
		|| languages.FindString("language", &language) != B_OK
		|| strncmp(language, "it", 2) != 0)
		return channels;
	for (const NewsChannel& channel : kNewsChannels) {
		if (channel.byDefault)
			channels.push_back(channel.id);
	}
	return channels;
}


#endif	// NEWS_CHANNELS_H
