/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Subtitles.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <Autolock.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <Language.h>
#include <LocaleRoster.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <set>
#include <utility>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Subtitles"


static const bigtime_t kMinimumCue = 500000;
	// a cue shorter than this is kept this long
static const off_t kMaxSubtitleFile = 32 * 1024 * 1024;
	// a file bigger than this is not a subtitle


/*!	The language code of a token of a file name or of a container, as
	"it": two letters, three letters, or a name.
*/
struct LanguageCode {
	const char*	code;
	const char*	iso;
};


static const LanguageCode kLanguageCodes[] = {
	{ "af", "af" }, { "afr", "af" }, { "afrikaans", "af" },
	{ "ar", "ar" }, { "ara", "ar" }, { "arabic", "ar" },
	{ "bg", "bg" }, { "bul", "bg" }, { "bulgarian", "bg" },
	{ "ca", "ca" }, { "cat", "ca" }, { "catalan", "ca" },
	{ "cs", "cs" }, { "ces", "cs" }, { "cze", "cs" }, { "czech", "cs" },
	{ "da", "da" }, { "dan", "da" }, { "danish", "da" },
	{ "de", "de" }, { "deu", "de" }, { "ger", "de" }, { "german", "de" },
	{ "el", "el" }, { "ell", "el" }, { "gre", "el" }, { "greek", "el" },
	{ "en", "en" }, { "eng", "en" }, { "english", "en" },
	{ "es", "es" }, { "spa", "es" }, { "spanish", "es" },
	{ "et", "et" }, { "est", "et" }, { "estonian", "et" },
	{ "fa", "fa" }, { "fas", "fa" }, { "per", "fa" }, { "persian", "fa" },
	{ "fi", "fi" }, { "fin", "fi" }, { "finnish", "fi" },
	{ "fr", "fr" }, { "fra", "fr" }, { "fre", "fr" }, { "french", "fr" },
	{ "he", "he" }, { "heb", "he" }, { "hebrew", "he" },
	{ "hi", "hi" }, { "hin", "hi" }, { "hindi", "hi" },
	{ "hr", "hr" }, { "hrv", "hr" }, { "croatian", "hr" },
	{ "hu", "hu" }, { "hun", "hu" }, { "hungarian", "hu" },
	{ "id", "id" }, { "ind", "id" }, { "indonesian", "id" },
	{ "it", "it" }, { "ita", "it" }, { "italian", "it" },
	{ "italiano", "it" },
	{ "ja", "ja" }, { "jpn", "ja" }, { "japanese", "ja" },
	{ "ko", "ko" }, { "kor", "ko" }, { "korean", "ko" },
	{ "lt", "lt" }, { "lit", "lt" }, { "lithuanian", "lt" },
	{ "lv", "lv" }, { "lav", "lv" }, { "latvian", "lv" },
	{ "nb", "nb" }, { "nob", "nb" }, { "norwegian", "nb" },
	{ "nl", "nl" }, { "nld", "nl" }, { "dut", "nl" }, { "dutch", "nl" },
	{ "no", "no" }, { "nor", "no" },
	{ "pl", "pl" }, { "pol", "pl" }, { "polish", "pl" },
	{ "pt", "pt" }, { "por", "pt" }, { "portuguese", "pt" },
	{ "ro", "ro" }, { "ron", "ro" }, { "rum", "ro" }, { "romanian", "ro" },
	{ "ru", "ru" }, { "rus", "ru" }, { "russian", "ru" },
	{ "sk", "sk" }, { "slk", "sk" }, { "slo", "sk" }, { "slovak", "sk" },
	{ "sl", "sl" }, { "slv", "sl" }, { "slovenian", "sl" },
	{ "sr", "sr" }, { "srp", "sr" }, { "serbian", "sr" },
	{ "sv", "sv" }, { "swe", "sv" }, { "swedish", "sv" },
	{ "th", "th" }, { "tha", "th" }, { "thai", "th" },
	{ "tr", "tr" }, { "tur", "tr" }, { "turkish", "tr" },
	{ "uk", "uk" }, { "ukr", "uk" }, { "ukrainian", "uk" },
	{ "vi", "vi" }, { "vie", "vi" }, { "vietnamese", "vi" },
	{ "zh", "zh" }, { "zho", "zh" }, { "chi", "zh" }, { "chinese", "zh" },
};


static BString
lower(BString text)
{
	text.ToLower();
	return text;
}


