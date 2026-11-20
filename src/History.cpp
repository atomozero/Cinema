/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "History.h"
#include "Pictures.h"
#include "PictureName.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <Bitmap.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <TranslationUtils.h>


namespace History {

static const int32 kMost = 30;


static bool
history_path(BPath& path)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return false;
	path.Append("Cinema");
	create_directory(path.Path(), 0755);
	return path.Append("history") == B_OK;
}


static BString
field(const BString& text)
{
	BString clean(text);
	clean.ReplaceAll("\t", " ");
	clean.ReplaceAll("\n", " ");
	clean.ReplaceAll("\r", " ");
	return clean;
}


static BBitmap*
picture(const BString& id, const char* cache)
{
	if (id.IsEmpty())
		return NULL;
	BString path;
	if (cache != NULL && cache[0] != '\0') {
		path = cache;
		path << "/" << PictureName(id) << ".jpg";
		BBitmap* bitmap = Pictures::Load(path.String());
		if (bitmap != NULL)
			return bitmap;
	}
	BPath home;
	if (find_directory(B_USER_DIRECTORY, &home) != B_OK)
		return NULL;
	path = home.Path();
	path << "/Cinema/.pictures/" << PictureName(id) << ".jpg";
	return Pictures::Load(path.String());
}


static std::vector<MediaItem>
load(const char* cache, int32 pictures = INT32_MAX)
{
	std::vector<MediaItem> items;
	BPath path;
	if (!history_path(path))
		return items;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return items;

	char* line = NULL;
	size_t size = 0;
	while (getline(&line, &size, file) >= 0) {
		BString text(line);
		text.RemoveAll("\n").RemoveAll("\r");
		BString fields[9];
		int32 start = 0;
		for (int32 i = 0; i < 8; i++) {
			int32 tab = text.FindFirst('\t', start);
			if (tab < 0)
				break;
			text.CopyInto(fields[i], start, tab - start);
			start = tab + 1;
		}
		if (!fields[0].IsEmpty())
			text.CopyInto(fields[8], start, text.Length() - start);

		MediaItem item;
		item.id = fields[0];
		item.provider = fields[1].IsEmpty() ? "youtube" : fields[1];
		item.kind = fields[2].IsEmpty() ? "video" : fields[2];
		item.title = fields[3];
		item.channel = fields[4];
		item.channelId = fields[5];
		item.duration = atoi(fields[6].String());
		item.path = fields[7];
		if (item.id.IsEmpty())
			continue;
		if (!item.path.IsEmpty() && !BEntry(item.path.String()).Exists())
			item.path = "";
		item.downloaded = !item.path.IsEmpty();
		// none without a cache: Remember() only rewrites the file; only
		// the first ones, which the shelf shows
		item.picture = cache != NULL && (int32)items.size() < pictures
			? picture(item.id, cache) : NULL;
		items.push_back(item);
		if ((int32)items.size() >= kMost)
			break;
	}
	free(line);
	fclose(file);
	return items;
}


std::vector<MediaItem>
Load(const char* cache, int32 pictures)
{
	return load(cache, pictures);
}


void
Remember(const MediaItem& item, const char* cache)
{
	if (item.id.IsEmpty())
		return;
	BPath path;
	if (!history_path(path))
		return;

	std::vector<MediaItem> items = load(NULL);
	for (size_t i = 0; i < items.size(); i++) {
		if (items[i].id == item.id) {
			items.erase(items.begin() + i);
			break;
		}
	}
	MediaItem recent = item;
	recent.picture = NULL;
	items.insert(items.begin(), recent);
	if ((int32)items.size() > kMost)
		items.resize(kMost);

	BString temporary(path.Path());
	temporary << ".part";
	FILE* file = fopen(temporary.String(), "w");
	if (file == NULL)
		return;
	for (const MediaItem& video : items) {
		fprintf(file, "%s\t%s\t%s\t%s\t%s\t%s\t%" B_PRId32 "\t%s\t%" B_PRId64
			"\n", field(video.id).String(), field(video.provider).String(),
			field(video.kind).String(), field(video.title).String(),
			field(video.channel).String(), field(video.channelId).String(),
			video.duration, field(video.path).String(),
			(int64)time(NULL));
	}
	// on the disk before it takes the place of the old one: a power cut
	// would leave an empty file otherwise
	fflush(file);
	fsync(fileno(file));
	if (fclose(file) == 0)
		rename(temporary.String(), path.Path());
	else
		remove(temporary.String());
	(void)cache;
}

}	// namespace History
