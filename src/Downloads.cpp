/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Downloads.h"
#include "Pictures.h"
#include "PictureName.h"

#include <algorithm>
#include <utility>

#include <Bitmap.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Mime.h>
#include <Node.h>
#include <Path.h>
#include <TranslationUtils.h>
#include <TypeConstants.h>
#include <Volume.h>


namespace Downloads {

static const char* kIdAttribute = "Cinema:id";
static const char* kProviderAttribute = "Cinema:provider";
static const char* kKindAttribute = "Cinema:kind";
static const char* kChannelAttribute = "Cinema:channel";
static const char* kChannelIdAttribute = "Cinema:channel_id";
static const char* kDurationAttribute = "Cinema:duration";
static const char* kTitleAttribute = "Media:Title";


BString
Folder()
{
	BPath path;
	if (find_directory(B_USER_DIRECTORY, &path) != B_OK
		|| path.Append("Cinema") != B_OK
		|| create_directory(path.Path(), 0755) != B_OK)
		return "";
	return path.Path();
}


static BString
picture_path(const char* id)
{
	BString path(Folder());
	path << "/.pictures/" << PictureName(id) << ".jpg";
	return path;
}


std::vector<MediaItem>
Load(std::map<BString, BString>* files, int32 pictures)
{
	struct Found {
		MediaItem	video;
		time_t	time;
	};
	std::vector<Found> found;

	// the downloads, the torrents in a folder of their own, and the folders
	// they were put in to keep them in order (Film/..., by hand or by a
	// script): four levels down, the hidden ones (.pictures) left out
	std::vector<std::pair<BString, int32>> folders;
	folders.push_back(std::make_pair(Folder(), 0));
	for (size_t next = 0; next < folders.size(); next++) {
	BDirectory folder(folders[next].first.String());
	int32 depth = folders[next].second;
	BEntry entry;
	while (folder.GetNextEntry(&entry) == B_OK) {
		if (entry.IsDirectory()) {
			char name[B_FILE_NAME_LENGTH];
			if (depth < 4 && entry.GetName(name) == B_OK && name[0] != '.') {
				BPath sub;
				entry.GetPath(&sub);
				folders.push_back(std::make_pair(BString(sub.Path()),
					depth + 1));
			}
			continue;
		}
		BNode node(&entry);
		BString id;
		if (!entry.IsFile() || node.ReadAttrString(kIdAttribute, &id) != B_OK)
			continue;
		BPath path;
		entry.GetPath(&path);

		Found file;
		file.video.id = id;
		file.video.path = path.Path();
		if (node.ReadAttrString(kProviderAttribute, &file.video.provider)
				!= B_OK)
			file.video.provider = "youtube";
		if (node.ReadAttrString(kKindAttribute, &file.video.kind) != B_OK)
			file.video.kind = "video";
		if (node.ReadAttrString(kTitleAttribute, &file.video.title) != B_OK)
			file.video.title = path.Leaf();
		node.ReadAttrString(kChannelAttribute, &file.video.channel);
		node.ReadAttrString(kChannelIdAttribute, &file.video.channelId);
		node.ReadAttr(kDurationAttribute, B_INT32_TYPE, 0,
			&file.video.duration, sizeof(int32));
		file.video.downloaded = true;
		file.time = 0;
		entry.GetModificationTime(&file.time);
		found.push_back(file);
		if (files != NULL)
			(*files)[id] = path.Path();
	}
	}

	std::sort(found.begin(), found.end(),
		[](const Found& a, const Found& b) { return a.time > b.time; });
	std::vector<MediaItem> videos;
	for (Found& file : found) {
		if ((int32)videos.size() < pictures) {
			file.video.picture = Pictures::Load(
				picture_path(file.video.id).String());
		}
		videos.push_back(file.video);
	}
	return videos;
}


void
SetPicture(const char* id, const char* file)
{
	BString to = picture_path(id);
	BString pictures(Folder());
	pictures << "/.pictures";
	create_directory(pictures.String(), 0755);
	BFile source(file, B_READ_ONLY);
	off_t size;
	if (source.InitCheck() != B_OK || source.GetSize(&size) != B_OK
		|| size <= 0 || size > 5 * 1024 * 1024)
		return;
	char* data = new char[size];
	BFile target(to.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (source.Read(data, size) == size)
		target.Write(data, size);
	delete[] data;
}


void
KeepPicture(const char* cache, const char* id)
{
	BString to = picture_path(id);
	if (BEntry(to.String()).Exists())
		return;
	BString pictures(Folder());
	pictures << "/.pictures";
	create_directory(pictures.String(), 0755);

	BString from(cache);
	from << "/" << PictureName(id) << ".jpg";
	BFile source(from.String(), B_READ_ONLY);
	off_t size;
	if (source.InitCheck() != B_OK || source.GetSize(&size) != B_OK
		|| size <= 0 || size > 1024 * 1024)
		return;
	char* data = new char[size];
	BFile target(to.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	bool copied = source.Read(data, size) == size
		&& target.Write(data, size) == size;
	delete[] data;
	if (!copied)
		BEntry(to.String()).Remove();
}


void
Describe(const char* path, const BMessage& video)
{
	BNode node(path);
	if (node.InitCheck() != B_OK)
		return;
	BString text = video.GetString("id", "");
	node.WriteAttrString(kIdAttribute, &text);
	text = video.GetString("provider", "youtube");
	node.WriteAttrString(kProviderAttribute, &text);
	text = video.GetString("kind", "video");
	node.WriteAttrString(kKindAttribute, &text);
	text = video.GetString("title", "");
	node.WriteAttrString(kTitleAttribute, &text);
	text = video.GetString("channel", "");
	node.WriteAttrString(kChannelAttribute, &text);
	text = video.GetString("channel id", "");
	node.WriteAttrString(kChannelIdAttribute, &text);
	int32 duration = video.GetInt32("duration", 0);
	node.WriteAttr(kDurationAttribute, B_INT32_TYPE, 0, &duration,
		sizeof(duration));
	update_mime_info(path, false, true, B_UPDATE_MIME_INFO_NO_FORCE);
}


status_t
MoveToTrash(const char* path)
{
	BEntry entry(path);
	struct stat info;
	status_t status = entry.GetStat(&info);
	if (status != B_OK)
		return status;
	BVolume volume(info.st_dev);
	BPath trash;
	status = find_directory(B_TRASH_DIRECTORY, &trash, true, &volume);
	if (status != B_OK)
		return status;
	BDirectory directory(trash.Path());

	// as Tracker does, so that it can be put back from the Trash
	BNode node(&entry);
	BString original(path);
	node.WriteAttrString("_trk/original_path", &original);
	return entry.MoveTo(&directory, NULL, false);
}

static bool
pending_path(BPath& path)
{
	return find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("Cinema") == B_OK
		&& create_directory(path.Path(), 0755) == B_OK
		&& path.Append("torrents") == B_OK;
}


std::vector<BMessage>
LoadPending()
{
	std::vector<BMessage> downloads;
	BPath path;
	if (!pending_path(path))
		return downloads;
	BFile file(path.Path(), B_READ_ONLY);
	BMessage archive;
	if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK)
		return downloads;
	BMessage download;
	for (int32 i = 0; archive.FindMessage("download", i, &download) == B_OK;
			i++)
		downloads.push_back(download);
	return downloads;
}


void
SavePending(const std::vector<BMessage>& downloads)
{
	BPath path;
	if (!pending_path(path))
		return;
	if (downloads.empty()) {
		BEntry(path.Path()).Remove();
		return;
	}
	BMessage archive;
	for (const BMessage& download : downloads)
		archive.AddMessage("download", &download);
	BString temporary(path.Path());
	temporary << ".part";
	BFile file(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	if (file.InitCheck() != B_OK || archive.Flatten(&file) != B_OK)
		return;
	file.Sync();
	BEntry(temporary.String()).Rename(path.Path(), true);
}

}	// namespace Downloads