static BString
primary(const BString& language)
{
	BString code = lower(language);
	code.Trim();
	int32 cut = code.FindFirst('-');
	if (cut < 0)
		cut = code.FindFirst('_');
	if (cut > 0)
		code.Truncate(cut);
	return code;
}


static BString
code_of(const BString& text)
{
	BString token = lower(text);
	token.Trim();
	if (token.IsEmpty())
		return "";
	for (const LanguageCode& language : kLanguageCodes) {
		if (token == language.code)
			return language.iso;
	}
	return "";
}


static bool
same_language(const BString& one, const BString& other)
{
	BString first = code_of(primary(one));
	if (first.IsEmpty())
		first = primary(one);
	BString second = code_of(primary(other));
	if (second.IsEmpty())
		second = primary(other);
	if (first.IsEmpty() || second.IsEmpty())
		return false;
	return first == second;
}


static BString
language_name(const BString& code)
{
	BString name;
	if (!code.IsEmpty()) {
		BLanguage language(code.String());
		if (language.GetName(name) != B_OK || name.IsEmpty()) {
			name = code;
			name.ToUpper();
		}
	}
	return name;
}


static void
tokens_of(const BString& text, std::vector<BString>& tokens)
{
	BString token;
	for (int32 i = 0; i < text.Length(); i++) {
		char c = text[i];
		if (c == '.' || c == '_' || c == '-' || c == ' ' || c == '['
			|| c == ']' || c == '(' || c == ')' || c == '{' || c == '}') {
			if (!token.IsEmpty()) {
				tokens.push_back(token);
				token = "";
			}
		} else
			token << c;
	}
	if (!token.IsEmpty())
		tokens.push_back(token);
}


static BString
language_of_name(const BString& leaf, const BString& videoBase)
{
	BString stem(leaf);
	int32 dot = stem.FindLast('.');
	if (dot > 0)
		stem.Truncate(dot);

	BString rest(stem);
	BString base(videoBase);
	if (!base.IsEmpty()) {
		BString lowerStem = lower(stem);
		BString lowerBase = lower(base);
		if (lowerStem.StartsWith(lowerBase))
			rest = stem.String() + base.Length();
	}
	std::vector<BString> tokens;
	tokens_of(rest, tokens);
	for (const BString& token : tokens) {
		BString code = code_of(token);
		if (!code.IsEmpty())
			return code;
	}

	// no name of the video before it: the last words of the file
	if (rest == stem) {
		tokens.clear();
		tokens_of(stem, tokens);
		int32 first = std::max((int32)0, (int32)tokens.size() - 4);
		for (int32 i = first; i < (int32)tokens.size(); i++) {
			BString code = code_of(tokens[i]);
			if (!code.IsEmpty())
				return code;
		}
	}
	return "";
}


static BString
extension_of(const BString& leaf)
{
	int32 dot = leaf.FindLast('.');
	return dot >= 0 ? lower(BString(leaf.String() + dot)) : "";
}


static bool
is_subtitle_file(const BString& leaf)
{
	BString extension = extension_of(leaf);
	return extension == ".srt" || extension == ".vtt" || extension == ".ass"
		|| extension == ".ssa";
}


static bigtime_t
time_of(const BString& text, bool* ok)
{
	*ok = false;
	BString value(text);
	value.Trim();
	int32 space = value.FindFirst(' ');
	if (space >= 0)
		value.Truncate(space);
	value.ReplaceAll(',', '.');
	if (value.IsEmpty())
		return 0;

	double parts[3];
	int32 count = 0;
	int32 start = 0;
	while (count < 3) {
		int32 colon = value.FindFirst(':', start);
		BString part;
		if (colon < 0)
			part = value.String() + start;
		else
			part = BString(value.String() + start, colon - start);
		part.Trim();
		if (part.IsEmpty())
			return 0;
		parts[count++] = atof(part.String());
		if (colon < 0)
			break;
		start = colon + 1;
	}
	if (count == 0)
		return 0;

	double seconds = parts[count - 1];
	double multiplier = 60;
	for (int32 i = count - 2; i >= 0; i--) {
		seconds += parts[i] * multiplier;
		multiplier *= 60;
	}
	*ok = seconds >= 0;
	return (bigtime_t)(seconds * 1000000 + 0.5);
}


