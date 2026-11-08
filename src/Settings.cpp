/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Settings.h"

#include <stdio.h>

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>


static bool
settings_path(BPath& path, const char* name)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return false;
	path.Append("Cinema");
	create_directory(path.Path(), 0755);
	return path.Append(name) == B_OK;
}


static void
write_file(const BPath& path, const BMessage* archive, const BString* text)
{
	BString temporary(path.Path());
	temporary << ".part";
	BFile file(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	if (file.InitCheck() != B_OK)
		return;
	status_t status = B_OK;
	if (archive != NULL)
		status = archive->Flatten(&file);
	else if (text != NULL && file.Write(text->String(), text->Length())
			!= text->Length())
		status = B_IO_ERROR;
	file.Sync();
	if (status == B_OK)
		BEntry(temporary.String()).Rename(path.Path(), true);
	else
		BEntry(temporary.String()).Remove();
}


Settings
Settings::Load()
{
	Settings settings;
	BPath path;
	if (!settings_path(path, "settings"))
		return settings;
	BFile file(path.Path(), B_READ_ONLY);
	BMessage archive;
	if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK)
		return settings;
	settings.playInCinema = archive.GetBool("play in cinema", true);
	settings.resume = archive.GetBool("resume", true);
	settings.searchSource = archive.GetString("search source", "");
	settings.videoHeight = archive.GetInt32("video height", 0);
	settings.tvOffered = archive.GetBool("tv offered", false);
	settings.volume = archive.GetFloat("volume", 1.0f);
	settings.subtitleLanguage = archive.GetString("subtitle language", "");
	settings.evenVolume = archive.GetBool("even volume", true);
	// a key of its own since 1.4.3: 1.4.2 saved "overlay" on for all, and
	// x86 is off by default now
	settings.overlay = archive.GetBool("use overlay",
		Settings::OverlayByDefault());
	return settings;
}


void
Settings::Save() const
{
	BPath path;
	if (!settings_path(path, "settings"))
		return;
	BMessage archive;
	archive.AddBool("play in cinema", playInCinema);
	archive.AddBool("resume", resume);
	archive.AddString("search source", searchSource);
	archive.AddInt32("video height", videoHeight);
	archive.AddBool("tv offered", tvOffered);
	archive.AddFloat("volume", volume);
	archive.AddString("subtitle language", subtitleLanguage);
	archive.AddBool("even volume", evenVolume);
	archive.AddBool("use overlay", overlay);
	write_file(path, &archive, NULL);
}


BString
Settings::TmdbKey()
{
	BPath path;
	BString key;
	if (!settings_path(path, "tmdb_key"))
		return key;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return key;
	char line[1024];
	if (fgets(line, sizeof(line), file) != NULL)
		key = line;
	fclose(file);
	key.Trim();
	return key;
}


void
Settings::SetTmdbKey(const char* key)
{
	BPath path;
	if (!settings_path(path, "tmdb_key"))
		return;
	BString text(key);
	text.Trim();
	if (text.IsEmpty()) {
		BEntry(path.Path()).Remove();
		return;
	}
	text << "\n";
	write_file(path, NULL, &text);
}
