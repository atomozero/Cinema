/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Helper.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <Autolock.h>
#include <LocaleRoster.h>
#include <Message.h>

#include "Deliver.h"
#include "Json.h"
#include "Messages.h"
#include "VideoLimits.h"


Helper::Helper(BMessenger target)
	:
	fTarget(target),
	fLock("helper"),
	fProvider("youtube"),
	fHeight(0),
	fInput(-1),
	fOutput(-1),
	fProcess(-1),
	fReader(-1),
	fReady(false),
	fStopping(false)
{
}


Helper::~Helper()
{
	_Stop();
}


void
Helper::_Stop()
{
	// the reader gives up a message the window does not take any more
	fStopping = true;
	{
		BAutolock locker(fLock);
		if (fInput >= 0)
			close(fInput);
		fInput = -1;
	}
	if (fProcess > 0) {
		// the end of its input: it stops its downloads and its relay, in
		// three seconds at most, and goes
		bool gone = false;
		for (int i = 0; i < 80 && !gone; i++) {
			gone = waitpid(fProcess, NULL, WNOHANG) == fProcess;
			if (!gone)
				snooze(50000);
		}
		if (!gone) {
			kill(fProcess, SIGTERM);
			for (int i = 0; i < 20 && !gone; i++) {
				gone = waitpid(fProcess, NULL, WNOHANG) == fProcess;
				if (!gone)
					snooze(50000);
			}
		}
		if (!gone) {
			kill(fProcess, SIGKILL);
			waitpid(fProcess, NULL, 0);
		}
		fProcess = -1;
	}
	if (fReader >= 0) {
		status_t status;
		wait_for_thread(fReader, &status);
		fReader = -1;
	}
	if (fOutput >= 0)
		close(fOutput);
	fOutput = -1;
}


status_t
Helper::Restart()
{
	_Stop();
	return Start(fScript.String(), fCache.String());
}