static void
split(const BString& text, char separator, int32 max, std::vector<BString>& out)
{
	int32 start = 0;
	for (int32 i = 0; i < max - 1; i++) {
		int32 found = text.FindFirst(separator, start);
		if (found < 0)
			break;
		out.push_back(BString(text.String() + start, found - start));
		start = found + 1;
	}
	out.push_back(BString(text.String() + start));
}


static BString
clean_text(const char* text)
{
	if (text == NULL)
		return "";
	BString in(text);
	in.ReplaceAll("\r\n", "\n");
	in.ReplaceAll('\r', '\n');

	BString out;
	bool tag = false;
	// once no "}" follows, none is looked for again: a text of many "{"
	// would be searched to its end for each
	bool closes = true;
	for (int32 i = 0; i < in.Length(); i++) {
		char c = in[i];
		if (c == '<') {
			tag = true;
			continue;
		}
		if (c == '>') {
			tag = false;
			continue;
		}
		if (c == '{' && closes) {
			int32 end = in.FindFirst('}', i);
			if (end > i) {
				i = end;
				continue;
			}
			closes = false;
		}
		if (c == '\\' && i + 1 < in.Length()) {
			char next = in[i + 1];
			if (next == 'N' || next == 'n') {
				out << '\n';
				i++;
				continue;
			}
			if (next == 'h') {
				out << ' ';
				i++;
				continue;
			}
		}
		if (!tag)
			out << c;
	}

	out.ReplaceAll("&amp;", "&");
	out.ReplaceAll("&lt;", "<");
	out.ReplaceAll("&gt;", ">");
	out.ReplaceAll("&quot;", "\"");
	out.ReplaceAll("&#39;", "'");
	out.ReplaceAll("&apos;", "'");
	out.ReplaceAll("&nbsp;", " ");

	BString result;
	int32 start = 0;
	while (start <= out.Length()) {
		int32 end = out.FindFirst('\n', start);
		BString line;
		if (end < 0)
			line = out.String() + start;
		else
			line = BString(out.String() + start, end - start);
		line.Trim();
		if (!line.IsEmpty()) {
			if (!result.IsEmpty())
				result << '\n';
			result << line;
		}
		if (end < 0)
			break;
		start = end + 1;
	}
	return result;
}


static BString
ass_text(const char* line)
{
	// FFmpeg 5 and later: "ReadOrder,Layer,Style,Name,MarginL,MarginR,
	// MarginV,Effect,Text"; older ones a whole "Dialogue: Layer,Start,End,
	// Style,Name,MarginL,MarginR,MarginV,Effect,Text". The text, last, may
	// hold commas and colons of its own.
	BString value(line != NULL ? line : "");
	int32 count = 9;
	if (value.StartsWith("Dialogue:")) {
		value.Remove(0, strlen("Dialogue:"));
		count = 10;
	}
	std::vector<BString> fields;
	split(value, ',', count, fields);
	if ((int32)fields.size() == count)
		return clean_text(fields[count - 1].String());
	return clean_text(value.String());
}


static BString
subtitle_text(const AVSubtitle& subtitle)
{
	BString text;
	for (unsigned i = 0; i < subtitle.num_rects; i++) {
		const AVSubtitleRect* rect = subtitle.rects[i];
		if (rect == NULL)
			continue;
		BString part;
		if (rect->type == SUBTITLE_ASS && rect->ass != NULL)
			part = ass_text(rect->ass);
		else if (rect->text != NULL)
			part = clean_text(rect->text);
		if (part.IsEmpty())
			continue;
		if (!text.IsEmpty())
			text << '\n';
		text << part;
	}
	return text;
}


static bool
text_subtitle_codec(AVCodecID codec)
{
	switch (codec) {
		case AV_CODEC_ID_DVD_SUBTITLE:
		case AV_CODEC_ID_DVB_SUBTITLE:
		case AV_CODEC_ID_XSUB:
		case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
		case AV_CODEC_ID_DVB_TELETEXT:
		case AV_CODEC_ID_ARIB_CAPTION:
			return false;
		default:
			return true;
	}
}


Subtitles::Subtitles()
	:
	fLock("subtitles"),
	fSelected(-1),
	fChosen(false),
	fStop(false),
	fThread(-1)
{
	BMessage languages;
	const char* language;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&languages) == B_OK) {
		for (int32 i = 0;
			languages.FindString("language", i, &language) == B_OK; i++) {
			if (language != NULL && language[0] != '\0')
				fPreferred.push_back(primary(language));
		}
	}
}


