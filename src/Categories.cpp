/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Categories.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Catalog.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <LocaleRoster.h>
#include <Message.h>
#include <Path.h>

#include <algorithm>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Categories"


static void
write_examples(const char* path)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	fprintf(file, "%s\n", B_TRANSLATE("# One tab of the top bar per line: "
		"name = what to search on YouTube"));
	fprintf(file, "%s\n", B_TRANSLATE("# or name = channel:<id> for the "
		"latest videos of a channel"));
	fprintf(file, "%s\n", B_TRANSLATE("# or name = @iptv channel:<address "
		"of an M3U list> for TV channels"));
	fprintf(file, "%s = %s\n", B_TRANSLATE("News"),
		B_TRANSLATE("news today"));
	fprintf(file, "%s = %s\n", B_TRANSLATE("Music"),
		B_TRANSLATE("official music video"));
	fprintf(file, "%s = %s\n", B_TRANSLATE("Technology"),
		B_TRANSLATE("technology review"));
	fprintf(file, "%s = %s\n", B_TRANSLATE("Science"),
		B_TRANSLATE("science explained"));
	fprintf(file, "%s = %s\n", B_TRANSLATE("Documentaries"),
		B_TRANSLATE("full documentary"));
	// public domain films and cartoons of the Internet Archive
	fprintf(file, "%s = @archive channel:feature_films\n",
		B_TRANSLATE("Classic films"));
	fprintf(file, "%s = @archive channel:animationandcartoons\n",
		B_TRANSLATE("Classic cartoons"));
	fclose(file);
}


static const char* kChannelPrefix = "channel:";
static const char* kCatalogSignature = "x-vnd.atomozero-Cinema";


struct DefaultCategory {
	const char*	name;
	const char*	query;
};


static const DefaultCategory kDefaultCategories[] = {
	{ "News", "news today" },
	{ "Music", "official music video" },
	{ "Technology", "technology review" },
	{ "Science", "science explained" },
	{ "Documentaries", "full documentary" }
};


static void
localize_defaults(std::vector<Category>& categories)
{
	const int32 count = (int32)(sizeof(kDefaultCategories)
		/ sizeof(kDefaultCategories[0]));
	std::vector<int32> matched(categories.size(), -1);
	bool missing = false;

	for (size_t i = 0; i < categories.size(); i++) {
		const BString& name = categories[i].name;
		const BString& query = categories[i].query;
		for (int32 j = 0; j < count; j++) {
			if ((name == kDefaultCategories[j].name
					&& query == kDefaultCategories[j].query)
				|| (name == B_TRANSLATE(kDefaultCategories[j].name)
					&& query == B_TRANSLATE(kDefaultCategories[j].query))) {
				matched[i] = j;
				break;
			}
		}
		if (matched[i] < 0)
			missing = true;
	}

	// The file may have been written in another language before the user
	// changed the system language: recognize its examples there too.
	if (missing) {
		BMessage languages;
		if (BLocaleRoster::Default()->GetAvailableCatalogs(&languages,
				kCatalogSignature) == B_OK) {
			const char* language;
			for (int32 l = 0;
				languages.FindString("language", l, &language) == B_OK; l++) {
				BCatalog catalog(kCatalogSignature, language);
				for (size_t i = 0; i < categories.size(); i++) {
					if (matched[i] >= 0)
						continue;
					for (int32 j = 0; j < count; j++) {
						const char* translatedName = catalog.GetString(
							kDefaultCategories[j].name,
							B_TRANSLATION_CONTEXT);
						const char* translatedQuery = catalog.GetString(
							kDefaultCategories[j].query,
							B_TRANSLATION_CONTEXT);
						if (translatedName != NULL && translatedQuery != NULL
							&& categories[i].name == translatedName
							&& categories[i].query == translatedQuery) {
							matched[i] = j;
							break;
						}
					}
				}
				if (std::find(matched.begin(), matched.end(), -1)
					== matched.end())
					break;
			}
		}
	}

	for (size_t i = 0; i < categories.size(); i++) {
		if (matched[i] < 0)
			continue;
		// the news: the latest first, unless chosen otherwise
		if (strcmp(kDefaultCategories[matched[i]].name, "News") == 0
			&& categories[i].order.IsEmpty())
			categories[i].order = "newest";
		categories[i].name = B_TRANSLATE(kDefaultCategories[matched[i]].name);
		categories[i].query = B_TRANSLATE(
			kDefaultCategories[matched[i]].query);
	}
}