status_t
Helper::Start(const char* script, const char* cache)
{
	fScript = script;
	fCache = cache;
	fReady = false;
	fStopping = false;
	// a helper gone would end Cinema at the next request
	signal(SIGPIPE, SIG_IGN);

	int toHelper[2];
	int fromHelper[2];
	if (pipe(toHelper) != 0)
		return B_ERROR;
	if (pipe(fromHelper) != 0) {
		close(toHelper[0]);
		close(toHelper[1]);
		return B_ERROR;
	}

	// the language of the system, for what the sources write: "it",
	// "pt-BR"
	BString language("en");
	BMessage languages;
	const char* preferred;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&languages) == B_OK
		&& languages.FindString("language", &preferred) == B_OK) {
		language = preferred;
		language.ReplaceAll('_', '-');
	}

	fProcess = fork();
	if (fProcess < 0)
		return B_ERROR;
	if (fProcess == 0) {
		dup2(toHelper[0], STDIN_FILENO);
		dup2(fromHelper[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		close(toHelper[0]);
		close(toHelper[1]);
		close(fromHelper[0]);
		close(fromHelper[1]);
		execlp("python3", "python3", script, cache, language.String(),
			(char*)NULL);
		_exit(127);
	}

	close(toHelper[0]);
	close(fromHelper[1]);
	// not for the programs Cinema starts later, such as a second helper
	fcntl(toHelper[1], F_SETFD, FD_CLOEXEC);
	fcntl(fromHelper[0], F_SETFD, FD_CLOEXEC);
	{
		BAutolock locker(fLock);
		fInput = toHelper[1];
	}
	fOutput = fromHelper[0];

	fReader = spawn_thread(&_ReaderEntry, "cinema-helper reader",
		B_NORMAL_PRIORITY, this);
	if (fReader < 0)
		return fReader;
	return resume_thread(fReader);
}


void
Helper::SetProvider(const char* id)
{
	BAutolock locker(fLock);
	if (id != NULL && id[0] != '\0')
		fProvider = id;
}


void
Helper::SetHeight(int32 height)
{
	BAutolock locker(fLock);
	fHeight = LimitVideoHeight(height);
}


BString
Helper::_Provider(const char* provider)
{
	if (provider != NULL && provider[0] != '\0')
		return provider;
	BAutolock locker(fLock);
	return fProvider;
}


void
Helper::Search(int32 generation, int32 count, const char* query,
	const char* provider)
{
	Json request = Json::MakeObject();
	request.Set("type", "search");
	request.Set("tag", (int)generation);
	request.Set("provider", _Provider(provider).String());
	request.Set("count", (int)count);
	request.Set("query", query);
	_Send(request);
}


void
Helper::Channel(int32 generation, int32 count, const char* id,
	const char* provider)
{
	Json request = Json::MakeObject();
	request.Set("type", "channel");
	request.Set("tag", (int)generation);
	request.Set("provider", _Provider(provider).String());
	request.Set("count", (int)count);
	request.Set("channel", id);
	_Send(request);
}


void
Helper::Details(const char* provider, const char* id, const char* kind)
{
	Json request = Json::MakeObject();
	request.Set("type", "details");
	request.Set("provider", _Provider(provider).String());
	request.Set("id", id);
	request.Set("kind", kind != NULL && kind[0] != '\0' ? kind : "video");
	_Send(request);
}


void
Helper::Find(const char* id, const char* provider)
{
	Json request = Json::MakeObject();
	request.Set("type", "find");
	request.Set("provider", _Provider(provider).String());
	request.Set("id", id);
	request.Set("kind", "video");
	request.Set("height", (int)fHeight);
	_Send(request);
}


void
Helper::Subtitles(const char* id, const char* provider, const char* kind)
{
	Json request = Json::MakeObject();
	request.Set("type", "subtitles");
	request.Set("provider", _Provider(provider).String());
	request.Set("id", id);
	request.Set("kind", kind != NULL && kind[0] != '\0' ? kind : "video");
	_Send(request);
}


void
Helper::Subtitle(const char* id, const char* language, bool automatic,
	const char* provider, const char* kind)
{
	Json request = Json::MakeObject();
	request.Set("type", "subtitle");
	request.Set("provider", _Provider(provider).String());
	request.Set("id", id);
	request.Set("kind", kind != NULL && kind[0] != '\0' ? kind : "video");
	request.Set("language", language);
	request.Set("automatic", automatic);
	_Send(request);
}


void
Helper::Download(const char* id, const char* folder, const char* provider)
{
	Json request = Json::MakeObject();
	request.Set("type", "download");
	request.Set("provider", _Provider(provider).String());
	request.Set("id", id);
	request.Set("folder", folder);
	request.Set("height", (int)fHeight);
	_Send(request);
}


void
Helper::CheckTmdb()
{
	Json request = Json::MakeObject();
	request.Set("type", "check");
	request.Set("provider", "library");
	_Send(request);
}


void
Helper::Cancel(const char* id)
{
	Json request = Json::MakeObject();
	request.Set("type", "cancel");
	request.Set("id", id);
	_Send(request);
}


void
Helper::_Send(const Json& request)
{
	std::string line = request.ToString();
	line += '\n';
	BAutolock locker(fLock);
	if (fInput >= 0)
		write(fInput, line.c_str(), line.size());
}


status_t
Helper::_ReaderEntry(void* data)
{
	((Helper*)data)->_Read();
	return B_OK;
}


void
Helper::_Read()
{
	BString pending;
	char buffer[4096];
	ssize_t bytes;
	while ((bytes = read(fOutput, buffer, sizeof(buffer))) > 0
		|| (bytes < 0 && errno == EINTR)) {
		if (bytes < 0)
			continue;
		pending.Append(buffer, bytes);
		int32 end;
		while ((end = pending.FindFirst('\n')) >= 0) {
			BString line;
			pending.MoveInto(line, 0, end + 1);
			line.RemoveLast("\n");
			_Handle(line);
		}
	}
	// it is gone: nothing more will be found
	{
		BAutolock locker(fLock);
		if (fInput >= 0)
			close(fInput);
		fInput = -1;
	}
	// stopped on purpose, to start another: not to be taken for a helper
	// that could not start
	if (!fStopping)
		Deliver(fTarget, kMsgHelperGone, &fStopping);
}


static std::string
text_of(const Json& value, const char* key, const char* fallback)
{
	return value.GetString(key, fallback);
}


/*!	The fields of one MediaItem, as the window reads them back. */
static void
add_item(BMessage& message, const Json& item, const char* fallbackProvider)
{
	message.AddString("provider",
		text_of(item, "provider", fallbackProvider).c_str());
	message.AddString("kind", text_of(item, "kind", "video").c_str());
	message.AddString("id", text_of(item, "id", "").c_str());
	message.AddString("title", text_of(item, "title", "").c_str());
	message.AddString("description",
		text_of(item, "description", "").c_str());
	message.AddString("channel", text_of(item, "channel", "").c_str());
	message.AddString("channel id",
		text_of(item, "channel_id", "").c_str());
	message.AddInt32("duration", item.GetInt("duration", 0));
	message.AddInt32("year", item.GetInt("year", 0));
	message.AddString("date", text_of(item, "date", "").c_str());
	message.AddInt64("views", (int64)item.GetDouble("views", 0));
	message.AddDouble("rating", item.GetDouble("rating", 0));
	message.AddString("poster", text_of(item, "poster", "").c_str());
	message.AddString("thumbnail", text_of(item, "thumbnail", "").c_str());
}


void
Helper::_Handle(const BString& line)
{
	bool ok = false;
	Json answer = Json::Parse(std::string(line.String()), &ok);
	if (!ok)
		return;

	BString type(answer.GetString("type", "").c_str());
	// a copy: the window changes it meanwhile
	BString fallback = _Provider(NULL);

	if (type == "ready") {
		fReady = true;
		return;
	}

	if (type == "providers") {
		BMessage message(kMsgProviders);
		const Json* providers = answer.Find("providers");
		if (providers != NULL && providers->IsArray()) {
			for (const Json& provider : providers->Elements()) {
				BMessage one;
				one.AddString("id", provider.GetString("id", "").c_str());
				one.AddString("name",
					provider.GetString("name", "").c_str());
				const Json* kinds = provider.Find("kinds");
				if (kinds != NULL && kinds->IsArray())
					for (const Json& kind : kinds->Elements())
						one.AddString("kinds", kind.String().c_str());
				message.AddMessage("provider", &one);
			}
		}
		Deliver(fTarget, &message, &fStopping);
		return;
	}

	if (type == "results") {
		BMessage results(kMsgResults);
		results.AddInt32("generation", answer.GetInt("tag", -1));
		const Json* items = answer.Find("items");
		if (items != NULL && items->IsArray())
			for (const Json& item : items->Elements())
				add_item(results, item, fallback.String());
		// a channel: what it is, for the head of its videos
		const Json* about = answer.Find("about");
		if (about != NULL && about->IsObject()) {
			results.AddString("about name",
				about->GetString("name", "").c_str());
			results.AddString("about handle",
				about->GetString("handle", "").c_str());
			results.AddInt64("about followers",
				(int64)about->GetDouble("followers", 0));
			results.AddString("about description",
				about->GetString("description", "").c_str());
			results.AddString("about avatar",
				about->GetString("avatar", "").c_str());
		}
		Deliver(fTarget, &results, &fStopping);
		return;
	}

	if (type == "details") {
		BMessage details(kMsgDetails);
		details.AddString("id", answer.GetString("id", "").c_str());
		const Json* item = answer.Find("item");
		bool found = item != NULL && item->IsObject();
		details.AddBool("found", found);
		if (found)
			add_item(details, *item, fallback.String());
		Deliver(fTarget, &details, &fStopping);
		return;
	}

	if (type == "nothing") {
		BMessage failed(kMsgSearchFailed);
		failed.AddInt32("generation", answer.GetInt("tag", -1));
		Deliver(fTarget, &failed, &fStopping);
		return;
	}

	if (type == "found") {
		BMessage found(kMsgFound);
		found.AddString("id", answer.GetString("id", "").c_str());
		found.AddString("url", answer.GetString("url", "").c_str());
		Deliver(fTarget, &found, &fStopping);
		return;
	}

	if (type == "lost") {
		BMessage lost(kMsgLost);
		lost.AddString("id", answer.GetString("id", "").c_str());
		Deliver(fTarget, &lost, &fStopping);
		return;
	}

	if (type == "subtitles") {
		BMessage subtitles(kMsgSubtitles);
		subtitles.AddString("id", answer.GetString("id", "").c_str());
		const Json* tracks = answer.Find("tracks");
		if (tracks != NULL && tracks->IsArray()) {
			for (const Json& track : tracks->Elements()) {
				BMessage one;
				one.AddString("language",
					track.GetString("language", "").c_str());
				one.AddString("name",
					track.GetString("name", "").c_str());
				one.AddBool("automatic",
					track.GetBool("automatic", false));
				subtitles.AddMessage("track", &one);
			}
		}
		Deliver(fTarget, &subtitles, &fStopping);
		return;
	}

	if (type == "subtitle") {
		BMessage subtitle(kMsgSubtitle);
		subtitle.AddString("id", answer.GetString("id", "").c_str());
		subtitle.AddString("language",
			answer.GetString("language", "").c_str());
		subtitle.AddBool("automatic",
			answer.GetBool("automatic", false));
		subtitle.AddString("text", answer.GetString("text", "").c_str());
		subtitle.AddBool("failed", answer.GetBool("failed", false));
		Deliver(fTarget, &subtitle, &fStopping);
		return;
	}

	if (type == "progress") {
		BMessage progress(kMsgDownloadProgress);
		progress.AddString("id", answer.GetString("id", "").c_str());
		progress.AddInt32("percent", answer.GetInt("percent", 0));
		for (const char* field : { "done", "total", "speed", "eta" })
			progress.AddString(field, answer.GetString(field, "").c_str());
		progress.AddInt32("peers", answer.GetInt("peers", -1));
		Deliver(fTarget, &progress, &fStopping);
		return;
	}

	if (type == "checked") {
		BMessage checked(kMsgTmdbChecked);
		checked.AddBool("ok", answer.GetBool("ok", false));
		Deliver(fTarget, &checked, &fStopping);
		return;
	}

	if (type == "cancelled") {
		BMessage cancelled(kMsgDownloadCancelled);
		cancelled.AddString("id", answer.GetString("id", "").c_str());
		Deliver(fTarget, &cancelled, &fStopping);
		return;
	}

	if (type == "saved") {
		BMessage saved(kMsgDownloaded);
		saved.AddString("id", answer.GetString("id", "").c_str());
		saved.AddString("path", answer.GetString("path", "").c_str());
		Deliver(fTarget, &saved, &fStopping);
		return;
	}

	if (type == "failed") {
		BMessage failed(kMsgDownloadFailed);
		failed.AddString("id", answer.GetString("id", "").c_str());
		Deliver(fTarget, &failed, &fStopping);
	}
}