Subtitles::~Subtitles()
{
	Clear();
}


void
Subtitles::Clear()
{
	StopEmbedded();
	BAutolock locker(fLock);
	fTracks.clear();
	fSelected = -1;
	fChosen = false;
}


int32
Subtitles::Count() const
{
	BAutolock locker(fLock);
	return fTracks.size();
}


bool
Subtitles::TrackAt(int32 index, SubtitleTrackInfo* info) const
{
	BAutolock locker(fLock);
	if (index < 0 || index >= (int32)fTracks.size())
		return false;
	if (info != NULL) {
		info->language = fTracks[index].language;
		info->name = fTracks[index].name;
		info->automatic = fTracks[index].automatic;
		info->loaded = fTracks[index].loaded;
	}
	return true;
}


int32
Subtitles::AddTrack(const char* language, const char* name, bool automatic,
	bool loaded)
{
	BAutolock locker(fLock);
	Track track;
	track.language = language != NULL ? language : "";
	track.name = name != NULL ? name : "";
	track.automatic = automatic;
	track.loaded = loaded;
	if (track.name.IsEmpty())
		track.name = language_name(track.language);
	fTracks.push_back(track);
	int32 index = fTracks.size() - 1;
	if (!fChosen)
		_SelectPreferred();
	return index;
}


int32
Subtitles::AddText(const char* language, const char* name, const char* text,
	bool automatic)
{
	std::vector<Cue> cues;
	_Parse(text, cues);

	BAutolock locker(fLock);
	Track track;
	track.language = language != NULL ? language : "";
	track.name = name != NULL ? name : "";
	track.automatic = automatic;
	track.loaded = true;
	track.cues = std::move(cues);
	_Index(track.cues, track.maxEnd);
	if (track.name.IsEmpty())
		track.name = language_name(track.language);
	fTracks.push_back(track);
	int32 index = fTracks.size() - 1;
	if (!fChosen)
		_SelectPreferred();
	return index;
}


bool
Subtitles::Load(const char* language, bool automatic, const char* text)
{
	std::vector<Cue> cues;
	_Parse(text, cues);

	BAutolock locker(fLock);
	int32 index = -1;
	for (int32 i = 0; i < (int32)fTracks.size(); i++) {
		if (!fTracks[i].automatic == !automatic && !fTracks[i].loaded
			&& same_language(fTracks[i].language,
				language != NULL ? language : "")) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		Track track;
		track.language = language != NULL ? language : "";
		track.name = language_name(track.language);
		if (track.name.IsEmpty())
			track.name = B_TRANSLATE("Subtitles");
		track.automatic = automatic;
		track.loaded = true;
		fTracks.push_back(track);
		index = fTracks.size() - 1;
	}
	fTracks[index].cues = std::move(cues);
	_Index(fTracks[index].cues, fTracks[index].maxEnd);
	fTracks[index].loaded = true;
	if (!fChosen)
		_SelectPreferred();
	return index >= 0;
}


int32
Subtitles::Selected() const
{
	BAutolock locker(fLock);
	return fSelected;
}


void
Subtitles::Select(int32 index)
{
	BAutolock locker(fLock);
	fSelected = index >= 0 && index < (int32)fTracks.size() ? index : -1;
	fChosen = true;
}


void
Subtitles::SetPreference(const char* language)
{
	BAutolock locker(fLock);
	fPreference = language != NULL ? language : "";
}


BString
Subtitles::Choice() const
{
	BAutolock locker(fLock);
	if (fSelected < 0 || fSelected >= (int32)fTracks.size())
		return "off";
	return primary(fTracks[fSelected].language);
}


void
Subtitles::Toggle()
{
	BAutolock locker(fLock);
	if (fSelected >= 0) {
		fSelected = -1;
		fChosen = true;
		return;
	}
	fChosen = false;
	_SelectPreferred();
	if (fSelected < 0 && !fTracks.empty()) {
		fSelected = 0;
		fChosen = true;
	}
}


