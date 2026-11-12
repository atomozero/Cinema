/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef SCRIPTING_H
#define SCRIPTING_H


#include <PropertyInfo.h>

#include <string.h>


/*!	What Cinema answers to scripting (hey, or Pippo and its assistant), on
	the application, all DIRECT, the values as JSON text:

	get Downloads		the videos downloaded: [{"path", "id", "provider",
						"kind", "title", "channel", "duration", "size",
						"uploading"}, ...]
	get NetworkFolder	the network folder of the settings, "" if none
	set Move to '{"path": "...", "to": "..."}'
						a download renamed or moved: "to" is the new path,
						or a folder (ending with /) to keep its name; a path
						not starting with / is in ~/Cinema. Its folders are
						made. The answer: {"path": the new one}
	set Upload to '{"path": "...", "move": false}'
						a download copied (or moved) to the network folder;
						it goes on behind, "uploading" in Downloads says how
						far, a notification when it is done
	execute Rescan		the Downloads tab read again

	An error is answered with "error" (B_ERROR) and "message".
*/
static const char* kCinemaSuite = "suite/vnd.atomozero-Cinema";

static const property_info kCinemaProperties[] = {
	{ "Downloads", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The downloaded videos, as a JSON array.", 0, { B_STRING_TYPE } },
	{ "NetworkFolder", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The network folder of the settings.", 0, { B_STRING_TYPE } },
	{ "Move", { B_SET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Renames or moves a download: {\"path\", \"to\"} as JSON.", 0,
		{ B_STRING_TYPE } },
	{ "Upload", { B_SET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Copies a download to the network folder: {\"path\", \"move\"} as "
		"JSON.", 0, { B_STRING_TYPE } },
	{ "Rescan", { B_EXECUTE_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Reads the downloads again.", 0, {} },
	{ 0 }
};


static inline bool
is_cinema_property(const char* name)
{
	for (int32 i = 0; kCinemaProperties[i].name != NULL; i++) {
		if (strcmp(kCinemaProperties[i].name, name) == 0)
			return true;
	}
	return false;
}


#endif	// SCRIPTING_H