static bool
categories_path(BPath& path)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return false;
	path.Append("Cinema");
	create_directory(path.Path(), 0755);
	return path.Append("categories") == B_OK;
}


std::vector<Category>
LoadCategories()
{
	std::vector<Category> categories;
	BPath path;
	if (!categories_path(path))
		return categories;

	FILE* file = fopen(path.Path(), "r");
	if (file == NULL) {
		write_examples(path.Path());
		file = fopen(path.Path(), "r");
		if (file == NULL)
			return categories;
	}

	// lines of any length: the address of a TV list may be long
	char* line = NULL;
	size_t size = 0;
	while (getline(&line, &size, file) >= 0) {
		BString text(line);
		text.RemoveAll("\n").RemoveAll("\r");
		if (text.ByteAt(0) == '#')
			continue;
		// a name that starts with # was written after a backslash, so as
		// not to be taken for a comment
		if (text.ByteAt(0) == '\\')
			text.Remove(0, 1);
		int32 equals = text.FindFirst('=');
		if (equals <= 0)
			continue;
		Category category;
		text.CopyInto(category.name, 0, equals);
		text.CopyInto(category.query, equals + 1, text.Length() - equals - 1);
		category.name.Trim();
		category.query.Trim();
		// the order of a search, at the end
		for (const char* order : { "newest", "relevant" }) {
			BString mark;
			mark << "[" << order << "]";
			if (category.query.EndsWith(mark)) {
				category.order = order;
				category.query.Truncate(category.query.Length()
					- mark.Length());
				category.query.Trim();
			}
		}
		if (category.query.ByteAt(0) == '@') {
			// "@archive channel:feature_films": the provider first
			int32 space = category.query.FindFirst(' ');
			if (space < 0)
				continue;
			category.query.CopyInto(category.provider, 1, space - 1);
			category.query.Remove(0, space);
			category.query.Trim();
		}
		if (category.query.StartsWith(kChannelPrefix)) {
			category.query.CopyInto(category.channel, strlen(kChannelPrefix),
				category.query.Length() - strlen(kChannelPrefix));
			category.channel.Trim();
			category.query = "";
			if (category.channel.IsEmpty())
				continue;
		}
		if (!category.name.IsEmpty()
			&& (!category.query.IsEmpty() || category.IsChannel()))
			categories.push_back(category);
	}
	free(line);
	fclose(file);
	localize_defaults(categories);
	return categories;
}


void
SaveCategories(const std::vector<Category>& categories)
{
	BPath path;
	if (!categories_path(path))
		return;

	// the comments stay at the top
	BString comments;
	FILE* file = fopen(path.Path(), "r");
	if (file != NULL) {
		char* line = NULL;
		size_t size = 0;
		while (getline(&line, &size, file) >= 0) {
			if (line[0] == '#')
				comments << line;
		}
		free(line);
		fclose(file);
	}

	BString temporary(path.Path());
	temporary << ".part";
	file = fopen(temporary.String(), "w");
	if (file == NULL)
		return;
	fputs(comments.String(), file);
	for (const Category& category : categories) {
		BString name(category.name);
		name.ReplaceAll("=", "-");
		// one line each, and not a comment
		name.ReplaceAll('\n', ' ').ReplaceAll('\r', ' ');
		if (name.ByteAt(0) == '#' || name.ByteAt(0) == '\\')
			name.Prepend("\\");
		BString query(category.query);
		query.ReplaceAll('\n', ' ').ReplaceAll('\r', ' ');
		BString channel(category.channel);
		channel.ReplaceAll('\n', ' ').ReplaceAll('\r', ' ');
		BString provider;
		if (!category.provider.IsEmpty())
			provider << "@" << category.provider << " ";
		if (category.IsChannel()) {
			fprintf(file, "%s = %s%s%s\n", name.String(), provider.String(),
				kChannelPrefix, channel.String());
		} else {
			BString order;
			if (!category.order.IsEmpty())
				order << " [" << category.order << "]";
			fprintf(file, "%s = %s%s%s\n", name.String(), provider.String(),
				query.String(), order.String());
		}
	}
	// on the disk before it takes the place of the old one: a power cut
	// would leave an empty file otherwise
	fflush(file);
	fsync(fileno(file));
	if (fclose(file) == 0)
		rename(temporary.String(), path.Path());
	else
		remove(temporary.String());
}