BString
Subtitles::TextAt(bigtime_t time) const
{
	BAutolock locker(fLock);
	if (fSelected < 0 || fSelected >= (int32)fTracks.size())
		return "";
	const Track& track = fTracks[fSelected];
	BString text;
	const std::vector<Cue>& cues = track.cues;
	if (track.maxEnd.size() != cues.size()) {
		for (const Cue& cue : cues) {
			if (time < cue.start || time >= cue.end)
				continue;
			if (!text.IsEmpty())
				text << '\n';
			text << cue.text;
		}
		return text;
	}
	auto after = std::upper_bound(cues.begin(), cues.end(), time,
		[](bigtime_t value, const Cue& cue) { return value < cue.start; });
	size_t end = after - cues.begin();
	auto active = std::upper_bound(track.maxEnd.begin(),
		track.maxEnd.begin() + end, time);
	for (size_t i = active - track.maxEnd.begin(); i < end; i++) {
		if (time >= cues[i].end)
			continue;
		if (!text.IsEmpty())
			text << '\n';
		text << cues[i].text;
	}
	return text;
}


void
Subtitles::_SelectPreferred()
{
	if (fChosen)
		return;
	// none, as the last video was left
	if (fPreference == "off") {
		fSelected = -1;
		return;
	}
	int32 selected = -1;
	std::vector<BString> languages;
	if (!fPreference.IsEmpty())
		languages.push_back(fPreference);
	languages.insert(languages.end(), fPreferred.begin(), fPreferred.end());
	for (const BString& preferred : languages) {
		// the subtitles made by hand before the automatic ones
		for (int32 automatic = 0; automatic < 2 && selected < 0;
			automatic++) {
			for (int32 i = 0; i < (int32)fTracks.size(); i++) {
				if (fTracks[i].automatic == (automatic != 0)
					&& same_language(fTracks[i].language, preferred)) {
					selected = i;
					break;
				}
			}
		}
		if (selected >= 0)
			break;
	}
	if (selected < 0 && fTracks.size() == 1
		&& fTracks[0].language.IsEmpty())
		selected = 0;
	fSelected = selected;
}


void
Subtitles::_AddCues(int32 index, const std::vector<Cue>& cues)
{
	BAutolock locker(fLock);
	if (index < 0 || index >= (int32)fTracks.size())
		return;
	std::vector<Cue>& track = fTracks[index].cues;
	std::vector<bigtime_t>& maxEnd = fTracks[index].maxEnd;
	bool ordered = track.empty() || cues.empty()
		|| track.back().start <= cues.front().start;
	track.insert(track.end(), cues.begin(), cues.end());
	if (ordered) {
		for (const Cue& cue : cues)
			maxEnd.push_back(maxEnd.empty() ? cue.end
				: std::max(maxEnd.back(), cue.end));
	} else {
		std::sort(track.begin(), track.end(),
			[](const Cue& a, const Cue& b) { return a.start < b.start; });
		_Index(track, maxEnd);
	}
}


void
Subtitles::_Parse(const char* text, std::vector<Cue>& cues)
{
	if (text == NULL)
		return;
	BString input(text);
	input.ReplaceAll("\r\n", "\n");
	input.ReplaceAll('\r', '\n');

	if (input.FindFirst("[Events]") >= 0
		|| input.FindFirst("Dialogue:") >= 0) {
		// ASS/SSA
		std::vector<BString> lines;
		split(input, '\n', input.Length() + 1, lines);
		bool events = false;
		int32 startIndex = 1;
		int32 endIndex = 2;
		int32 textIndex = 9;
		for (const BString& raw : lines) {
			BString line(raw);
			line.Trim();
			if (line.IsEmpty())
				continue;
			if (line.StartsWith("[") && line.EndsWith("]")) {
				events = line == "[Events]";
				continue;
			}
			if (!events)
				continue;
			if (line.StartsWith("Format:")) {
				std::vector<BString> fields;
				split(BString(line.String() + 7), ',', 32, fields);
				for (int32 i = 0; i < (int32)fields.size(); i++) {
					BString field(fields[i]);
					field.Trim();
					if (field == "Start")
						startIndex = i;
					else if (field == "End")
						endIndex = i;
					else if (field == "Text")
						textIndex = i;
				}
				continue;
			}
			if (!line.StartsWith("Dialogue:"))
				continue;
			std::vector<BString> fields;
			split(BString(line.String() + 9), ',', textIndex + 1, fields);
			if (startIndex >= (int32)fields.size()
				|| endIndex >= (int32)fields.size()
				|| textIndex >= (int32)fields.size())
				continue;
			bool startOk, endOk;
			bigtime_t start = time_of(fields[startIndex], &startOk);
			bigtime_t end = time_of(fields[endIndex], &endOk);
			BString content = clean_text(fields[textIndex].String());
			if (!startOk || !endOk || content.IsEmpty())
				continue;
			if (end <= start)
				end = start + kMinimumCue;
			cues.push_back({ start, end, content });
		}
		std::sort(cues.begin(), cues.end(),
			[](const Cue& a, const Cue& b) { return a.start < b.start; });
		return;
	}

	// SRT and WebVTT
	std::vector<BString> lines;
	split(input, '\n', input.Length() + 1, lines);
	for (int32 i = 0; i < (int32)lines.size(); ) {
		BString line(lines[i]);
		line.Trim();
		if (line.IsEmpty() || line.StartsWith("WEBVTT")
			|| line.StartsWith("NOTE") || line.StartsWith("STYLE")
			|| line.StartsWith("REGION")) {
			i++;
			continue;
		}
		if (line.FindFirst("-->") < 0) {
			// the number of a cue of SRT, or its identifier in VTT
			if (i + 1 < (int32)lines.size()
				&& lines[i + 1].FindFirst("-->") >= 0) {
				i++;
				line = lines[i];
				line.Trim();
			} else {
				i++;
				continue;
			}
		}

		int32 arrow = line.FindFirst("-->");
		BString from(line.String(), arrow);
		BString to(line.String() + arrow + 3);
		bool startOk, endOk;
		bigtime_t start = time_of(from, &startOk);
		bigtime_t end = time_of(to, &endOk);
		i++;

		BString body;
		while (i < (int32)lines.size()) {
			BString next(lines[i]);
			BString trimmed(next);
			trimmed.Trim();
			if (trimmed.IsEmpty()) {
				i++;
				break;
			}
			if (trimmed.FindFirst("-->") >= 0)
				break;
			if (!body.IsEmpty())
				body << '\n';
			body << next;
			i++;
		}
		BString content = clean_text(body.String());
		if (!startOk || !endOk || content.IsEmpty())
			continue;
		if (end <= start)
			end = start + kMinimumCue;
		cues.push_back({ start, end, content });
	}
	std::sort(cues.begin(), cues.end(),
		[](const Cue& a, const Cue& b) { return a.start < b.start; });
}


void
Subtitles::_Index(std::vector<Cue>& cues, std::vector<bigtime_t>& maxEnd)
{
	maxEnd.clear();
	maxEnd.reserve(cues.size());
	for (const Cue& cue : cues)
		maxEnd.push_back(maxEnd.empty() ? cue.end
			: std::max(maxEnd.back(), cue.end));
}


void
Subtitles::_LoadFile(const char* path, const BString& videoBase)
{
	BFile file(path, B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return;
	off_t size = 0;
	if (file.GetSize(&size) != B_OK || size <= 0 || size > kMaxSubtitleFile)
		return;

	char* buffer = (char*)malloc(size + 1);
	if (buffer == NULL)
		return;
	ssize_t read = file.ReadAt(0, buffer, size);
	if (read != size) {
		free(buffer);
		return;
	}
	buffer[size] = '\0';

	BString leaf(path);
	int32 slash = leaf.FindLast('/');
	if (slash >= 0)
		leaf = leaf.String() + slash + 1;
	BString language = language_of_name(leaf, videoBase);
	BString name = language_name(language);
	if (name.IsEmpty()) {
		BString stem(leaf);
		int32 dot = stem.FindLast('.');
		if (dot > 0)
			stem.Truncate(dot);
		if (stem != videoBase)
			name = stem;
		else
			name = B_TRANSLATE("Subtitles");
	}
	AddText(language.String(), name.String(), buffer, false);
	free(buffer);
}


void
Subtitles::_ScanFolder(const char* folder, const BString& videoBase, bool exact,
	int32* added, int32 limit, std::set<BString>& seen)
{
	BDirectory directory(folder);
	if (directory.InitCheck() != B_OK)
		return;

	BString lowerBase = lower(videoBase);
	BEntry entry;
	while (*added < limit && directory.GetNextEntry(&entry, false) == B_OK) {
		if (!entry.IsFile())
			continue;
		BPath path;
		if (entry.GetPath(&path) != B_OK)
			continue;
		BString leaf(path.Leaf());
		if (!is_subtitle_file(leaf))
			continue;
		if (exact) {
			BString lowerLeaf = lower(leaf);
			if (lowerBase.IsEmpty() || !lowerLeaf.StartsWith(lowerBase))
				continue;
			// "movie" is not "movie2"
			if (lowerLeaf.Length() > lowerBase.Length()) {
				char next = lowerLeaf[lowerBase.Length()];
				if (next != '.' && next != '_' && next != '-'
					&& next != ' ')
					continue;
			}
		}
		BString full(path.Path());
		if (!seen.insert(full).second)
			continue;
		_LoadFile(full.String(), videoBase);
		(*added)++;
	}
}


void
Subtitles::LoadSidecars(const char* video)
{
	if (video == NULL || video[0] != '/')
		return;
	BEntry entry(video);
	BPath path;
	if (entry.GetPath(&path) != B_OK)
		path.SetTo(video);
	BPath parent;
	if (path.GetParent(&parent) != B_OK)
		return;

	BString leaf(path.Leaf());
	BString base(leaf);
	int32 dot = base.FindLast('.');
	if (dot > 0)
		base.Truncate(dot);

	std::set<BString> seen;
	int32 added = 0;
	_ScanFolder(parent.Path(), base, true, &added, 24, seen);
	if (added == 0)
		_ScanFolder(parent.Path(), base, false, &added, 24, seen);

	// the folder many releases keep their subtitles in
	static const char* kFolders[] = {
		"Subs", "Subtitles", "subs", "subtitles", "Sub"
	};
	for (const char* name : kFolders) {
		if (added >= 24)
			break;
		BString folder(parent.Path());
		folder << "/" << name;
		_ScanFolder(folder.String(), base, added > 0, &added, 24, seen);
	}
}


namespace {
struct EmbeddedWork {
	Subtitles*	subtitles;
	BString		path;
};
}


status_t
Subtitles::_EmbeddedEntry(void* data)
{
	EmbeddedWork* work = (EmbeddedWork*)data;
	work->subtitles->_Embedded(work->path.String());
	delete work;
	return B_OK;
}


void
Subtitles::StartEmbedded(const char* video)
{
	StopEmbedded();
	if (video == NULL || video[0] != '/')
		return;
	fStop = false;
	EmbeddedWork* work = new EmbeddedWork { this, video };
	// below the pictures and the sound, which open at the same time
	fThread = spawn_thread(&_EmbeddedEntry, "subtitles", B_LOW_PRIORITY,
		work);
	if (fThread < 0)
		delete work;
	else
		resume_thread(fThread);
}


int
Subtitles::_Interrupt(void* data)
{
	return ((Subtitles*)data)->fStop ? 1 : 0;
}


void
Subtitles::StopEmbedded()
{
	if (fThread >= 0) {
		fStop = true;
		status_t status;
		wait_for_thread(fThread, &status);
		fThread = -1;
	}
}


void
Subtitles::_Embedded(const char* video)
{
	// a few seconds after the video, which is opening and seeking: on the
	// AC100 both reading the same file at once slowed the start
	for (int32 i = 0; i < 30 && !fStop; i++)
		snooze(100000);
	if (fStop)
		return;

	// stopped while it opens or probes too: a big file on a slow disk
	// would hold the window that waits for it
	AVFormatContext* format = avformat_alloc_context();
	if (format == NULL)
		return;
	format->interrupt_callback.callback = &_Interrupt;
	format->interrupt_callback.opaque = this;
	if (avformat_open_input(&format, video, NULL, NULL) < 0)
		return;
	// MP4 and Matroska name their streams in the header: probing them
	// would decode pictures for nothing
	const char* container = format->iformat != NULL
		? format->iformat->name : "";
	bool named = strstr(container, "mov") != NULL
		|| strstr(container, "matroska") != NULL;
	if (!named && avformat_find_stream_info(format, NULL) < 0) {
		avformat_close_input(&format);
		return;
	}

	struct EmbeddedStream {
		int					index;
		AVCodecContext*		codec;
		int32				track;
	};
	std::vector<EmbeddedStream> streams;
	for (unsigned i = 0; i < format->nb_streams && !fStop; i++) {
		AVStream* stream = format->streams[i];
		AVCodecParameters* parameters = stream->codecpar;
		if (parameters->codec_type != AVMEDIA_TYPE_SUBTITLE
			|| !text_subtitle_codec(parameters->codec_id))
			continue;
		const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
		if (codec == NULL)
			continue;
		AVCodecContext* decoder = avcodec_alloc_context3(codec);
		if (decoder == NULL)
			continue;
		if (avcodec_parameters_to_context(decoder, parameters) < 0
			|| avcodec_open2(decoder, codec, NULL) < 0) {
			avcodec_free_context(&decoder);
			continue;
		}

		BString language;
		BString name;
		AVDictionaryEntry* value = av_dict_get(stream->metadata, "language",
			NULL, 0);
		if (value != NULL)
			language = code_of(value->value);
		value = av_dict_get(stream->metadata, "title", NULL, 0);
		if (value != NULL)
			name = value->value;
		if (name.IsEmpty())
			name = language_name(language);
		if (name.IsEmpty())
			name = B_TRANSLATE("Subtitles");
		if ((stream->disposition & AV_DISPOSITION_FORCED) != 0)
			name << " (" << B_TRANSLATE("forced") << ")";

		int32 track = AddTrack(language.String(), name.String(), false, true);
		streams.push_back({ (int)i, decoder, track });
	}

	if (streams.empty()) {
		avformat_close_input(&format);
		return;
	}

	// where the file starts, which the player counts from: without
	// avformat_find_stream_info() (skipped for MP4 and Matroska) the format
	// does not say it, so the first packets do, before going back
	int64 origin = format->start_time;
	if (origin == AV_NOPTS_VALUE) {
		AVPacket* first = av_packet_alloc();
		for (int32 i = 0; first != NULL && i < 64 && !fStop
				&& av_read_frame(format, first) >= 0; i++) {
			int64 time = first->pts != AV_NOPTS_VALUE ? first->pts : first->dts;
			if (time != AV_NOPTS_VALUE) {
				time = av_rescale_q(time,
					format->streams[first->stream_index]->time_base,
					AV_TIME_BASE_Q);
				if (origin == AV_NOPTS_VALUE || time < origin)
					origin = time;
			}
			av_packet_unref(first);
		}
		av_packet_free(&first);
		av_seek_frame(format, -1, INT64_MIN, AVSEEK_FLAG_BACKWARD);
	}
	if (origin == AV_NOPTS_VALUE)
		origin = 0;

	for (unsigned i = 0; i < format->nb_streams; i++) {
		bool wanted = false;
		for (const EmbeddedStream& stream : streams) {
			if ((int)i == stream.index) {
				wanted = true;
				break;
			}
		}
		format->streams[i]->discard = wanted
			? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	}

	AVPacket* packet = av_packet_alloc();
	while (!fStop && packet != NULL && av_read_frame(format, packet) >= 0) {
		EmbeddedStream* found = NULL;
		for (EmbeddedStream& stream : streams) {
			if (stream.index == packet->stream_index) {
				found = &stream;
				break;
			}
		}
		if (found != NULL && packet->size > 0) {
			AVSubtitle subtitle;
			memset(&subtitle, 0, sizeof(subtitle));
			int got = 0;
			int used = avcodec_decode_subtitle2(found->codec, &subtitle,
				&got, packet);
			if (used >= 0 && got) {
				int64 time = subtitle.pts;
				if (time == AV_NOPTS_VALUE)
					time = av_rescale_q(packet->pts,
						format->streams[found->index]->time_base,
						AV_TIME_BASE_Q);
				if (time == AV_NOPTS_VALUE)
					time = 0;
				// from the start of the file, as the player counts: an
				// MPEG-TS file starts at 1.4 s or so
				else
					time -= origin;
				bigtime_t start = time > 0 ? time : 0;
				start += (bigtime_t)subtitle.start_display_time * 1000;
				uint32_t shown = std::max<uint32_t>(
					subtitle.end_display_time, 1000);
				bigtime_t end = start + (bigtime_t)shown * 1000;
				BString text = subtitle_text(subtitle);
				static int32 cuesShown = 0;
				if (getenv("CINEMA_PLAYER_DEBUG") != NULL && cuesShown < 5) {
					cuesShown++;
					fprintf(stderr, "subtitles: %.3f \"%s\"\n", start / 1e6,
						text.String());
				}
				if (!text.IsEmpty()) {
					std::vector<Cue> cue = { { start, end, text } };
					_AddCues(found->track, cue);
				}
				avsubtitle_free(&subtitle);
			}
		}
		av_packet_unref(packet);
	}
	if (packet != NULL)
		av_packet_free(&packet);

	for (EmbeddedStream& stream : streams)
		avcodec_free_context(&stream.codec);
	avformat_close_input(&format);
}
