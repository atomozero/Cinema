#!/usr/bin/env python3
# Copyright 2026, Andrea Bernardi
# SPDX-License-Identifier: MIT
#
# cinema-helper: keeps the providers loaded for Cinema, which talks to it
# through its standard input and output, one JSON object per line.
#
# A request names a provider and what it wants:
#
#   {"type": "search",   "tag": 1, "provider": "youtube", "count": 24,
#    "query": "...", "order": "newest"}  (order optional: the latest first)
#   {"type": "channel",  "tag": 1, "provider": "youtube", "count": 24,
#    "channel": "UC..."}  (also ":news:<id>,<id>", ":subscriptions"...)
#   {"type": "details",  "provider": "youtube", "id": "...", "kind": "video"}
#   {"type": "find",     "id": "...", "provider": "youtube", "kind": "video"}
#   {"type": "subtitles","id": "...", "provider": "youtube", "kind": "video"}
#   {"type": "subtitle", "id": "...", "provider": "youtube", "kind": "video",
#    "language": "it", "automatic": false}
#   {"type": "download", "id": "...", "provider": "youtube", "folder": "/..."}
#   {"type": "pause", "id": "..."}, {"type": "cancel", "id": "..."}: a
#   download (or a copy to the network folder) stopped, what it got kept or
#   not; {"type": "discard", "id", "provider", "folder"} throws away what a
#   paused one got
#   {"type": "upload", "id": "...", "path": "/...", "target": "ftp://...",
#    "user": "...", "password": "..."}, and "upload check" without id and
#   path: a downloaded video copied to a network folder
#   {"type": "organize", "id", "path", "root", "provider", "kind", "title",
#    "channel", "names": {"movies", "series", "videos", "season",
#    "podcasts"}}: a download put in order
#   {"type": "youtube account", "enabled": true}: the pages of the account
#   (channels ":subscriptions", ":watchlater", ":liked", ":history",
#   ":recommended") read with the cookies of WebPositive; "youtube check"
#   tries them
#   {"type": "check"}: whether TMDB takes the key
#
# "find" and "download" carry "height": the highest picture wanted, 0 for
# what suits the machine, and "limit", what the screen and the processor
# allow; they stay for the next ones.
#
# The answers:
#
#   {"type": "providers", "providers": [{"id", "name", "kinds"}, ...]}, again
#   when the servers of the home network answer; then {"type": "ready"}
#   {"type": "results", "tag": 1, "items": [MediaItem, ...]}, with "about"
#   (name, handle, followers, description, avatar) for a channel
#   {"type": "nothing", "tag": 1}
#   {"type": "details", "id": "...", "item": MediaItem or null}
#   {"type": "found", "id": "...", "url": "...", "note": ""}  (note
#   "javascript": not above 360p, no QuickJS nor Node.js)
#   {"type": "lost", "id": "..."}
#   {"type": "subtitles", "id": "...", "tracks": [{"language", "name",
#    "automatic"}, ...]}
#   {"type": "subtitle", "id": "...", "language": "...", "automatic": false,
#    "text": "WEBVTT...", "failed": false}
#   {"type": "progress", "id": "...", "percent": 42, "done", "total",
#    "speed", "eta", "peers"}, with "map" for a torrent: what of its video
#   is there (torrent_map)
#   {"type": "saved", "id": "...", "path": "/..."}
#   {"type": "failed", "id": "..."}, {"type": "cancelled", "id": "..."},
#   {"type": "paused", "id": "..."}
#   {"type": "uploading", "id": "...", "percent": 42}
#   {"type": "uploaded", "id": "...", "ok": true, "error": ""}
#   {"type": "upload checked", "ok": true, "error": ""}
#   {"type": "organized", "id", "from", "path", "picture", "section"}
#   (section: "movies", "series", "videos" or "podcasts")
#   {"type": "youtube checked", "ok": true, "error": ""}
#   {"type": "checked", "ok": true}
#
# A MediaItem is {"provider", "kind", "id", "title", "description",
# "channel", "channel_id", "duration", "year", "date", "views", "rating",
# "poster", "thumbnail", "season", "episode"}; kind is "video", "movie",
# "episode", "live", or "series" (a folder or a series, opened as a
# channel). Some add "timestamp", to sort them.
#
# Loading yt-dlp takes about 9 s on the AC100; this way it happens once.
# Each request runs on its own thread, so a search does not wait for a video.
# Searches are kept for a few hours in the folder given as argument.

import hashlib
import html
import http.server
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request


def unpacked_yt_dlp(archive):
	"""Where the single-file release is kept unpacked and compiled, for
	this version of it, and whether it is there yet. From the zip archive
	Python compiles every module again at each start: 9 s on the AC100,
	against 2.5 s for the compiled copy."""
	folder = sys.argv[1] if len(sys.argv) > 1 else ""
	if not folder:
		return None, False
	status = os.stat(archive)
	name = "yt-dlp-%d-%d" % (int(status.st_mtime), status.st_size)
	path = os.path.join(folder, name)
	return path, os.path.isfile(os.path.join(path, "complete"))


def unpack_yt_dlp(archive, path):
	"""Unpacks and compiles the release in the background, a minute after
	the start so as not to slow it, in a process of its own; then drops
	the copies of older versions."""
	def work():
		time.sleep(60)
		temporary = path + ".part"
		try:
			import zipfile
			shutil.rmtree(temporary, ignore_errors=True)
			with zipfile.ZipFile(archive) as release:
				release.extractall(temporary)
			subprocess.run([sys.executable, "-m", "compileall", "-q",
				temporary], stdout=subprocess.DEVNULL,
				stderr=subprocess.DEVNULL, check=True)
			open(os.path.join(temporary, "complete"), "w").close()
			shutil.rmtree(path, ignore_errors=True)
			os.rename(temporary, path)
		except Exception:
			shutil.rmtree(temporary, ignore_errors=True)
			return
		folder = os.path.dirname(path)
		for name in os.listdir(folder):
			old = os.path.join(folder, name)
			if name.startswith("yt-dlp-") and old != path:
				shutil.rmtree(old, ignore_errors=True)
	threading.Thread(target=work, daemon=True).start()


def load_yt_dlp():
	try:
		import yt_dlp
		return yt_dlp
	except ImportError:
		pass
	# the single-file release: a zip archive Python can import from
	for directory in os.environ.get("PATH", "").split(os.pathsep):
		candidate = os.path.join(directory, "yt-dlp")
		if os.path.isfile(candidate):
			try:
				unpacked, ready = unpacked_yt_dlp(candidate)
			except OSError:
				unpacked, ready = None, False
			if ready:
				sys.path.insert(0, unpacked)
				try:
					import yt_dlp
					return yt_dlp
				except ImportError:
					sys.path.remove(unpacked)
			sys.path.insert(0, candidate)
			import yt_dlp
			if unpacked:
				unpack_yt_dlp(candidate, unpacked)
			return yt_dlp
	raise ImportError("yt-dlp not found")


def yt_dlp_available():
	"""Whether yt-dlp is there, without loading it."""
	import importlib.util
	try:
		if importlib.util.find_spec("yt_dlp") is not None:
			return True
	except (ImportError, ValueError):
		pass
	return any(os.path.isfile(os.path.join(directory, "yt-dlp"))
		for directory in os.environ.get("PATH", "").split(os.pathsep))


# the words, and the short forms YouTube writes ("1 g fa", "2 gg fa")
ITALIAN_UNITS = {"secondi": "second", "secondo": "second", "sec": "second",
	"s": "second", "minuti": "minute", "minuto": "minute", "min": "minute",
	"ore": "hour", "ora": "hour", "h": "hour", "giorni": "day",
	"giorno": "day", "gg": "day", "g": "day", "settimane": "week",
	"settimana": "week", "sett": "week", "mesi": "month", "mese": "month",
	"anni": "year", "anno": "year", "aa": "year", "a": "year"}


def teach_italian(module):
	"""yt-dlp reads when a video came out ("3 hours ago") in English only:
	asked in Italian, for titles in Italian rather than translated, it
	reads "3 ore fa", "ieri", "oggi" too. Left as it is if yt-dlp changed."""
	try:
		try:
			from yt_dlp.extractor.youtube._base import \
				YoutubeBaseInfoExtractor as base
		except ImportError:
			from yt_dlp.extractor.youtube import \
				YoutubeBaseInfoExtractor as base
		from yt_dlp.utils import datetime_from_str
		english = base.extract_relative_time.__func__
	except (ImportError, AttributeError):
		return
	units = "|".join(sorted(ITALIAN_UNITS, key=len, reverse=True))
	pattern = re.compile(r"\b(?P<start>oggi|ieri|adesso)\b|(?P<time>\d+)\s*"
		r"(?P<unit>%s)\.?\s+fa\b" % units, re.IGNORECASE)

	def relative(cls, text):
		found = english(cls, text)
		if found is not None or not text:
			return found
		match = pattern.search(text)
		if not match:
			return None
		start = (match.group("start") or "").lower()
		try:
			if start:
				return datetime_from_str({"oggi": "today",
					"ieri": "yesterday", "adesso": "now"}[start])
			return datetime_from_str("now-%s%s" % (match.group("time"),
				ITALIAN_UNITS[match.group("unit").lower()]))
		except ValueError:
			return None

	base.extract_relative_time = classmethod(relative)


def youtube_lang():
	"""Titles and descriptions as YouTube writes them in the language of the
	system, rather than translated into English: in Italian, whose dates
	teach_italian() reads."""
	return {"lang": ["it"]} if LANGUAGE.lower().startswith("it") else {}


class LazyYtDlp:
	"""yt-dlp, loaded the first time it is used: 12 s and 26 MB on the
	AC100 that podcasts, the news from feeds, TV and the home network do
	not wait for, nor take when YouTube is not used."""

	def __init__(self):
		self._module = None
		self._lock = threading.Lock()

	def __getattr__(self, name):
		if name.startswith("_"):
			raise AttributeError(name)
		if self._module is None:
			with self._lock:
				if self._module is None:
					module = load_yt_dlp()
					teach_italian(module)
					self._module = module
		return getattr(self._module, name)


# the Internet Archive and the other sources still work without it
yt_dlp = LazyYtDlp() if yt_dlp_available() else None
output = threading.Lock()

CACHE_TIME = 6 * 60 * 60
DETAILS_CACHE_TIME = 24 * 60 * 60
cache = sys.argv[1] if len(sys.argv) > 1 else None
# the language of the system, as "it" or "pt-BR", for what TMDB writes
LANGUAGE = sys.argv[2] if len(sys.argv) > 2 else "en"

DEFAULT_PROVIDER = "youtube"
PROVIDERS = {}
# what yt-dlp said of the last videos: the page of a video, playing it and
# its subtitles would take one extraction each otherwise (11 s on the
# AC100). Not for long: the addresses of the streams expire.
INFO_CACHE = {}
INFO_LOCK = threading.Lock()
INFO_CACHE_TIME = 30 * 60
INFO_CACHE_SIZE = 8


def finite(value):
	"""value with its NaN and infinities as 0: Cinema's JSON has no words
	for them, and would drop the whole answer."""
	if isinstance(value, float):
		return value if value == value and value not in (float("inf"),
			float("-inf")) else 0
	if isinstance(value, dict):
		return {key: finite(item) for key, item in value.items()}
	if isinstance(value, (list, tuple)):
		return [finite(item) for item in value]
	return value


def answer(message):
	# always a line: what cannot be written is written as text, and an
	# answer is never lost to a value json does not take
	try:
		line = json.dumps(message, ensure_ascii=False, allow_nan=False,
			default=str)
	except (TypeError, ValueError):
		line = json.dumps(finite(message), ensure_ascii=False, default=str)
	with output:
		try:
			sys.stdout.write(line + "\n")
			sys.stdout.flush()
		except (OSError, ValueError):
			# Cinema is gone: watch_parent and the end of stdin stop us
			pass


def as_int(value, fallback):
	try:
		return int(value)
	except (TypeError, ValueError):
		return fallback


def subtitle_time(milliseconds):
	"""A time of WebVTT, from the milliseconds of YouTube's json3."""
	milliseconds = max(0, as_int(milliseconds, 0))
	return "%02d:%02d:%02d.%03d" % (milliseconds // 3600000,
		milliseconds // 60000 % 60, milliseconds // 1000 % 60,
		milliseconds % 1000)


def json3_to_vtt(text):
	"""YouTube's json3 captions, which are small, as WebVTT."""
	try:
		data = json.loads(text)
	except ValueError:
		return ""
	lines = ["WEBVTT", ""]
	for event in data.get("events") or []:
		content = "".join(str(segment.get("utf8") or "")
			for segment in event.get("segs") or [])
		content = re.sub(r"\s+", " ", content).strip()
		if not content:
			continue
		start = as_int(event.get("tStartMs"), 0)
		duration = as_int(event.get("dDurationMs"), 0) or 2000
		lines.append("%s --> %s" % (subtitle_time(start),
			subtitle_time(start + duration)))
		lines.append(content)
		lines.append("")
	return "\n".join(lines)


def clean(text):
	"""The text of a title or a channel: one line, as the grid shows it."""
	return str(text).replace("\t", " ").replace("\n", " ").replace("\r", " ")


def multiline(text):
	"""The description, with its own lines, as the detail view shows it."""
	return str(text).replace("\r\n", "\n").replace("\r", "\n").replace(
		"\t", " ").strip()


# a video being opened, or its page: the lists of the home and their
# refreshes wait meanwhile, as threads of Python share one interpreter and
# the AC100 two slow cores
PLAYING = [0]
PLAYING_CONDITION = threading.Condition()


class playing:
	def __enter__(self):
		with PLAYING_CONDITION:
			PLAYING[0] += 1

	def __exit__(self, *details):
		with PLAYING_CONDITION:
			PLAYING[0] -= 1
			PLAYING_CONDITION.notify_all()


def wait_for_play(most=60):
	"""Until no video is being opened, for most seconds at most."""
	deadline = time.time() + most
	with PLAYING_CONDITION:
		while PLAYING[0] > 0 and time.time() < deadline:
			PLAYING_CONDITION.wait(deadline - time.time())


STALE_TIME = 7 * 24 * 60 * 60
REFRESHING = set()
REFRESHING_LOCK = threading.Lock()


def store_videos(path, videos):
	temporary = path + ".part.%d" % threading.get_ident()
	try:
		with open(temporary, "w", encoding="utf-8") as file:
			for video in videos:
				file.write(json.dumps(video, ensure_ascii=False) + "\n")
		os.replace(temporary, path)
	except OSError:
		pass


def cached(name, fetch, fresh=None, stale=None):
	"""The videos fetch() gives, from the cache when it is recent enough.
	Older than CACHE_TIME but not than STALE_TIME, they are given at once
	all the same, and fetched again in the background for the next time:
	the shelves of the home show without waiting for yt-dlp."""
	fresh = CACHE_TIME if fresh is None else fresh
	stale = STALE_TIME if stale is None else stale
	path = None
	if cache:
		key = hashlib.sha1(name.encode()).hexdigest()
		path = os.path.join(cache, "videos-%s.json" % key[:16])
		try:
			age = time.time() - os.path.getmtime(path)
			if age < stale:
				with open(path, encoding="utf-8") as file:
					videos = [json.loads(line) for line in file if line.strip()]
				if videos and age >= fresh:
					refresh_later(path, fetch)
				if videos:
					return videos
		except (OSError, ValueError):
			pass

	videos = fetch()
	if path and videos:
		store_videos(path, videos)
	return videos


def refresh_later(path, fetch):
	with REFRESHING_LOCK:
		if path in REFRESHING:
			return
		REFRESHING.add(path)

	def work():
		try:
			wait_for_play()
			videos = fetch()
			if videos:
				store_videos(path, videos)
		except Exception:
			pass
		finally:
			with REFRESHING_LOCK:
				REFRESHING.discard(path)
	threading.Thread(target=work, daemon=True).start()


def stream_path(media_id):
	if not cache:
		return None
	key = hashlib.sha1(media_id.encode()).hexdigest()[:16]
	return os.path.join(cache, "stream-%s.json" % key)


def stream_expiry(url):
	"""When the address of a stream of YouTube expires (its expire=), or 0."""
	found = re.search(r"[?&/]expire[=/](\d+)", url)
	return int(found.group(1)) if found else 0


def kept_stream(media_id):
	"""The address found for the video before, if it still has half an hour:
	it plays at once, after Cinema started again too."""
	path = stream_path(media_id)
	try:
		with open(path, encoding="utf-8") as file:
			kept = json.load(file)
		url = kept.get("url")
		if kept.get("id") == media_id and url \
				and kept.get("expire", 0) - time.time() > 30 * 60:
			if stream_works(url):
				return url
			# another network, say: YouTube gives addresses for one IP
			forget_stream(media_id)
	except (OSError, ValueError, TypeError):
		pass
	return None


def stream_works(url):
	"""Whether YouTube still sends the file of an address: its first byte,
	in less than a second, where finding the video again takes several."""
	request = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
	try:
		with urllib.request.urlopen(request, timeout=8) as response:
			return response.status in (200, 206)
	except Exception:
		return False


def forget_stream(media_id):
	"""An address that does not play any more: off the disk and the memory,
	so that the video is found again."""
	path = stream_path(media_id)
	try:
		if path:
			os.remove(path)
	except OSError:
		pass
	with INFO_LOCK:
		INFO_CACHE.pop(media_id, None)


def keep_stream(media_id, url):
	path = stream_path(media_id)
	expire = stream_expiry(url)
	if not path or not expire:
		return
	temporary = path + ".part.%d" % threading.get_ident()
	try:
		with open(temporary, "w", encoding="utf-8") as file:
			json.dump({"id": media_id, "url": url, "expire": expire}, file)
		os.replace(temporary, path)
	except OSError:
		pass


def about_path(provider_id, channel_id):
	if not cache:
		return None
	key = hashlib.sha1(("%s %s" % (provider_id, channel_id)).encode())
	return os.path.join(cache, "about-%s.json" % key.hexdigest()[:16])


def keep_about(provider_id, channel_id, about):
	"""What a channel is, kept with its list: shown even when the list comes
	from the cache."""
	path = about_path(provider_id, channel_id)
	if not path or not about or not about.get("name"):
		return
	temporary = path + ".part.%d" % threading.get_ident()
	try:
		with open(temporary, "w", encoding="utf-8") as file:
			json.dump(about, file, ensure_ascii=False)
		os.replace(temporary, path)
	except OSError:
		pass


def kept_about(provider_id, channel_id):
	path = about_path(provider_id, channel_id)
	try:
		with open(path, encoding="utf-8") as file:
			about = json.load(file)
		return about if isinstance(about, dict) else None
	except (OSError, TypeError, ValueError):
		return None


def cached_details(name, fetch):
	"""The MediaItem fetch() gives, from the cache when it is recent enough.

	A video does not change much, so its details are kept longer than the
	lists, and a failure is not kept at all.
	"""
	path = None
	if cache:
		key = hashlib.sha1(name.encode()).hexdigest()
		path = os.path.join(cache, "details-%s.json" % key[:16])
		try:
			if time.time() - os.path.getmtime(path) < DETAILS_CACHE_TIME:
				with open(path, encoding="utf-8") as file:
					return json.load(file)
		except (OSError, ValueError):
			pass

	item = fetch()
	if path and item:
		temporary = path + ".part.%d" % threading.get_ident()
		try:
			with open(temporary, "w", encoding="utf-8") as file:
				json.dump(item, file, ensure_ascii=False)
			os.replace(temporary, path)
		except OSError:
			pass
	return item


USER_AGENT = "Cinema (Haiku)"
# machines that play 320x240 or 360p at once, and need a faster processor
# for more, such as the Toshiba AC100
MACHINE = platform.machine().lower()
SLOW_MACHINE = MACHINE.startswith("arm") and "64" not in MACHINE


def total_memory():
	try:
		return os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
	except (ValueError, OSError, AttributeError):
		return 0


# a machine whose memory a few runs of the JavaScript of YouTube can fill:
# the AC100 (512 MB), an old netbook with 32-bit Haiku
SHORT_OF_MEMORY = SLOW_MACHINE or 0 < total_memory() < 1536 * 1024 * 1024
# the height of the videos Cinema asks for, which a request may carry: 0
# lets the machine choose
HEIGHT = [0]


# what the JavaScript of the web client of YouTube needs on the AC100 with
# yt-dlp and QuickJS, at its peak: more than 100 MB, a few minutes; with
# less free, the 512 MB of the machine ran out and it stopped answering
JS_MEMORY = 160 * 1024 * 1024
# one at a time there
JS_LOCK = threading.Lock()


def free_memory():
	"""The memory free now, in bytes, or None when it is not known."""
	try:
		return os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
	except (ValueError, OSError, AttributeError):
		return None


# the highest picture the screen and the processor allow, which Cinema
# sends: "Automatic" (HEIGHT 0) no higher than that either
LIMIT = [0]


def limited(height):
	return min(height, LIMIT[0]) if LIMIT[0] > 0 else height


def wanted_height():
	return limited(HEIGHT[0] or (360 if SLOW_MACHINE else 720))


def download_height():
	"""The height of a download: as for playing, but 480p on a slow
	machine, which plays a 480p file smoothly with FFmpeg and the overlay
	(the AC100) while finding the two streams of YouTube to play them at
	once takes it a minute and a half."""
	return limited(HEIGHT[0] or (480 if SLOW_MACHINE else 720))


def http_json(url, query=None):
	if query:
		url += "?" + urllib.parse.urlencode(query)
	request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
	with urllib.request.urlopen(request, timeout=30) as response:
		return json.load(response)


def file_name(title):
	"""A title that can be the name of a file: "Name: Subtitle" as
	"Name - Subtitle", the spaces left once."""
	text = re.sub(r"\s*:\s+", " - ", str(title))
	text = re.sub(r'[/\\:*?"<>|]', " ", text)
	return re.sub(r"\s+", " ", text).strip()[:150] or "video"


def size_text(size):
	"""A size as aria2 writes it: 512KiB, 12.3MiB."""
	size = float(size)
	for unit in ("B", "KiB", "MiB", "GiB"):
		if size < 1024 or unit == "GiB":
			return ("%d%s" if unit == "B" else "%.1f%s") % (size, unit)
		size /= 1024


def duration_text(seconds):
	"""A time as aria2 writes it: 45s, 4m25s, 1h3m."""
	seconds = int(seconds)
	if seconds < 60:
		return "%ds" % seconds
	if seconds < 3600:
		return "%dm%ds" % (seconds // 60, seconds % 60)
	return "%dh%dm" % (seconds // 3600, seconds // 60 % 60)


class Cancelled(Exception):
	pass


class Paused(Exception):
	"""A download paused: what it got is kept, to go on later."""


# the downloads paused, stopping now
PAUSED = set()


# the downloads asked to stop, and the programs of those that run one
CANCELLED = set()
RUNNING = {}


def short_id(media_id):
	"""A few letters that name a video in the name of its file: an id that
	is an address or a path cannot be one."""
	return hashlib.sha1(media_id.encode("utf-8", "surrogateescape")) \
		.hexdigest()[:8]


def http_download(url, path, size=0, progress=None):
	"""Downloads url to path, through path.part. Paused (or a network that
	failed), the .part stays, and the next time goes on from where it was
	when the server can give the rest; cancelled, it goes. size is used
	when the server does not tell the length; a file that comes shorter
	than told is not taken.

	Every download of a file by HTTP goes through here: one place for the
	progress, the pause, the cancel and the checks."""
	temporary = path + ".part"
	have = os.path.getsize(temporary) if os.path.exists(temporary) else 0
	headers = {"User-Agent": USER_AGENT}
	if have:
		headers["Range"] = "bytes=%d-" % have
	request = urllib.request.Request(url, headers=headers)
	try:
		with urllib.request.urlopen(request, timeout=60) as response:
			# the rest, or (the server not giving parts) all again
			if have and response.status != 206:
				have = 0
			total = as_int(response.headers.get("Content-Length"), 0)
			total = total + have if total else size
			with open(temporary, "ab" if have else "wb") as output_file:
				done = have
				start = time.time()
				while True:
					block = response.read(256 * 1024)
					if not block:
						break
					output_file.write(block)
					done += len(block)
					if progress is not None:
						elapsed = max(time.time() - start, 0.001)
						speed = (done - have) / elapsed
						progress(int(100 * done / total) if total else 0,
							done=size_text(done),
							total=size_text(total) if total else "",
							speed=size_text(speed),
							eta=duration_text((total - done) / speed)
								if total and speed > 0 else "")
	except Cancelled:
		try:
			os.remove(temporary)
		except OSError:
			pass
		raise
	if total and os.path.getsize(temporary) < total:
		raise IOError("the file came shorter than told")
	os.replace(temporary, path)
	return path


class Provider:
	"""A source of media: what Cinema can search, find and download.

	Each kind of the class says what it can do, and is what the helper tells
	Cinema at the start. The methods return MediaItems, not yt-dlp's info.
	"""

	id = ""
	name = ""
	kinds = ()

	def describe(self):
		return {"id": self.id, "name": self.name, "kinds": list(self.kinds)}

	def search(self, query, count):
		raise NotImplementedError

	def channel(self, channel_id, count):
		raise NotImplementedError

	def details(self, media_id, kind):
		raise NotImplementedError

	def find(self, media_id, kind):
		raise NotImplementedError

	def download(self, media_id, folder, progress):
		raise NotImplementedError

	def subtitles(self, media_id, kind):
		"""The tracks of the subtitles of a video, as {language, name,
		automatic}, or an empty list."""
		return []

	def subtitle(self, media_id, kind, language, automatic):
		"""The text of one track, as WebVTT, SRT or ASS, or None."""
		return None


# the account of YouTube: the pages of the person signed in (subscriptions,
# Watch later...), with the cookies WebPositive keeps after signing in there;
# only for these pages, the rest stays anonymous
ACCOUNT = {"enabled": False}
YOUTUBE_FEEDS = {
	":subscriptions": "https://www.youtube.com/feed/subscriptions",
	":watchlater": "https://www.youtube.com/playlist?list=WL",
	":liked": "https://www.youtube.com/playlist?list=LL",
	":history": "https://www.youtube.com/feed/history",
	":recommended": "https://www.youtube.com/",
}
WEBPOSITIVE_COOKIES = os.path.join(os.path.expanduser("~"), "config",
	"settings", "WebPositive", "cookie.jar.db")


def youtube_cookies():
	"""The cookies of YouTube and Google that WebPositive keeps, as a
	cookies file of Netscape for yt-dlp, readable only by its owner; None
	without them or without the account on."""
	if not ACCOUNT["enabled"] or not cache \
			or not os.path.exists(WEBPOSITIVE_COOKIES):
		return None
	target = os.path.join(cache, "youtube-cookies.txt")
	sources = [WEBPOSITIVE_COOKIES + ending for ending in ("", "-wal")
		if os.path.exists(WEBPOSITIVE_COOKIES + ending)]
	newest = max(os.path.getmtime(path) for path in sources)
	if os.path.exists(target) and os.path.getmtime(target) >= newest:
		return target
	import sqlite3
	import tempfile
	# a copy, with its journal: WebPositive may be writing to it
	with tempfile.TemporaryDirectory() as folder:
		copy = os.path.join(folder, "cookies.db")
		for path in sources:
			shutil.copyfile(path, copy + path[len(WEBPOSITIVE_COOKIES):])
		connection = sqlite3.connect(copy)
		try:
			columns = [row[1].lower() for row in
				connection.execute("PRAGMA table_info(Cookie)")]
			rows = connection.execute("SELECT * FROM Cookie").fetchall()
		finally:
			connection.close()
	lines = ["# Netscape HTTP Cookie File"]
	for row in rows:
		cookie = dict(zip(columns, row))
		domain = str(cookie.get("domain") or "")
		if not domain.lstrip(".").endswith(("youtube.com", "google.com")):
			continue
		expires = as_int(cookie.get("expires"), 0)
		if expires > 10 ** 11:
			expires //= 1000
		lines.append("\t".join([
			("#HttpOnly_" if cookie.get("httponly") else "") + domain,
			"TRUE" if domain.startswith(".") else "FALSE",
			str(cookie.get("path") or "/"),
			"TRUE" if cookie.get("secure") else "FALSE",
			str(max(0, expires)), str(cookie.get("name") or ""),
			str(cookie.get("value") or "")]))
	temporary = target + ".part"
	descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
		0o600)
	with os.fdopen(descriptor, "w", encoding="utf-8") as file:
		file.write("\n".join(lines) + "\n")
	os.replace(temporary, target)
	return target


def forget_youtube_cookies():
	if cache:
		try:
			os.remove(os.path.join(cache, "youtube-cookies.txt"))
		except OSError:
			pass


class YouTubeProvider(Provider):
	id = "youtube"
	name = "YouTube"
	kinds = ("search", "channel", "details", "find", "download")

	# YouTube's Android client needs no JavaScript to give the file; the web
	# client with QuickJS is much slower on the AC100 and only used if it fails
	FORMAT = "18/b[height<=360][vcodec^=avc1]"
	OPTIONS = {"quiet": True, "no_warnings": True, "noprogress": True}

	def item(self, entry, channel_name="", channel_id=""):
		"""The MediaItem of an entry of yt-dlp, or None if it can't be played."""
		duration = entry.get("duration")
		# a live stream has no duration, and no file to play
		if not entry.get("id") or not duration:
			return None
		return {
			"provider": self.id,
			"kind": "video",
			"id": entry["id"],
			"title": clean(entry.get("title") or ""),
			"description": clean(entry.get("description") or ""),
			"channel": clean(entry.get("channel") or channel_name),
			"channel_id": entry.get("channel_id") or channel_id,
			"duration": int(duration),
			"year": 0,
			# the day it came out, from "3 days ago" (approximate_date), and
			# its views: given by the list itself, at no cost
			"date": self.day_of(entry),
			"views": as_int(entry.get("view_count"), 0),
			"rating": 0.0,
			"poster": "",
			"thumbnail": "https://i.ytimg.com/vi/%s/mqdefault.jpg" % entry["id"],
			"season": 0,
			"episode": 0,
		}

	@staticmethod
	def day_of(entry):
		"""YYYY-MM-DD of an entry of yt-dlp, or empty."""
		day = entry.get("upload_date") or ""
		if len(day) == 8 and day.isdigit():
			return "%s-%s-%s" % (day[:4], day[4:6], day[6:])
		stamp = entry.get("timestamp") or entry.get("release_timestamp")
		if stamp:
			try:
				return time.strftime("%Y-%m-%d", time.gmtime(int(stamp)))
			except (TypeError, ValueError, OverflowError):
				pass
		return ""

	def picture(self, info):
		"""The address of the biggest picture of a video yt-dlp described.

		The list of the thumbnails does not always say how big they are, so
		the name of the file does: maxresdefault is 1280x720, hq720 the same
		in another name, sddefault 640x480, hqdefault 480x360.
		"""
		urls = [thumbnail.get("url") or ""
			for thumbnail in info.get("thumbnails") or []]
		urls.append(info.get("thumbnail") or "")
		for name in ("maxresdefault", "hq720", "sddefault", "hqdefault",
				"mqdefault"):
			for url in urls:
				if name in url and ".jpg" in url:
					return url.split("?")[0]
		for url in urls:
			if ".jpg" in url:
				return url.split("?")[0]
		if info.get("id"):
			return "https://i.ytimg.com/vi/%s/hqdefault.jpg" % info["id"]
		return ""

	def full_item(self, info):
		"""The MediaItem of a video yt-dlp described in full, not flat."""
		video = self.item(info)
		if video is None:
			return None
		date = str(info.get("upload_date") or "")
		if len(date) == 8:
			video["date"] = "%s-%s-%s" % (date[:4], date[4:6], date[6:])
			video["year"] = as_int(date[:4], 0)
		video["description"] = multiline(info.get("description") or "")
		video["views"] = as_int(info.get("view_count"), 0)
		video["poster"] = self.picture(info)
		return video

	# the YoutubeDL objects, kept for the next extraction: making one costs
	# most of a second on the AC100, and its extractors are slow the first
	# time they work. Each is lent to one thread at a time.
	EXTRACTORS = {}
	EXTRACTORS_LOCK = threading.Lock()
	# the videos being extracted, so that the page, playing and the
	# subtitles of one wait for a single extraction
	EXTRACTING = {}

	class borrowed:
		def __init__(self, provider, key, options):
			self.provider, self.key, self.options = provider, key, options

		def __enter__(self):
			with self.provider.EXTRACTORS_LOCK:
				free = self.provider.EXTRACTORS.setdefault(self.key, [])
				self.ydl = free.pop() if free else None
			if self.ydl is None:
				self.ydl = yt_dlp.YoutubeDL(self.options)
			return self.ydl

		def __exit__(self, *details):
			with self.provider.EXTRACTORS_LOCK:
				free = self.provider.EXTRACTORS[self.key]
				if len(free) < 2:
					free.append(self.ydl)

	def extractor(self, client, formatted):
		"""A YoutubeDL to lend that describes a video, from a client of
		YouTube ("" the web client) and with or without Cinema's format."""
		arguments = {"youtube": dict({"player_skip": ["webpage"]},
			**youtube_lang())}
		if client:
			arguments["youtube"]["player_client"] = [client]
		options = dict(self.OPTIONS, extractor_args=arguments)
		if formatted:
			options["format"] = self.FORMAT
		if not client:
			options["js_runtimes"] = YouTubeProvider.JS_RUNTIMES
		return self.borrowed(self, ("video", client, formatted), options)

	def warm_up(self):
		"""Makes ready what the first video played needs, while nothing
		else is asked: seconds less for it on the AC100."""
		wait_for_play()
		with self.extractor("android", True) as ydl:
			ydl.get_info_extractor("Youtube")

	def video_info(self, media_id, playable):
		"""What yt-dlp says of a video, from INFO_CACHE when it is there.
		With the format Cinema plays, so that info["url"] is its address;
		without it too when playable is False and no format fits, for the
		page and the subtitles of such a video. Without the page of the
		video, which the Android client does not need: half the time."""
		def known():
			found = INFO_CACHE.get(media_id)
			if found is not None and time.time() - found[0] < INFO_CACHE_TIME \
					and (found[2] or not playable):
				return found[1]
			return None

		while True:
			with INFO_LOCK:
				info = known()
				if info is not None:
					return info
				busy = self.EXTRACTING.get(media_id)
				if busy is None:
					busy = threading.Event()
					self.EXTRACTING[media_id] = busy
					break
			# another thread is at it: its answer, then
			busy.wait(120)

		try:
			url = "https://www.youtube.com/watch?v=" + media_id
			for formatted in ((True,) if playable else (True, False)):
				for client in ("android", ""):
					try:
						with self.extractor(client, formatted) as ydl:
							info = ydl.extract_info(url, download=False)
					except Exception:
						continue
					if not info or (formatted and not info.get("url")):
						continue
					with INFO_LOCK:
						INFO_CACHE[media_id] = (time.time(), info, formatted)
						for old in sorted(INFO_CACHE,
								key=lambda key: INFO_CACHE[key][0])[
									:max(0, len(INFO_CACHE) - INFO_CACHE_SIZE)]:
							del INFO_CACHE[old]
					return info
			return None
		finally:
			with INFO_LOCK:
				self.EXTRACTING.pop(media_id, None)
			busy.set()

	def details(self, media_id, kind="video"):
		info = self.video_info(media_id, False)
		return self.full_item(info) if info else None

	def videos_of(self, url, count, channel_name="", channel_id="",
			cookies=None):
		# approximate_date: the day of each video from "3 days ago" in the
		# list, without opening its page
		options = dict(self.OPTIONS, extract_flat="in_playlist",
			playlistend=count, extractor_args={
				"youtubetab": {"approximate_date": [""]},
				"youtube": dict({"approximate_date": [""]}, **youtube_lang())})
		if cookies:
			# the pages of the account: a YoutubeDL of their own, not lent
			options["cookiefile"] = cookies
			with yt_dlp.YoutubeDL(options) as ydl:
				info = ydl.extract_info(url, download=False)
		else:
			with self.borrowed(self, ("list", count), options) as ydl:
				info = ydl.extract_info(url, download=False)
		if channel_id:
			keep_about(self.id, channel_id, self.about(info))
		videos = []
		for entry in info.get("entries") or []:
			video = self.item(entry,
				channel_name=info.get("channel") or channel_name,
				channel_id=info.get("channel_id") or channel_id)
			if video is not None:
				# when it came out, to put them in order (Cinema reads the
				# date)
				video["timestamp"] = as_int(entry.get("timestamp"), 0)
				videos.append(video)
		return videos

	@staticmethod
	def about(info):
		"""What the list of a channel says of it: its name, handle, followers,
		the start of its description and its picture."""
		avatar = ""
		square = 0
		for thumbnail in info.get("thumbnails") or []:
			url = thumbnail.get("url") or ""
			if thumbnail.get("id") == "avatar_uncropped" and url:
				avatar = url
				break
			width = as_int(thumbnail.get("width"), 0)
			if url and width and width == as_int(thumbnail.get("height"), 0) \
					and width > square:
				avatar, square = url, width
		# the pictures of googleusercontent take their size at the end
		avatar = re.sub(r"=s\d+(-[^/]*)?$", "=s176", avatar) \
			if "googleusercontent" in avatar else avatar
		description = clean(info.get("description") or "")
		return {
			"name": clean(info.get("channel") or info.get("uploader") or ""),
			"handle": info.get("uploader_id") or "",
			"followers": as_int(info.get("channel_follower_count"), 0),
			"description": description[:400],
			"avatar": avatar,
		}

	def search(self, query, count, order=""):
		if order != "newest":
			return self.videos_of("ytsearch%d:%s" % (count, query), count)
		# the latest first: YouTube no longer sorts a search by date (and
		# yt-dlp has no ytsearchdate any more), so the videos of this week,
		# put in order by the day each says it came out ("3 hours ago"),
		# YouTube's order kept within a day; all of them if the week has
		# too few
		# today's first (YouTube's choice among a day is its most relevant
		# ones, which may leave today out of the week's), then the week's;
		# not the shorts, under a minute, which are seldom news
		address = "https://www.youtube.com/results?search_query=%s&sp=%s"
		quoted = urllib.parse.quote_plus(query)
		videos = []
		seen = set()
		for period in ("EgIIAg%3D%3D", "EgIIAw%3D%3D"):
			try:
				found = self.videos_of(address % (quoted, period), count)
			except Exception:
				found = []
			for video in found:
				if video["id"] not in seen and video.get("duration", 0) > 60:
					seen.add(video["id"])
					videos.append(video)
		if len(videos) < min(8, count):
			for video in self.videos_of("ytsearch%d:%s" % (count, query),
					count):
				if video["id"] not in seen:
					seen.add(video["id"])
					videos.append(video)
		videos.sort(key=lambda video: -(video.get("timestamp") or 0))
		return videos[:max(count, 24)]

	RSS = "https://www.youtube.com/feeds/videos.xml?channel_id=%s"
	ATOM = {"a": "http://www.w3.org/2005/Atom",
		"yt": "http://www.youtube.com/xml/schemas/2015",
		"media": "http://search.yahoo.com/mrss/"}

	def feed_of(self, channel):
		"""The latest videos of a channel from its RSS feed: their original
		titles (yt-dlp gets them translated into English), the second each
		came out, its views; no shorts. Light and quick, without yt-dlp;
		their length is not in it."""
		import xml.etree.ElementTree as tree
		import calendar
		request = urllib.request.Request(self.RSS % channel,
			headers={"User-Agent": USER_AGENT})
		with urllib.request.urlopen(request, timeout=20) as response:
			root = tree.fromstring(response.read(2 * 1024 * 1024))
		names = self.ATOM
		videos = []
		for entry in root.findall("a:entry", names):
			video_id = entry.findtext("yt:videoId", "", names)
			link = entry.find("a:link", names)
			address = link.get("href") if link is not None else ""
			if not video_id or "/shorts/" in address:
				continue
			published = entry.findtext("a:published", "", names)
			try:
				stamp = calendar.timegm(time.strptime(published[:19],
					"%Y-%m-%dT%H:%M:%S"))
			except ValueError:
				stamp = 0
			statistics = entry.find("media:group/media:community/"
				"media:statistics", names)
			videos.append({"provider": self.id, "kind": "video",
				"id": video_id,
				"title": clean(entry.findtext("a:title", "", names)),
				"description": clean(entry.findtext(
					"media:group/media:description", "", names)),
				"channel": clean(entry.findtext("a:author/a:name", "",
					names)),
				"channel_id": channel, "duration": 0,
				"year": as_int(published[:4], 0), "date": published[:10],
				"views": as_int(statistics.get("views")
					if statistics is not None else 0, 0),
				"rating": 0.0, "poster": "", "thumbnail": "", "season": 0,
				"episode": 0, "timestamp": stamp})
		return videos

	def news(self, channels, count):
		"""The latest videos of news channels together, the newest first,
		from their RSS feeds (yt-dlp for one whose feed fails). A few
		channels at a time."""
		ids = [channel for channel in channels.split(",")
			if re.fullmatch(r"UC[\w-]{22}", channel)]
		videos = []
		lock = threading.Lock()

		def fetch(channel):
			try:
				found = self.feed_of(channel)
			except Exception:
				try:
					found = self.videos_of("https://www.youtube.com/channel/"
						"%s/videos" % channel, 8, channel_id=channel)
				except Exception:
					return
			with lock:
				videos.extend(found)

		from concurrent.futures import ThreadPoolExecutor
		with ThreadPoolExecutor(4) as pool:
			list(pool.map(fetch, ids))
		videos.sort(key=lambda video: -(video.get("timestamp") or 0))
		return videos[:max(count, 60)]

	def channel(self, channel_id, count):
		# the news of a few channels together, the newest first
		if channel_id.startswith(":news:"):
			return self.news(channel_id[len(":news:"):], count)
		# the pages of the account signed in
		if channel_id in YOUTUBE_FEEDS:
			cookies = youtube_cookies()
			if not cookies:
				return []
			return self.videos_of(YOUTUBE_FEEDS[channel_id], count,
				cookies=cookies)
		# a channel: its latest videos from its feed, in under a second
		# (yt-dlp takes seconds, many on the AC100), with their own titles
		# and the time each came out; what the channel is, its picture and
		# its followers, which the feed does not say, from yt-dlp behind,
		# the first time
		if re.fullmatch(r"UC[\w-]{22}", channel_id):
			try:
				videos = self.feed_of(channel_id)
			except Exception:
				videos = []
			if videos:
				if not kept_about(self.id, channel_id):
					threading.Thread(target=lambda: ignore_errors(
						lambda: self.videos_of("https://www.youtube.com/"
							"channel/%s/videos" % channel_id, 1,
							channel_id=channel_id)), daemon=True).start()
				return videos[:count] if count > 0 else videos
		# the videos of a channel only name it once, in the list
		return self.videos_of(
			"https://www.youtube.com/channel/%s/videos" % channel_id,
			count, channel_id=channel_id)

	# what runs the JavaScript of the web client: QuickJS, built for the
	# AC100, or Node.js, which HaikuPorts has
	JS_RUNTIMES = {"quickjs": {}, "node": {}}

	def separate(self, height):
		"""The format of a picture up to height and its sound, as two files:
		all YouTube gives above 360p. H.264 and AAC, which every machine
		decodes; through HTTPS, not the playlists of segments."""
		return ("bv*[height<=%d][vcodec^=avc1][protocol^=https]"
			"+ba[acodec^=mp4a][protocol^=https]" % height)

	def find_separate(self, url, height):
		"""The address of the relay that plays the picture and the sound
		of the video together, or None. Only the web client gives them,
		with JavaScript: slow on the AC100, a few seconds elsewhere."""
		if not shutil.which("ffmpeg"):
			# the relay needs it
			return None
		options = dict(self.OPTIONS, format=self.separate(height),
			js_runtimes=self.JS_RUNTIMES)
		try:
			with yt_dlp.YoutubeDL(options) as ydl:
				info = ydl.extract_info(url, download=False)
		except Exception:
			return None
		formats = info.get("requested_formats") or []
		if len(formats) != 2 or not all(f.get("url") for f in formats) \
				or as_int(formats[0].get("height"), 0) <= 360:
			# not better than the single file
			return None
		inputs = [{"url": f["url"], "headers": f.get("http_headers") or {}}
			for f in formats]
		return RELAY.address(inputs, info.get("duration") or 0)

	def find(self, media_id, kind="video"):
		url = "https://www.youtube.com/watch?v=" + media_id
		# above 360p needs the JavaScript of YouTube run: without QuickJS
		# or Node.js it stays at 360p, which Cinema says
		if wanted_height() > 360 and not (shutil.which("qjs")
				or shutil.which("node")):
			NOTES[media_id] = "javascript"
		if wanted_height() > 360:
			found = None
			if not SHORT_OF_MEMORY:
				found = self.find_separate(url, wanted_height())
			else:
				with JS_LOCK:
					free = free_memory()
					if free is None or free >= JS_MEMORY:
						found = self.find_separate(url, wanted_height())
					# else 360p, which needs no JavaScript, rather than the
					# machine stopping
			if found:
				return found
		kept = kept_stream(media_id)
		if kept:
			return kept
		info = self.video_info(media_id, True)
		url = info.get("url") if info else None
		if url:
			keep_stream(media_id, url)
		return url

	def subtitles(self, media_id, kind="video"):
		info = self.video_info(media_id, False)
		if not info:
			return []
		tracks = []
		seen = set()
		for automatic, source in (
				(False, info.get("subtitles") or {}),
				(True, info.get("automatic_captions") or {})):
			for language, formats in source.items():
				if not language or language == "live_chat" or language in seen:
					continue
				if not any(f.get("url") for f in formats or []):
					continue
				# YouTube answers 429 to the tracks it translates by itself
				# (tlang in the address): only the automatic one in the
				# language of the video can be fetched
				if automatic and all("tlang=" in (f.get("url") or "")
						for f in formats if f.get("url")):
					continue
				name = ""
				for f in formats:
					if f.get("name"):
						name = f["name"]
						break
				tracks.append({"language": language, "name": clean(name),
					"automatic": automatic})
				seen.add(language)
		# "it-orig" and "it" are often the same automatic track
		automatic = {t["language"] for t in tracks if t["automatic"]}
		return [t for t in tracks if not (t["automatic"]
			and t["language"].endswith("-orig")
			and t["language"][:-len("-orig")] in automatic)]

	def subtitle(self, media_id, kind, language, automatic):
		info = self.video_info(media_id, False)
		if not info:
			return None
		source = info.get("automatic_captions" if automatic
			else "subtitles") or {}
		formats = source.get(language)
		if not formats:
			base = language.lower().split("-")[0]
			for key, value in source.items():
				if key.lower() == language.lower() \
						or key.lower().split("-")[0] == base:
					language = key
					formats = value
					break
		if not formats:
			return None

		def score(f):
			return 0 if f.get("ext") == "vtt" else \
				1 if f.get("ext") == "json3" else 9

		for f in sorted(formats, key=score):
			url = f.get("url")
			extension = f.get("ext")
			if not url or extension not in ("vtt", "json3"):
				continue
			headers = {"User-Agent": USER_AGENT}
			headers.update(f.get("http_headers") or {})
			try:
				request = urllib.request.Request(url, headers=headers)
				with urllib.request.urlopen(request, timeout=30) as response:
					text = response.read().decode("utf-8", "replace")
			except Exception:
				continue
			if extension == "json3":
				text = json3_to_vtt(text)
			if text.strip():
				return text
		return None

	def download(self, media_id, folder, progress=None):
		url = "https://www.youtube.com/watch?v=" + media_id
		last = [-1]

		def report(status):
			# paused or cancelled at the next piece, whatever is known of
			# its size: some files say none, and were never stopped
			if media_id in CANCELLED:
				raise Cancelled()
			if media_id in PAUSED:
				raise Paused()
			if progress is None or status.get("status") != "downloading":
				return
			done = status.get("downloaded_bytes", 0) or 0
			total = status.get("total_bytes") or \
				status.get("total_bytes_estimate")
			if not total:
				# how much, without a percent
				progress(max(0, last[0]), done=size_text(done), total="")
				return
			percent = int(100 * done / total)
			if percent != last[0]:
				last[0] = percent
				progress(percent, done=size_text(done),
					total=size_text(total))

		# a single file with sound and picture, as played: nothing to merge
		options = dict(self.OPTIONS, format=self.FORMAT,
			progress_hooks=[report],
			outtmpl=os.path.join(folder,
				"%(title).150B [%(id)s].%(ext)s"),
			extractor_args={"youtube": {"player_client": ["android"]}})
		def fetch(ydl, javascript):
			"""The file of the video, its address found first: with the
			JavaScript of the web client, one at a time where memory is
			short and only with enough of it free (a few of them at once
			stopped the AC100), the download itself after, without the
			lock."""
			if javascript and SHORT_OF_MEMORY:
				with JS_LOCK:
					free = free_memory()
					if free is not None and free < JS_MEMORY:
						return None
					info = ydl.extract_info(url, download=False)
				info = ydl.process_ie_result(info, download=True)
			else:
				info = ydl.extract_info(url, download=True)
			done = info.get("requested_downloads") or [{}]
			path = done[0].get("filepath") or ydl.prepare_filename(info)
			return path if os.path.exists(path) else None

		if download_height() > 360 and shutil.which("ffmpeg"):
			# above 360p, the picture and the sound that ffmpeg puts
			# together; the single file if they cannot be had
			better = dict(options, format=self.separate(download_height())
				+ "/" + self.FORMAT, merge_output_format="mp4",
				js_runtimes=self.JS_RUNTIMES)
			del better["extractor_args"]
			try:
				with yt_dlp.YoutubeDL(better) as ydl:
					path = fetch(ydl, True)
				if path:
					return path
			except Exception:
				if media_id in CANCELLED or media_id in PAUSED:
					raise
		for attempt in range(2):
			try:
				with yt_dlp.YoutubeDL(options) as ydl:
					path = fetch(ydl, attempt > 0)
				if path:
					return path
			except Exception:
				if media_id in CANCELLED or media_id in PAUSED:
					raise
			# the web client, as find() does
			options["extractor_args"] = {"youtube": {}}
			options["js_runtimes"] = self.JS_RUNTIMES
		return None


def register(provider_class):
	# a source that cannot start (a broken file of its cache, say) is left
	# out: the others, and the helper, go on
	try:
		provider = provider_class()
	except Exception:
		return provider_class
	PROVIDERS[provider.id] = provider
	return provider_class


class ArchiveProvider(Provider):
	"""The films of the Internet Archive: public domain features, cartoons,
	television and newsreels, with their own metadata. A channel is one of
	its collections, such as feature_films or animationandcartoons.
	"""

	id = "archive"
	name = "Internet Archive"
	kinds = ("search", "channel", "details", "find", "download")

	SITE = "https://archive.org"
	FIELDS = ("identifier", "title", "year", "runtime", "description",
		"creator", "downloads", "avg_rating", "collection")
	# collections that only say where an item comes from
	GENERIC = {"moviesandfilms", "movies", "opensource_movies",
		"feature_films_unsorted", "community_media"}
	# the smallest file that still looks good first on slow machines: the
	# AC100 plays 320x240 at once, 640x480 needs a faster processor
	FORMATS = ("512Kb MPEG4", "h.264", "h.264 IA", "MPEG4", "Ogg Video") \
		if SLOW_MACHINE else ("h.264", "h.264 IA", "512Kb MPEG4", "MPEG4",
			"Ogg Video")

	def __init__(self):
		self.collection_names = {}

	def get_json(self, path, query=None):
		return http_json(self.SITE + path, query)

	@staticmethod
	def seconds(runtime):
		"""The length of a "1:17:26", "20:33", "77 min" or "1 hr 5 min"."""
		text = str(runtime or "").strip().lower()
		if re.fullmatch(r"\d+(:\d{1,2}){1,2}", text):
			total = 0
			for part in text.split(":"):
				total = total * 60 + int(part)
			return total
		hours = re.search(r"(\d+)\s*h", text)
		minutes = re.search(r"(\d+)\s*m", text)
		if hours or minutes:
			return (int(hours.group(1)) * 3600 if hours else 0) \
				+ (int(minutes.group(1)) * 60 if minutes else 0)
		return as_int(text, 0) * 60 if text.isdigit() else 0

	@staticmethod
	def text(value):
		"""A field that may be a list, as plain text without its HTML."""
		if isinstance(value, list):
			value = value[0] if value else ""
		value = re.sub(r"<br\s*/?>|</p>", "\n", str(value or ""),
			flags=re.IGNORECASE)
		return html.unescape(re.sub(r"<[^>]+>", "", value))

	def collection(self, collections):
		"""The collection that says the most about an item."""
		if isinstance(collections, str):
			collections = [collections]
		for name in collections or []:
			if name not in self.GENERIC:
				return name
		return (collections or [""])[0]

	def collection_name(self, identifier):
		if not identifier:
			return ""
		if identifier not in self.collection_names:
			try:
				metadata = self.get_json("/metadata/" + identifier)
				name = self.text(metadata.get("metadata", {}).get("title"))
			except Exception:
				name = ""
			self.collection_names[identifier] = name or identifier
		return self.collection_names[identifier]

	def item(self, document, channel_id=""):
		identifier = document.get("identifier")
		if not identifier:
			return None
		channel_id = channel_id or self.collection(document.get("collection"))
		picture = "%s/services/img/%s" % (self.SITE, identifier)
		return {
			"provider": self.id,
			"kind": "movie",
			"id": identifier,
			"title": clean(self.text(document.get("title")) or identifier),
			"description": clean(self.text(document.get("description"))),
			"channel": clean(self.collection_name(channel_id)),
			"channel_id": channel_id,
			"duration": self.seconds(self.text(document.get("runtime"))),
			"year": as_int(str(document.get("year") or "")[:4], 0),
			"date": "",
			"views": as_int(document.get("downloads"), 0),
			# reviews give 1 to 5 stars
			"rating": float(document.get("avg_rating") or 0) * 2,
			"poster": picture,
			"thumbnail": picture,
			"season": 0,
			"episode": 0,
		}

	def items(self, query, count, channel_id=""):
		response = self.get_json("/advancedsearch.php", [("q", query)]
			+ [("fl[]", field) for field in self.FIELDS]
			+ [("sort[]", "downloads desc"), ("rows", str(count)),
				("output", "json")])
		items = []
		for document in response.get("response", {}).get("docs", []):
			item = self.item(document, channel_id)
			if item is not None:
				items.append(item)
		return items

	# films only, and only those that anybody can watch
	PLAYABLE = " AND mediatype:(movies) AND NOT access-restricted-item:(true)"

	def search(self, query, count):
		words = query.replace('"', " ").strip()
		return self.items("(%s)%s" % (words, self.PLAYABLE), count)

	def channel(self, channel_id, count):
		return self.items("collection:(%s)%s" % (channel_id, self.PLAYABLE),
			count, channel_id)

	def metadata(self, media_id):
		metadata = self.get_json("/metadata/" + media_id)
		return metadata if metadata.get("metadata") else None

	def video_file(self, metadata):
		"""The file to play: the first of FORMATS, the smallest of a kind."""
		files = metadata.get("files") or []
		for wanted in self.FORMATS:
			found = [f for f in files if f.get("format") == wanted]
			if found:
				return min(found, key=lambda f: as_int(f.get("size"), 0))
		for f in files:
			if f.get("name", "").lower().endswith((".mp4", ".ogv", ".webm")):
				return f
		return None

	def file_url(self, media_id, file):
		return "%s/download/%s/%s" % (self.SITE, media_id,
			urllib.parse.quote(file["name"]))

	def details(self, media_id, kind="movie"):
		metadata = self.metadata(media_id)
		if metadata is None:
			return None
		fields = dict(metadata["metadata"], identifier=media_id)
		item = self.item(fields)
		item["description"] = multiline(self.text(
			metadata["metadata"].get("description")))
		date = self.text(metadata["metadata"].get("date"))
		if re.match(r"\d{4}-\d{2}-\d{2}", date):
			item["date"] = date[:10]
		if not item["year"]:
			item["year"] = as_int(date[:4], 0)
		file = self.video_file(metadata)
		if file is not None and not item["duration"]:
			item["duration"] = int(float(file.get("length") or 0))
		return item

	def find(self, media_id, kind="movie"):
		metadata = self.metadata(media_id)
		file = self.video_file(metadata) if metadata else None
		return self.file_url(media_id, file) if file else None

	def download(self, media_id, folder, progress=None):
		metadata = self.metadata(media_id)
		file = self.video_file(metadata) if metadata else None
		if file is None:
			return None
		title = file_name(self.text(metadata["metadata"].get("title"))
			or media_id)
		extension = os.path.splitext(file["name"])[1] or ".mp4"
		path = os.path.join(folder, "%s [%s]%s" % (title, media_id, extension))
		return http_download(self.file_url(media_id, file), path,
			as_int(file.get("size"), 0), progress)


class PeerTubeProvider(Provider):
	"""The videos of PeerTube, the federation of video sites: searched on
	all of them through SepiaSearch, played from the site that has them.
	A video is "<uuid>@<site>", a channel "<name>@<site>".
	"""

	id = "peertube"
	name = "PeerTube"
	kinds = ("search", "channel", "details", "find", "download")

	SEARCH = "https://sepiasearch.org/api/v1/search/videos"

	@staticmethod
	def split(media_id):
		name, _, host = str(media_id).rpartition("@")
		return name, host

	def item(self, video, host=""):
		channel = video.get("channel") or {}
		host = channel.get("host") or host
		if not video.get("uuid") or not host or video.get("isLive") \
				or video.get("nsfw"):
			return None

		def address(url, path):
			if url:
				return url
			return "https://%s%s" % (host, path) if path else ""

		date = str(video.get("publishedAt") or "")[:10]
		return {
			"provider": self.id,
			"kind": "video",
			"id": "%s@%s" % (video["uuid"], host),
			"title": clean(video.get("name") or ""),
			"description": clean(video.get("truncatedDescription")
				or video.get("description") or ""),
			"channel": clean(channel.get("displayName")
				or channel.get("name") or ""),
			"channel_id": "%s@%s" % (channel.get("name"), host)
				if channel.get("name") else "",
			"duration": as_int(video.get("duration"), 0),
			"year": as_int(date[:4], 0),
			"date": date,
			"views": as_int(video.get("views"), 0),
			"rating": 0.0,
			"poster": address(video.get("previewUrl"),
				video.get("previewPath")),
			"thumbnail": address(video.get("thumbnailUrl"),
				video.get("thumbnailPath")),
			"season": 0,
			"episode": 0,
		}

	def items(self, videos, host=""):
		return [item for item in (self.item(video, host) for video in videos)
			if item is not None]

	def search(self, query, count):
		response = http_json(self.SEARCH, {"search": query, "count": count,
			"nsfw": "false", "isLive": "false"})
		return self.items(response.get("data") or [])

	def channel(self, channel_id, count):
		name, host = self.split(channel_id)
		response = http_json("https://%s/api/v1/video-channels/%s/videos"
			% (host, urllib.parse.quote(name)),
			{"count": count, "sort": "-publishedAt", "nsfw": "false"})
		return self.items(response.get("data") or [], host)

	def video(self, media_id):
		uuid, host = self.split(media_id)
		return http_json("https://%s/api/v1/videos/%s" % (host, uuid)), host

	def details(self, media_id, kind="video"):
		video, host = self.video(media_id)
		item = self.item(video, host)
		if item is not None:
			item["description"] = multiline(video.get("description") or "")
		return item

	def video_file(self, video):
		"""The file to play: the biggest up to the height wanted, or the
		smallest when they are all bigger."""
		files = list(video.get("files") or [])
		for playlist in video.get("streamingPlaylists") or []:
			files += playlist.get("files") or []
		files = [f for f in files if f.get("fileUrl")
			and as_int((f.get("resolution") or {}).get("id"), 0) > 0]
		if not files:
			return None

		def height(f):
			return as_int((f.get("resolution") or {}).get("id"), 0)

		fitting = [f for f in files if height(f) <= wanted_height()]
		if fitting:
			return max(fitting, key=height)
		return min(files, key=height)

	def find(self, media_id, kind="video"):
		video, host = self.video(media_id)
		file = self.video_file(video)
		return file["fileUrl"] if file else None

	def download(self, media_id, folder, progress=None):
		video, host = self.video(media_id)
		file = self.video_file(video)
		if file is None:
			return None
		uuid, host = self.split(media_id)
		extension = os.path.splitext(
			urllib.parse.urlparse(file["fileUrl"]).path)[1] or ".mp4"
		path = os.path.join(folder, "%s [%s]%s"
			% (file_name(video.get("name") or uuid), uuid[:8], extension))
		return http_download(file["fileUrl"], path, as_int(file.get("size"), 0),
			progress)


# the programs started for downloads, stopped with the helper: they would
# go on after Cinema otherwise
CHILDREN = set()
CHILDREN_LOCK = threading.Lock()


def stop_children():
	with CHILDREN_LOCK:
		children = list(CHILDREN)
	for process in children:
		try:
			process.terminate()
		except OSError:
			pass
	# aria2 may take a moment to save where it is; not more
	deadline = time.time() + 3
	for process in children:
		try:
			process.wait(max(0.1, deadline - time.time()))
		except Exception:
			try:
				process.kill()
			except OSError:
				pass


class Relay:
	"""Plays what the Media Kit cannot open as it is, such as a video of
	YouTube whose picture and sound are two files: ffmpeg puts them in one
	MPEG-TS stream without decoding them, served on 127.0.0.1.

	The address is http://127.0.0.1:<port>/<token>/<seconds>, the seconds
	being how long the video lasts (0 when not known); ?t=<seconds> starts
	it there, which ffmpeg finds much faster than the Media Kit through
	HTTP. One stream at a time for each address: a new one stops the one
	before.
	"""

	def __init__(self):
		self.lock = threading.Lock()
		self.server = None
		self.sessions = {}		# token: the inputs, [{"url", "headers"}]
		self.playing = {}		# token: the ffmpeg serving it

	def torrent_address(self, session, name):
		"""Where a video of a torrent being downloaded can be played from:
		http://localhost:<port>/torrent/<token>/<name>, not 127.0.0.1, which
		is the relay for Cinema; an ordinary file, which the player reads by
		ranges as it would a video on the web."""
		token = hashlib.sha1(session["id"].encode("utf-8")).hexdigest()[:16]
		with self.lock:
			self.torrents = getattr(self, "torrents", {})
			self.torrents[token] = session
			if self.server is None:
				self.start()
		return "http://localhost:%d/torrent/%s/%s" % (self.server.server_port,
			token, urllib.parse.quote(name))

	def serve_torrent(self, request, head=False):
		"""The bytes of the video asked, as aria2c gets their pieces: what is
		not there yet is waited for, a few minutes at most."""
		token = request.path.split("/")[2] if request.path.count("/") >= 2 \
			else ""
		with self.lock:
			session = getattr(self, "torrents", {}).get(token)
		if session is None:
			request.send_error(404)
			return
		size = session["length"]
		start, end = 0, size - 1
		ranged = request.headers.get("Range", "")
		found = re.match(r"bytes=(\d*)-(\d*)", ranged)
		if found and (found.group(1) or found.group(2)):
			if found.group(1):
				start = int(found.group(1))
				if found.group(2):
					end = min(size - 1, int(found.group(2)))
			else:
				start = max(0, size - int(found.group(2)))
			if start >= size:
				request.send_response(416)
				request.send_header("Content-Range", "bytes */%d" % size)
				request.end_headers()
				return
			request.send_response(206)
			request.send_header("Content-Range", "bytes %d-%d/%d"
				% (start, end, size))
		else:
			request.send_response(200)
		request.send_header("Content-Length", str(end - start + 1))
		request.send_header("Accept-Ranges", "bytes")
		request.send_header("Content-Type", "video/mp4"
			if session["name"].lower().endswith((".mp4", ".m4v", ".mov"))
			else "application/octet-stream")
		request.end_headers()
		if head:
			return
		position = start
		chunk = 256 * 1024
		try:
			while position <= end:
				last = min(end, position + chunk - 1)
				if not torrent_wait(session, position, last):
					return
				data = torrent_read(session, position, last - position + 1)
				if not data:
					return
				request.wfile.write(data)
				position += len(data)
		except (OSError, ValueError):
			# the player went away, or moved to another place
			return

	def address(self, inputs, duration=0):
		key = "\n".join(entry["url"] for entry in inputs)
		token = hashlib.sha1(key.encode("utf-8")).hexdigest()[:16]
		with self.lock:
			self.sessions[token] = inputs
			if self.server is None:
				self.start()
		return "http://127.0.0.1:%d/%s/%d" % (self.server.server_port, token,
			int(duration or 0))

	def start(self):
		relay = self

		class Handler(http.server.BaseHTTPRequestHandler):
			def log_message(self, *arguments):
				pass

			def do_HEAD(self):
				if self.path.startswith("/torrent/"):
					relay.serve_torrent(self, head=True)
					return
				if relay.inputs(self.path) is None:
					self.send_error(404)
					return
				self.send_response(200)
				self.send_header("Content-Type", "video/mp2t")
				self.end_headers()

			def do_GET(self):
				if self.path.startswith("/torrent/"):
					relay.serve_torrent(self)
					return
				relay.serve(self)

		self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
			Handler)
		self.server.daemon_threads = True
		threading.Thread(target=self.server.serve_forever, daemon=True).start()

	def inputs(self, path):
		token = urllib.parse.urlparse(path).path.strip("/").split("/")[0]
		with self.lock:
			return self.sessions.get(token)

	@staticmethod
	def http_options(entry):
		"""The user agent and headers of an input, as ffmpeg takes them."""
		options = []
		headers = dict(entry.get("headers") or {})
		agent = headers.pop("User-Agent", None)
		if agent:
			options += ["-user_agent", agent]
		if headers:
			options += ["-headers", "".join("%s: %s\r\n" % item
				for item in headers.items())]
		return options

	def key_frame(self, entry, start):
		"""The time of the key frame of the picture at or before start, or
		None. Copied without decoding, the picture can only begin there."""
		command = ["ffprobe", "-v", "error"] + self.http_options(entry) + [
			"-select_streams", "v:0", "-read_intervals", "%.3f%%+#1" % start,
			"-show_entries", "packet=pts_time", "-of", "csv=p=0",
			entry["url"]]
		try:
			found = subprocess.run(command, capture_output=True, text=True,
				timeout=30, stdin=subprocess.DEVNULL).stdout.split()
			time = float(found[0].strip(",")) if found else None
		except (OSError, ValueError, subprocess.SubprocessError):
			return None
		if time is None or time < 0 or time > start + 0.001:
			return None
		return time

	def command(self, inputs, start):
		command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin"]
		if start > 0 and len(inputs) > 1:
			# the picture starts at a key frame, seconds before start maybe:
			# the sound starts there too, or it would be that much ahead
			key = self.key_frame(inputs[0], start)
			if key is not None:
				start = key + 0.001
		for entry in inputs:
			command += self.http_options(entry)
			# a connection gone quiet ends ffmpeg, and the stream, rather
			# than holding both forever
			command += ["-rw_timeout", "15000000"]
			# a connection YouTube drops halfway is made again from where
			# it was, rather than ending the stream
			command += ["-reconnect", "1", "-reconnect_on_network_error", "1",
				"-reconnect_delay_max", "5"]
			if start > 0:
				command += ["-ss", "%.3f" % start]
			command += ["-i", entry["url"]]
		if len(inputs) > 1:
			command += ["-map", "0:v:0", "-map", "1:a:0"]
		else:
			command += ["-map", "0:v:0?", "-map", "0:a:0?"]
		return command + ["-c", "copy", "-f", "mpegts", "-"]

	def serve(self, request):
		parts = urllib.parse.urlparse(request.path)
		token = parts.path.strip("/").split("/")[0]
		with self.lock:
			inputs = self.sessions.get(token)
		if inputs is None:
			request.send_error(404)
			return
		query = urllib.parse.parse_qs(parts.query)
		try:
			start = max(0.0, float((query.get("t") or ["0"])[0]))
		except ValueError:
			start = 0.0
		try:
			process = subprocess.Popen(self.command(inputs, start),
				stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
				stdin=subprocess.DEVNULL)
		except OSError:
			request.send_error(500)
			return
		with self.lock:
			before = self.playing.get(token)
			self.playing[token] = process
		with CHILDREN_LOCK:
			CHILDREN.add(process)
		if before is not None:
			self.stop(before)
		try:
			request.send_response(200)
			request.send_header("Content-Type", "video/mp2t")
			request.send_header("Connection", "close")
			request.end_headers()
			while True:
				data = process.stdout.read(65536)
				if not data:
					break
				request.wfile.write(data)
		except (OSError, ValueError):
			# the player went away, or another stream took its place
			pass
		finally:
			self.stop(process)
			with self.lock:
				if self.playing.get(token) is process:
					del self.playing[token]

	def stop(self, process):
		try:
			process.kill()
			process.wait(5)
		except Exception:
			pass
		with CHILDREN_LOCK:
			CHILDREN.discard(process)


RELAY = Relay()


class IptvProvider(Provider):
	"""Television channels, live, from a playlist M3U: an address or a
	file. A channel of Cinema is the playlist, a video one of its streams,
	which Cinema plays with FFmpeg. The free channels of each country that
	iptv-org lists are offered; other lists the user adds.
	"""

	id = "iptv"
	name = "TV"
	kinds = ("channel", "find")
	# read again each time: the streams of a list change often, and the
	# headers they need are kept here
	cache_channels = False

	MAX_CHANNELS = 500
	ATTRIBUTE = re.compile(r'([\w-]+)="([^"]*)"')
	ADULT = re.compile(r"\b(xxx|adult|nsfw)", re.IGNORECASE)
	NOTES = re.compile(r"\s*(\(\d+[pi]\)|\[[^\]]*\])")

	def __init__(self):
		self.headers = {}	# stream: the HTTP headers it asks for

	def read(self, source, headers=None, limit=20 * 1024 * 1024):
		return self.fetch(source, headers, limit)[0]

	def fetch(self, source, headers=None, limit=20 * 1024 * 1024):
		"""The text of a list, and where it was in the end: what it names
		is from there."""
		if source.startswith(("http://", "https://")):
			request = urllib.request.Request(source,
				headers=dict({"User-Agent": USER_AGENT}, **(headers or {})))
			with urllib.request.urlopen(request, timeout=30) as response:
				data = response.read(limit)
				source = response.geturl()
		else:
			with open(os.path.expanduser(source), "rb") as file:
				data = file.read(limit)
		return data.decode("utf-8", "replace"), source

	def channel(self, channel_id, count):
		items = []
		extinf = None
		options = {}
		for line in self.read(channel_id).splitlines():
			line = line.strip()
			if line.startswith("#EXTINF"):
				# #EXTINF:-1 tvg-logo="..." group-title="...",Name
				quote = line.rfind('"')
				comma = line.find(",", quote + 1 if quote >= 0 else 0)
				extinf = (dict(self.ATTRIBUTE.findall(line[:comma])),
					line[comma + 1:].strip() if comma >= 0 else "")
				options = {}
			elif line.startswith("#EXTVLCOPT:"):
				key, _, value = line[len("#EXTVLCOPT:"):].partition("=")
				options[key.strip()] = value.strip()
			elif line and not line.startswith("#") and extinf is not None:
				attributes, title = extinf
				extinf = None
				# Cinema may be on a computer for children
				if self.ADULT.search(attributes.get("group-title", "")):
					continue
				url = urllib.parse.urljoin(channel_id, line) \
					if channel_id.startswith(("http://", "https://")) else line
				if not url.startswith(("http://", "https://")):
					continue
				headers = {}
				agent = options.get("http-user-agent") \
					or attributes.get("http-user-agent")
				referrer = options.get("http-referrer") \
					or attributes.get("http-referrer")
				if agent:
					headers["User-Agent"] = agent
				if referrer:
					headers["Referer"] = referrer
				self.headers[url] = headers
				# "Name (720p) [Geo-blocked]": the name, and the rest told
				title = title or attributes.get("tvg-name") or url
				notes = self.NOTES.findall(title)
				title = self.NOTES.sub("", title).strip() or title
				group = attributes.get("group-title", "")
				if group.lower() == "undefined":
					group = ""
				items.append({
					"provider": self.id,
					"kind": "live",
					"id": url,
					"title": clean(title),
					"description": " · ".join(note.strip("()[] ")
						for note in notes),
					"channel": clean(group.replace(";", ", ")),
					"channel_id": "",
					"duration": 0,
					"year": 0,
					"date": "",
					"views": 0,
					"rating": 0.0,
					"poster": "",
					"thumbnail": attributes.get("tvg-logo", ""),
					"season": 0,
					"episode": 0,
				})
				if len(items) >= self.MAX_CHANNELS:
					break
		return items

	def find(self, media_id, kind="live"):
		"""Cinema plays it with FFmpeg, which opens HLS by itself: the
		stream, the headers the list asks for and the height wanted."""
		headers = self.headers.get(media_id, {})
		return "cinema-live:?" + urllib.parse.urlencode({
			"url": media_id,
			"ua": headers.get("User-Agent", ""),
			"referer": headers.get("Referer", ""),
			"height": wanted_height(),
		})


class FavoritesProvider(Provider):
	"""What was given a star in Cinema, of any source: the file Cinema
	writes in the settings, Cinema/favorites, with MediaItems as the other
	providers give them. Its only channel is "all"; a favorite is played
	by the provider it came from.
	"""

	id = "favorites"
	name = "Favorites"
	kinds = ("channel",)
	cache_channels = False

	FIELDS = {"provider": "", "kind": "video", "id": "", "title": "",
		"description": "", "channel": "", "channel_id": "", "duration": 0,
		"year": 0, "date": "", "views": 0, "rating": 0.0, "poster": "",
		"thumbnail": "", "season": 0, "episode": 0}

	def channel(self, channel_id, count):
		path = os.path.join(os.path.expanduser("~"), "config", "settings",
			"Cinema", "favorites")
		try:
			with open(path, encoding="utf-8") as file:
				favorites = json.load(file)
		except (OSError, ValueError):
			return []
		items = []
		for favorite in favorites if isinstance(favorites, list) else []:
			if not isinstance(favorite, dict) or not favorite.get("id"):
				continue
			item = dict(self.FIELDS)
			item.update({key: favorite[key] for key in self.FIELDS
				if key in favorite})
			items.append(item)
		return items


def stop_others(work):
	"""Stops the aria2c left working in work by a helper that went away:
	two of them on the same files would spoil them."""
	try:
		lines = subprocess.run(["ps"], capture_output=True, text=True,
			timeout=10).stdout.splitlines()
	except (OSError, subprocess.SubprocessError):
		return
	mine = set()
	with CHILDREN_LOCK:
		mine = {process.pid for process in CHILDREN}
	for line in lines:
		if "aria2c" not in line or "--dir=" + work not in line:
			continue
		# the team id comes before the threads and the two times
		fields = line.split()
		if len(fields) < 4 or not fields[-4].isdigit():
			continue
		pid = int(fields[-4])
		if pid in mine:
			continue
		try:
			os.kill(pid, 15)
			time.sleep(2)
			os.kill(pid, 9)
		except OSError:
			pass


VIDEO_EXTENSIONS = (".mp4", ".m4v", ".mkv", ".webm", ".avi", ".mov", ".ogv",
	".mpg", ".mpeg", ".wmv", ".flv", ".ts")


# the torrents being downloaded, for playing them meanwhile: their aria2c's
# RPC port and secret, and where the video went once done ("final")
TORRENTS = {}
TORRENTS_LOCK = threading.Lock()


def free_port():
	with socket.socket() as probe:
		probe.bind(("127.0.0.1", 0))
		return probe.getsockname()[1]


def aria2_call(stream, method, *params):
	"""A call to the RPC of an aria2c; its result, or None."""
	body = json.dumps({"jsonrpc": "2.0", "id": "cinema", "method": method,
		"params": ["token:" + stream["secret"]] + list(params)}).encode()
	request = urllib.request.Request(
		"http://127.0.0.1:%d/jsonrpc" % stream["port"], data=body,
		headers={"Content-Type": "application/json"})
	try:
		with urllib.request.urlopen(request, timeout=5) as response:
			return json.load(response).get("result")
	except Exception:
		return None


def torrent_watch(stream, process):
	"""aria2c with its RPC on stays when there is nothing left to get: told
	to go then, twice seen idle so as not to stop it between the .torrent
	and what it shares."""
	idle = 0
	while process.poll() is None:
		time.sleep(2)
		stat = aria2_call(stream, "aria2.getGlobalStat")
		if not stat:
			continue
		if stat.get("numActive") == "0" and stat.get("numWaiting") == "0":
			idle += 1
			if idle >= 2:
				aria2_call(stream, "aria2.shutdown")
				return
		else:
			idle = 0


def torrent_complete(work):
	"""Whether what aria2c got in work is whole: no control file left."""
	got = False
	for directory, _, names in os.walk(work):
		for name in names:
			if name.endswith(".aria2"):
				return False
			if name.lower().endswith(VIDEO_EXTENSIONS):
				got = True
	return got


def torrent_session(media_id, stream):
	"""The biggest video of the torrent aria2c is getting, once it knows its
	files: where it is, where it starts among the pieces, how big they are.
	None while the files are not known yet."""
	if stream.get("final"):
		path = stream["final"]
		return {"id": media_id, "stream": stream, "path": path,
			"name": os.path.basename(path), "offset": 0,
			"length": os.path.getsize(path), "piece": 1, "gid": None}
	keys = ["gid", "files", "pieceLength", "status"]
	downloads = (aria2_call(stream, "aria2.tellActive", keys) or []) \
		+ (aria2_call(stream, "aria2.tellStopped", 0, 20, keys) or [])
	best = None
	for download in downloads:
		offset = 0
		for entry in download.get("files") or []:
			length = as_int(entry.get("length"), 0)
			path = entry.get("path") or ""
			if path.lower().endswith(VIDEO_EXTENSIONS) and length > 0 \
					and (best is None or length > best["length"]):
				best = {"id": media_id, "stream": stream, "path": path,
					"name": os.path.basename(path), "offset": offset,
					"length": length, "gid": download.get("gid"),
					"piece": max(1, as_int(download.get("pieceLength"), 1))}
			offset += length
	return best


def torrent_ready(session, start, end):
	"""Whether the pieces of the bytes from start to end are there."""
	stream = session["stream"]
	if stream.get("final") or session["gid"] is None:
		return True
	now = time.time()
	if now - session.get("asked", 0) > 0.5:
		status = aria2_call(stream, "aria2.tellStatus", session["gid"],
			["bitfield", "status"])
		session["asked"] = now
		if status:
			session["bitfield"] = status.get("bitfield") or ""
			session["status"] = status.get("status") or ""
	if session.get("status") == "complete":
		return True
	bits = session.get("bitfield") or ""
	first = (session["offset"] + start) // session["piece"]
	last = (session["offset"] + end) // session["piece"]
	for piece in range(first, last + 1):
		digit = piece // 4
		if digit >= len(bits) or not (int(bits[digit], 16)
				>> (3 - piece % 4)) & 1:
			return False
	return True


def torrent_map(media_id, slots=160):
	"""How much of the video of a torrent is there, from its start to its
	end, in slots: a digit each, 0 nothing to 9 all; "" when not known.
	For the bar of the player, which shows it."""
	with TORRENTS_LOCK:
		stream = TORRENTS.get(media_id)
	if not stream:
		return ""
	if stream.get("final"):
		return "9" * slots
	session = stream.get("map session")
	if not session:
		session = torrent_session(media_id, stream)
		if not session:
			return ""
		stream["map session"] = session
	status = aria2_call(stream, "aria2.tellStatus", session["gid"],
		["bitfield", "status"])
	if not status:
		return ""
	if status.get("status") == "complete":
		return "9" * slots
	bits = status.get("bitfield") or ""
	piece, offset, length = session["piece"], session["offset"], \
		session["length"]
	digits = []
	for slot in range(slots):
		start = offset + length * slot // slots
		end = max(start, offset + length * (slot + 1) // slots - 1)
		first, last = start // piece, end // piece
		have = 0
		for number in range(first, last + 1):
			digit = number // 4
			if digit < len(bits) \
					and (int(bits[digit], 16) >> (3 - number % 4)) & 1:
				have += 1
		digits.append(str(have * 9 // (last - first + 1)))
	return "".join(digits)


def torrent_wait(session, start, end, most=600):
	"""Until those bytes are there; False if they do not come, or the
	download is gone."""
	deadline = time.time() + most
	while time.time() < deadline:
		if torrent_ready(session, start, end):
			return True
		if session.get("status") in ("error", "removed"):
			return False
		time.sleep(0.3)
	return False


def torrent_read(session, position, count):
	path = session["stream"].get("final") or session["path"]
	try:
		with open(path, "rb") as file:
			file.seek(position)
			return file.read(count)
	except OSError:
		return b""


class TorrentProvider(Provider):
	"""Downloads what a magnet link or a .torrent file (a path or an
	address) shares, with aria2c, and keeps its biggest video. The id is
	the link, the path or the address itself.
	"""

	id = "torrent"
	name = "BitTorrent"
	kinds = ("download", "find")

	# [#2089b0 400KiB/33MiB(1%) CN:44 SD:3 DL:115KiB ETA:4m25s]
	SUMMARY = re.compile(r"\[#\w+ (?:SEED\S* )?([\d.]+\w*B)/([\d.]+\w*B)"
		r"\((\d+)%\)(.*?)\]")

	@staticmethod
	def sources(media_id):
		"""What to give aria2c: the .torrent a magnet link points to with
		xs= first, which brings the files at once and the web seeds of the
		site, then the link itself."""
		found = []
		if media_id.startswith("magnet:"):
			query = urllib.parse.parse_qs(urllib.parse.urlsplit(
				media_id).query)
			for source in query.get("xs", []):
				if source.startswith(("http://", "https://")) \
						and source.endswith(".torrent"):
					found.append(source)
		found.append(media_id)
		return found

	def find(self, media_id, kind="video"):
		"""A video of a torrent while it downloads: the address the relay
		plays it from, once aria2c knows its files."""
		deadline = time.time() + 180
		while time.time() < deadline:
			with TORRENTS_LOCK:
				stream = TORRENTS.get(media_id)
			if stream is not None:
				session = torrent_session(media_id, stream)
				if session is not None:
					return RELAY.torrent_address(session, session["name"])
			elif media_id not in RUNNING and time.time() > deadline - 150:
				# no download of it after half a minute: nothing to play
				return None
			time.sleep(1)
		return None

	def run(self, media_id, source, work, progress):
		"""aria2c on one source, until it is done or gives up: its exit
		status, and whether anything came."""
		# its pieces in order, the start and the end first, and a local RPC
		# Cinema asks which pieces it has, so that the video plays while it
		# comes
		stream = {"port": free_port(),
			"secret": hashlib.sha1(os.urandom(16)).hexdigest()}
		command = ["aria2c", "--dir=" + work, "--seed-time=0",
			"--summary-interval=1", "--console-log-level=error",
			"--follow-torrent=mem", "--file-allocation=none",
			"--continue=true", "--bt-save-metadata=true",
			"--bt-load-saved-metadata=true", "--enable-dht=true",
			"--auto-file-renaming=false",
			"--stream-piece-selector=inorder",
			"--bt-prioritize-piece=head=8M,tail=2M",
			"--enable-rpc=true", "--rpc-listen-all=false",
			"--rpc-listen-port=%d" % stream["port"],
			"--rpc-secret=" + stream["secret"],
			# give up after ten minutes with nothing coming
			"--bt-stop-timeout=600", "--connect-timeout=30",
			source]
		process = subprocess.Popen(command, stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
			text=True, errors="replace")
		with CHILDREN_LOCK:
			CHILDREN.add(process)
			RUNNING[media_id] = process
		with TORRENTS_LOCK:
			TORRENTS[media_id] = stream
		# with RPC on, aria2c stays when it is done: told to go then
		threading.Thread(target=torrent_watch, args=(stream, process),
			daemon=True).start()
		got = False
		mapped = 0.0
		try:
			for line in process.stdout:
				found = self.SUMMARY.search(line)
				if found and int(found.group(3)) > 0:
					got = True
				if found and progress is not None:
					rest = found.group(4)

					def field(name):
						value = re.search(name + r":(\S+)", rest)
						return value.group(1) if value else ""

					# what of the video is there, every two seconds
					extra = {}
					if time.time() - mapped >= 2:
						mapped = time.time()
						try:
							extra["map"] = torrent_map(media_id)
						except Exception:
							pass
					progress(int(found.group(3)), done=found.group(1),
						total=found.group(2), speed=field("DL"),
						peers=as_int(field("SD"), -1)
							if field("SD") else as_int(field("CN"), -1),
						eta=field("ETA"), **extra)
		except Exception:
			process.terminate()
			try:
				process.wait(5)
			except Exception:
				process.kill()
			with CHILDREN_LOCK:
				CHILDREN.discard(process)
			raise
		finally:
			with CHILDREN_LOCK:
				RUNNING.pop(media_id, None)
		status = process.wait()
		with CHILDREN_LOCK:
			CHILDREN.discard(process)
		return status, got

	def download(self, media_id, folder, progress=None):
		key = hashlib.sha1(media_id.encode()).hexdigest()[:12]
		# the same folder for the same link, so that an interrupted
		# download goes on from where it was
		work = os.path.join(folder, ".torrent-" + key)
		os.makedirs(work, exist_ok=True)
		stop_others(work)
		complete = False
		try:
			status = 1
			for source in self.sources(media_id):
				if media_id in CANCELLED or media_id in PAUSED:
					break
				status, got = self.run(media_id, source, work, progress)
				# done, or begun: the next source is for when this one
				# brought nothing
				if (status == 0 and torrent_complete(work)) or got:
					break
			if status != 0 or not torrent_complete(work):
				return None
			complete = True

			videos = []
			for directory, _, names in os.walk(work):
				for name in names:
					if name.lower().endswith(VIDEO_EXTENSIONS):
						path = os.path.join(directory, name)
						videos.append((os.path.getsize(path), path))
			if not videos:
				return None
			source = max(videos)[1]
			stem, extension = os.path.splitext(os.path.basename(source))
			# a folder of their own, which the library always reads
			torrents = os.path.join(folder, "Torrent")
			os.makedirs(torrents, exist_ok=True)
			path = os.path.join(torrents, "%s [%s]%s"
				% (file_name(stem), key[:8], extension))
			shutil.move(source, path)
			# a video played meanwhile goes on from where it is now
			with TORRENTS_LOCK:
				if media_id in TORRENTS:
					TORRENTS[media_id]["final"] = path
			return path
		finally:
			# kept when interrupted, to go on later; not when it is done or
			# was stopped on purpose
			if complete or media_id in CANCELLED:
				shutil.rmtree(work, ignore_errors=True)


	def discard(self, media_id, folder):
		"""What a download stopped for good had got."""
		key = hashlib.sha1(media_id.encode()).hexdigest()[:12]
		shutil.rmtree(os.path.join(folder, ".torrent-" + key),
			ignore_errors=True)


class LocalProvider(Provider):
	"""The videos in a folder of this computer: a channel is the folder,
	a video its path. Their length and a picture come from ffprobe and
	ffmpeg when they are there, and are kept in the cache.
	"""

	id = "local"
	name = "Files"
	kinds = ("channel", "details", "find")

	DEPTH = 4
	LIMIT = 2000
	# what is not known yet is found this long, the rest the next time
	BUDGET = 6

	def __init__(self):
		self.lock = threading.Lock()
		self.known = {}
		self.path = os.path.join(cache, "local.json") if cache else None
		try:
			with open(self.path, encoding="utf-8") as file:
				self.known = json.load(file)
			if not isinstance(self.known, dict):
				self.known = {}
		except (OSError, ValueError, TypeError, RecursionError):
			# broken: made again
			self.known = {}

	def save(self):
		if not self.path:
			return
		with self.lock:
			try:
				temporary = self.path + ".part.%d" % threading.get_ident()
				with open(temporary, "w", encoding="utf-8") as file:
					json.dump(self.known, file)
				os.replace(temporary, self.path)
			except OSError:
				pass

	def probe(self, path, key):
		"""The length of a video and the path of a picture of it."""
		duration = 0
		picture = ""
		try:
			result = subprocess.run(["ffprobe", "-v", "error",
				"-show_entries", "format=duration", "-of",
				"default=noprint_wrappers=1:nokey=1", path],
				capture_output=True, text=True, timeout=30)
			duration = int(float(result.stdout.strip() or 0))
		except (OSError, ValueError, subprocess.SubprocessError):
			pass
		if cache:
			picture = os.path.join(cache, "local-%s.jpg"
				% hashlib.sha1(key.encode()).hexdigest()[:16])
			try:
				subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss",
					str(min(60, max(1, duration // 10))), "-i", path,
					"-frames:v", "1", "-vf", "scale=320:-2", picture],
					capture_output=True, timeout=60)
			except (OSError, subprocess.SubprocessError):
				pass
			if not os.path.exists(picture):
				picture = ""
		return {"duration": duration, "picture": picture}

	def videos(self, folder):
		found = []
		folder = os.path.abspath(folder)
		base = folder.count(os.sep)
		for directory, directories, names in os.walk(folder):
			if directory.count(os.sep) - base >= self.DEPTH:
				directories[:] = []
			directories[:] = [d for d in directories if not d.startswith(".")]
			for name in names:
				if name.lower().endswith(VIDEO_EXTENSIONS) \
						and not name.startswith("."):
					path = os.path.join(directory, name)
					try:
						found.append((os.path.getmtime(path), path))
					except OSError:
						pass
		found.sort(reverse=True)
		return [path for _, path in found[:self.LIMIT]]

	def item(self, path, wait):
		try:
			info = os.stat(path)
		except OSError:
			return None
		key = "%s %d %d" % (path, info.st_size, int(info.st_mtime))
		known = self.known.get(key)
		if known is None and wait:
			known = self.probe(path, key)
			with self.lock:
				self.known[key] = known
		known = known or {}
		picture = known.get("picture") or ""
		return {
			"provider": self.id,
			"kind": "video",
			"id": path,
			"title": clean(os.path.splitext(os.path.basename(path))[0]),
			"description": path,
			"channel": clean(os.path.basename(os.path.dirname(path))),
			"channel_id": os.path.dirname(path),
			"duration": as_int(known.get("duration"), 0),
			"year": time.localtime(info.st_mtime).tm_year,
			"date": time.strftime("%Y-%m-%d", time.localtime(info.st_mtime)),
			"views": 0,
			"rating": 0.0,
			"poster": "file://" + picture if picture else "",
			"thumbnail": "file://" + picture if picture else "",
			"season": 0,
			"episode": 0,
		}

	def channel(self, channel_id, count):
		start = time.time()
		items = []
		late = []
		for path in self.videos(channel_id):
			wait = time.time() - start < self.BUDGET
			item = self.item(path, wait)
			if item is not None:
				items.append(item)
				if not wait:
					late.append(path)
		if late:
			# what was not looked at yet, for the next time
			def finish():
				for path in late:
					self.item(path, True)
				self.save()
			threading.Thread(target=finish, daemon=True).start()
		self.save()
		return items

	def details(self, media_id, kind="video"):
		return self.item(media_id, True)

	def find(self, media_id, kind="video"):
		return media_id if os.path.isfile(media_id) else None


class DlnaProvider(Provider):
	"""The video servers of the home network: DLNA (UPnP AV), as a
	FRITZ!Box, a NAS, Jellyfin or Plex with DLNA on give. The channel
	"servers" is the servers found; a server or one of its folders is a
	"series", which Cinema opens as a channel; a video plays from the
	address the server gives for it, by HTTP. An id is "dlna|<address of
	its ContentDirectory>|<object id>".

	Found with SSDP: once at the start, in the background, and again each
	time the list of the servers is opened.
	"""

	id = "dlna"
	name = "Home network"
	kinds = ("channel", "find", "download")
	cache_channels = False

	SEARCH = "urn:schemas-upnp-org:device:MediaServer:1"
	DIRECTORY = "urn:schemas-upnp-org:service:ContentDirectory:1"
	NAMES = {"didl": "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/",
		"dc": "http://purl.org/dc/elements/1.1/",
		"upnp": "urn:schemas-upnp-org:metadata-1-0/upnp/"}
	PAGE = 200
	LIMIT = 2000

	def __init__(self):
		self.lock = threading.Lock()
		self.servers = {}	# the address of its ContentDirectory: its name
		self.urls = {}		# id: the address of the video
		self.titles = {}	# id: its title, for the name of a download

	def discover(self, wait=3.0):
		"""The servers that answer in wait seconds, added to those known."""
		locations = set()
		try:
			sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,
				socket.IPPROTO_UDP)
			sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
			sock.settimeout(0.5)
			message = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
				"MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n"
				% self.SEARCH).encode()
			for _ in range(2):
				sock.sendto(message, ("239.255.255.250", 1900))
			end = time.time() + wait
			while time.time() < end:
				try:
					data, _ = sock.recvfrom(8192)
				except socket.timeout:
					continue
				found = re.search(rb"(?im)^LOCATION:\s*(\S+)", data)
				if found:
					locations.add(found.group(1).decode("latin-1"))
			sock.close()
		except OSError:
			pass
		for location in locations:
			try:
				self.describe_server(location)
			except Exception:
				pass
		with self.lock:
			return dict(self.servers)

	def describe_server(self, location):
		import xml.etree.ElementTree as tree
		request = urllib.request.Request(location,
			headers={"User-Agent": USER_AGENT})
		with urllib.request.urlopen(request, timeout=5) as response:
			root = tree.fromstring(response.read(1024 * 1024))
		def local(element):
			return element.tag.rsplit("}", 1)[-1]
		name = ""
		base = location
		for element in root.iter():
			if local(element) == "friendlyName" and not name:
				name = (element.text or "").strip()
			elif local(element) == "URLBase" and (element.text or "").strip():
				base = element.text.strip()
		for service in root.iter():
			if local(service) != "service":
				continue
			fields = {local(child): (child.text or "").strip()
				for child in service}
			if fields.get("serviceType", "").startswith(
					"urn:schemas-upnp-org:service:ContentDirectory:"):
				control = urllib.parse.urljoin(base,
					fields.get("controlURL", ""))
				with self.lock:
					self.servers[control] = name or location
				return

	def browse(self, control, object_id, start, count, flag="BrowseDirectChildren"):
		"""The DIDL-Lite of the children of an object (or of the object
		itself with BrowseMetadata), and how many there are in all."""
		body = ('<?xml version="1.0" encoding="utf-8"?>'
			'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
			's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
			'<s:Body><u:Browse xmlns:u="%s"><ObjectID>%s</ObjectID>'
			'<BrowseFlag>%s</BrowseFlag><Filter>*</Filter>'
			'<StartingIndex>%d</StartingIndex>'
			'<RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria>'
			'</u:Browse></s:Body></s:Envelope>') % (self.DIRECTORY,
			html.escape(object_id), flag, start, count)
		request = urllib.request.Request(control, data=body.encode("utf-8"),
			headers={"Content-Type": 'text/xml; charset="utf-8"',
				"SOAPACTION": '"%s#Browse"' % self.DIRECTORY,
				"User-Agent": USER_AGENT})
		with urllib.request.urlopen(request, timeout=15) as response:
			text = response.read(16 * 1024 * 1024).decode("utf-8", "replace")
		result = re.search(r"<Result[^>]*>(.*?)</Result>", text, re.S)
		total = re.search(r"<TotalMatches>(\d+)</TotalMatches>", text)
		return (html.unescape(result.group(1)) if result else "",
			int(total.group(1)) if total else 0)

	@staticmethod
	def seconds(duration):
		"""'1:23:45.678' in seconds."""
		try:
			parts = [float(part) for part in duration.split(":")]
		except ValueError:
			return 0
		total = 0
		for part in parts:
			total = total * 60 + part
		return int(total)

	def items(self, control, didl, server, folder):
		import xml.etree.ElementTree as tree
		try:
			root = tree.fromstring(didl)
		except tree.ParseError:
			return []
		found = []
		names = self.NAMES
		for element in root:
			tag = element.tag.rsplit("}", 1)[-1]
			object_id = element.get("id") or ""
			title = clean(element.findtext("dc:title", "", names) or "")
			kind = element.findtext("upnp:class", "", names) or ""
			media_id = "dlna|%s|%s" % (control, object_id)
			item = {"provider": self.id, "id": media_id, "title": title,
				"description": "", "channel": folder or server,
				"channel_id": "", "duration": 0, "year": 0, "views": 0,
				"rating": 0.0, "poster": "", "thumbnail": "", "season": 0,
				"episode": 0}
			art = element.findtext("upnp:albumArtURI", "", names) or ""
			if tag == "container":
				item["kind"] = "series"
				item["description"] = server
			elif tag == "item" and kind.startswith("object.item.videoItem"):
				url = ""
				for res in element.findall("didl:res", names):
					protocol = res.get("protocolInfo") or ""
					if protocol.startswith("http-get:") and ":video/" in protocol:
						url = (res.text or "").strip()
						item["duration"] = self.seconds(res.get("duration") or "")
						break
				if not url:
					continue
				with self.lock:
					self.urls[media_id] = url
					self.titles[media_id] = title
				item["kind"] = "video"
				date = element.findtext("dc:date", "", names) or ""
				if date[:4].isdigit():
					item["year"] = int(date[:4])
					item["date"] = date[:10]
			else:
				continue
			if art.startswith("http"):
				item["thumbnail"] = item["poster"] = art
			found.append(item)
		return found

	def channel(self, channel_id, count):
		if channel_id in ("", "servers"):
			servers = self.discover()
			return [{"provider": self.id, "kind": "series",
				"id": "dlna|%s|0" % control, "title": clean(name),
				"description": urllib.parse.urlparse(control).hostname or "",
				"channel": "", "channel_id": "", "duration": 0, "year": 0,
				"views": 0, "rating": 0.0, "poster": "", "thumbnail": "",
				"season": 0, "episode": 0}
				for control, name in sorted(servers.items(),
					key=lambda entry: entry[1].lower())]
		parts = channel_id.split("|", 2)
		if len(parts) != 3 or parts[0] != "dlna":
			return []
		control, object_id = parts[1], parts[2]
		with self.lock:
			server = self.servers.get(control, "")
		found = []
		start = 0
		while start < self.LIMIT:
			didl, total = self.browse(control, object_id, start, self.PAGE)
			page = self.items(control, didl, server, "")
			found += page
			start += self.PAGE
			if not didl or start >= total:
				break
		return found

	def find(self, media_id, kind="video"):
		with self.lock:
			url = self.urls.get(media_id)
		if url:
			return url
		parts = media_id.split("|", 2)
		if len(parts) != 3:
			return None
		try:
			didl, _ = self.browse(parts[1], parts[2], 0, 1, "BrowseMetadata")
			self.items(parts[1], didl, "", "")
		except Exception:
			return None
		with self.lock:
			return self.urls.get(media_id)


	def download(self, media_id, folder, progress=None):
		"""The video copied from its server into folder, at the speed of
		the network: then it plays from the disk."""
		url = self.find(media_id)
		if not url:
			return None
		with self.lock:
			title = self.titles.get(media_id) or "video"
		extension = os.path.splitext(urllib.parse.urlparse(url).path)[1]
		if not re.fullmatch(r"\.[A-Za-z0-9]{1,5}", extension or ""):
			extension = ".mp4"
		os.makedirs(folder, exist_ok=True)
		path = os.path.join(folder, "%s [%s]%s" % (file_name(title),
			short_id(media_id), extension))
		return http_download(url, path, progress=progress)


class PodcastProvider(Provider):
	"""Podcasts, audio or video, from their feed RSS: a channel is the
	address of the feed, an episode the address of its file, which Cinema
	plays with FFmpeg (an audio one with its picture) or downloads. What
	the podcast is (its name, picture, description) comes with the list.
	"""

	id = "podcast"
	name = "Podcasts"
	kinds = ("channel", "find", "download")
	# new episodes come: read again, but not more than every 15 minutes
	cache_channels = False
	FRESH = 15 * 60
	ITUNES = "{http://www.itunes.com/dtds/podcast-1.0.dtd}"
	MEDIA = "{http://search.yahoo.com/mrss/}"

	def __init__(self):
		self.lock = threading.Lock()
		self.feeds = {}		# address: (when, its text)
		self.titles = {}	# the address of an episode: its title

	def feed(self, address):
		with self.lock:
			kept = self.feeds.get(address)
		if kept and time.time() - kept[0] < self.FRESH:
			return kept[1]
		request = urllib.request.Request(address,
			headers={"User-Agent": USER_AGENT})
		with urllib.request.urlopen(request, timeout=30) as response:
			text = response.read(20 * 1024 * 1024)
		with self.lock:
			self.feeds[address] = (time.time(), text)
		return text

	@staticmethod
	def seconds(text):
		"""'1:02:03', '62:03' or '3723' in seconds."""
		try:
			total = 0
			for part in str(text or "").strip().split(":"):
				total = total * 60 + float(part)
			return int(total)
		except ValueError:
			return 0

	@staticmethod
	def plain(text):
		"""The text of a description in HTML."""
		text = re.sub(r"(?i)<br\s*/?>|</p>", "\n", text or "")
		return html.unescape(re.sub(r"<[^>]+>", "", text)).strip()

	def channel(self, channel_id, count):
		import xml.etree.ElementTree as tree
		import email.utils
		root = tree.fromstring(self.feed(channel_id))
		feed = root.find("channel")
		if feed is None:
			return []
		name = clean(feed.findtext("title") or "")
		picture = ""
		image = feed.find(self.ITUNES + "image")
		if image is not None:
			picture = image.get("href") or ""
		if not picture:
			picture = feed.findtext("image/url") or ""
		keep_about(self.id, channel_id, {"name": name, "handle": "",
			"followers": 0, "avatar": picture,
			"description": clean(self.plain(feed.findtext("description")))})
		items = []
		for entry in feed.findall("item"):
			enclosure = entry.find("enclosure")
			url = enclosure.get("url") if enclosure is not None else ""
			kind = (enclosure.get("type") or "") if enclosure is not None else ""
			if not url:
				content = entry.find(self.MEDIA + "content")
				if content is not None:
					url = content.get("url") or ""
					kind = content.get("type") or ""
			if not url or not (kind.startswith(("audio/", "video/"))
					or url.lower().split("?")[0].endswith(
						(".mp3", ".m4a", ".aac", ".ogg", ".opus", ".mp4",
						".m4v", ".mov", ".webm"))):
				continue
			title = clean(entry.findtext("title") or "")
			with self.lock:
				self.titles[url] = title
			date = ""
			year = 0
			try:
				when = email.utils.parsedate_to_datetime(
					entry.findtext("pubDate") or "")
				date = when.strftime("%Y-%m-%d")
				year = when.year
			except (TypeError, ValueError):
				pass
			art = entry.find(self.ITUNES + "image")
			thumbnail = (art.get("href") if art is not None else "") or picture
			description = entry.findtext(self.ITUNES + "summary") \
				or entry.findtext("description") or ""
			items.append({"provider": self.id, "kind": "video", "id": url,
				"title": title, "description": clean(self.plain(description)),
				"channel": name, "channel_id": channel_id,
				"duration": self.seconds(
					entry.findtext(self.ITUNES + "duration")),
				"year": year, "date": date, "views": 0, "rating": 0.0,
				"poster": thumbnail, "thumbnail": thumbnail, "season": 0,
				"episode": 0})
			if len(items) >= max(count, 200):
				break
		return items

	def find(self, media_id, kind="video"):
		return media_id if media_id.startswith(("http://", "https://")) \
			else None

	def download(self, media_id, folder, progress=None):
		"""The file of an episode, as the server gives it."""
		with self.lock:
			title = self.titles.get(media_id) or ""
		address = urllib.parse.urlparse(media_id).path
		extension = os.path.splitext(address)[1]
		if not re.fullmatch(r"\.[A-Za-z0-9]{1,5}", extension or ""):
			extension = ".mp3"
		name = file_name(title or os.path.basename(address) or "podcast")
		os.makedirs(folder, exist_ok=True)
		path = os.path.join(folder, "%s [%s]%s" % (name, short_id(media_id),
			extension))
		return http_download(media_id, path, progress=progress)


def look_for_servers():
	"""At the start: the servers of the home network, and if there are any
	the source that shows them, told to Cinema."""
	dlna = DlnaProvider()
	if dlna.discover():
		PROVIDERS[dlna.id] = dlna
		answer({"type": "providers", "providers":
			[provider.describe() for provider in PROVIDERS.values()]})


if yt_dlp is not None:
	register(YouTubeProvider)
	DEFAULT_PROVIDER = "youtube"
else:
	DEFAULT_PROVIDER = "archive"
register(ArchiveProvider)
register(PeerTubeProvider)
if shutil.which("aria2c"):
	register(TorrentProvider)
register(LocalProvider)
register(IptvProvider)
register(PodcastProvider)
register(FavoritesProvider)


class LibraryProvider(Provider):
	"""The films and the series in the folders of the local tabs, known by
	the names of their files and, with a key, by TMDB: posters, plots,
	casts, the names of the episodes.

	Its channels are "movies", "series", and "series:<key>" for the
	episodes of one; a film or an episode is its path, a series
	"series:<key>". The key of TMDB is in the settings, Cinema/tmdb_key.
	"""

	id = "library"
	name = "Library"
	kinds = ("channel", "details", "find")

	API = "https://api.themoviedb.org/3"
	IMAGES = "https://image.tmdb.org/t/p/"
	TMDB_CACHE_TIME = 30 * 24 * 60 * 60

	# what the names of releases add after the title
	EXTRA = re.compile(r"\b(2160p|1080p|720p|576p|480p|4k|uhd|hdr|"
		r"web[ -]?dl|webrip|web|bluray|blu[ -]?ray|brrip|bdrip|dvdrip|"
		r"dvd|hdtv|hdrip|x264|x265|h ?264|h ?265|hevc|avc|aac\d?|ac3|"
		r"ddp?\d?|dts|truehd|atmos|ita|eng|multi|subs?|proper|repack|"
		r"extended|remastered|unrated|10bit|8bit)\b.*$", re.IGNORECASE)
	EPISODES = (
		re.compile(r"^(?P<title>.*?)[\s._-]*s(?P<season>\d{1,2})"
			r"[\s._-]*e(?P<episode>\d{1,3})", re.IGNORECASE),
		re.compile(r"^(?P<title>.*?)[\s._-]*(?P<season>\d{1,2})x"
			r"(?P<episode>\d{2,3})\b", re.IGNORECASE),
	)
	SEASON_FOLDER = re.compile(r"^(season|stagione|staffel|saison|"
		r"temporada|s)[\s._-]*(?P<season>\d{1,2})$", re.IGNORECASE)
	EPISODE_ONLY = re.compile(r"^(e|ep|episode|episodio|folge)?[\s._-]*"
		r"(?P<episode>\d{1,3})\b", re.IGNORECASE)
	YEAR = re.compile(r"^(?P<title>.*?)[\s._(\[-]+(?P<year>(19|20)\d{2})"
		r"([\s._)\]-]|$)")

	def __init__(self):
		self.local = None
		self.lock = threading.Lock()

	def cache_salt(self):
		return hashlib.sha1(self.key().encode()).hexdigest()[:8]

	# the files

	@staticmethod
	def tidy(text):
		text = re.sub(r"\[[^\]]*\]|\{[^}]*\}", " ", text)
		text = re.sub(r"[._]+", " ", text)
		text = LibraryProvider.EXTRA.sub("", text)
		return re.sub(r"\s+", " ", text).strip(" -")

	def parse(self, path):
		"""What a file is, by its name and its folders: a film with its
		title and year, or an episode with its series, season and
		number."""
		name = os.path.splitext(os.path.basename(path))[0]
		folder = os.path.basename(os.path.dirname(path))
		parent = os.path.basename(os.path.dirname(os.path.dirname(path)))
		for pattern in self.EPISODES:
			found = pattern.match(name)
			if found:
				title = self.tidy(found.group("title"))
				if not title:
					season_folder = self.SEASON_FOLDER.match(folder)
					title = self.tidy(parent if season_folder else folder)
				return {"kind": "episode", "series": title,
					"season": int(found.group("season")),
					"episode": int(found.group("episode"))}
		season_folder = self.SEASON_FOLDER.match(folder)
		episode_only = self.EPISODE_ONLY.match(name)
		if season_folder and episode_only:
			return {"kind": "episode", "series": self.tidy(parent),
				"season": int(season_folder.group("season")),
				"episode": int(episode_only.group("episode"))}
		found = self.YEAR.match(name)
		if found and self.tidy(found.group("title")):
			return {"kind": "movie", "title": self.tidy(found.group("title")),
				"year": int(found.group("year"))}
		return {"kind": "movie", "title": self.tidy(name) or name, "year": 0}

	@staticmethod
	def folders():
		"""The folders of the local tabs, from the categories file."""
		home = os.path.expanduser("~")
		path = os.path.join(home, "config", "settings", "Cinema",
			"categories")
		folders = []
		try:
			with open(path, encoding="utf-8") as file:
				for line in file:
					found = re.search(r"=\s*@local\s+channel:(.+)$",
						line.strip())
					if found and os.path.isdir(found.group(1).strip()):
						folders.append(found.group(1).strip())
		except OSError:
			pass
		# the films and series downloaded with BitTorrent, and the downloads
		# put in order
		torrents = os.path.join(home, "Cinema", "Torrent")
		if os.path.isdir(torrents) and torrents not in folders:
			folders.append(torrents)
		for folder in organized_folders():
			if folder not in folders:
				folders.append(folder)
		return folders

	def files(self):
		if self.local is None:
			self.local = PROVIDERS.get("local") or LocalProvider()
		found = []
		for folder in self.folders():
			for path in self.local.videos(folder):
				if path not in found:
					found.append(path)
		return found

	# TMDB

	@staticmethod
	def key():
		path = os.path.join(os.path.expanduser("~"), "config", "settings",
			"Cinema", "tmdb_key")
		try:
			with open(path, encoding="utf-8") as file:
				return file.read().strip()
		except OSError:
			return ""

	def tmdb(self, path, query=None):
		"""An answer of TMDB, from the cache when it is there."""
		key = self.key()
		if not key:
			return None
		query = dict(query or {}, language=LANGUAGE)
		name = path + "?" + urllib.parse.urlencode(sorted(query.items()))
		cached_path = None
		if cache:
			cached_path = os.path.join(cache, "tmdb-%s.json"
				% hashlib.sha1(name.encode()).hexdigest()[:16])
			try:
				if time.time() - os.path.getmtime(cached_path) \
						< self.TMDB_CACHE_TIME:
					with open(cached_path, encoding="utf-8") as file:
						return json.load(file)
			except (OSError, ValueError):
				pass
		headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
		# a read access token is long, an API key short
		if len(key) > 40:
			headers["Authorization"] = "Bearer " + key
		else:
			query["api_key"] = key
		request = urllib.request.Request("%s%s?%s" % (self.API, path,
			urllib.parse.urlencode(query)), headers=headers)
		try:
			with urllib.request.urlopen(request, timeout=30) as response:
				response = json.load(response)
		except Exception:
			return None
		if cached_path:
			try:
				temporary = cached_path + ".part.%d" % threading.get_ident()
				with open(temporary, "w", encoding="utf-8") as file:
					json.dump(response, file)
				os.replace(temporary, cached_path)
			except OSError:
				pass
		return response

	def check(self):
		"""Whether TMDB takes the key, asked now, not from the cache."""
		key = self.key()
		if not key:
			return False
		query = {}
		headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
		if len(key) > 40:
			headers["Authorization"] = "Bearer " + key
		else:
			query["api_key"] = key
		request = urllib.request.Request("%s/configuration?%s" % (self.API,
			urllib.parse.urlencode(query)), headers=headers)
		try:
			with urllib.request.urlopen(request, timeout=30) as response:
				return response.status == 200
		except Exception:
			return False

	def image(self, path, size="w342"):
		return self.IMAGES + size + path if path else ""

	def movie_info(self, title, year):
		query = {"query": title}
		if year:
			query["year"] = year
		found = self.tmdb("/search/movie", query)
		results = (found or {}).get("results") or []
		if not results and year:
			found = self.tmdb("/search/movie", {"query": title})
			results = (found or {}).get("results") or []
		if not results:
			return None
		return self.tmdb("/movie/%d" % results[0]["id"],
			{"append_to_response": "credits"})

	def series_info(self, name):
		found = self.tmdb("/search/tv", {"query": name})
		results = (found or {}).get("results") or []
		if not results:
			return None
		return self.tmdb("/tv/%d" % results[0]["id"],
			{"append_to_response": "credits"})

	def season_info(self, series_id, season):
		return self.tmdb("/tv/%d/season/%d" % (series_id, season)) or {}

	@staticmethod
	def people(info):
		cast = ((info or {}).get("credits") or {}).get("cast") or []
		return ", ".join(person.get("name", "") for person in cast[:5])

	@staticmethod
	def genres(info):
		return ", ".join(genre.get("name", "")
			for genre in (info or {}).get("genres") or [])

	# the items

	def picture(self, path):
		"""A frame of a file, when TMDB has no picture for it."""
		item = self.local.item(path, True) if self.local else None
		return item or {}

	def movie(self, path, details=False):
		parsed = self.parse(path)
		info = self.movie_info(parsed["title"], parsed.get("year"))
		frame = {} if info and info.get("poster_path") else self.picture(path)
		year = as_int(str((info or {}).get("release_date") or "")[:4],
			parsed.get("year") or 0)
		description = (info or {}).get("overview") or ""
		if details and info:
			lines = [description, "", self.genres(info), self.people(info)]
			description = "\n".join(line for line in lines if line is not None)
		picture = self.image((info or {}).get("poster_path")) \
			or frame.get("thumbnail", "")
		return {
			"provider": self.id, "kind": "movie", "id": path,
			"title": clean((info or {}).get("title") or parsed["title"]),
			"description": multiline(description) if details
				else clean(description),
			"channel": clean(self.genres(info)) if info else "",
			"channel_id": "",
			"duration": as_int((info or {}).get("runtime"), 0) * 60
				or as_int(frame.get("duration"), 0),
			"year": year, "date": "", "views": 0,
			"rating": float((info or {}).get("vote_average") or 0),
			"poster": picture, "thumbnail": picture,
			"season": 0, "episode": 0,
		}

	def episodes(self):
		"""The episodes, by series: {key: (name, [(season, episode, path)])}."""
		series = {}
		for path in self.files():
			parsed = self.parse(path)
			if parsed["kind"] != "episode" or not parsed["series"]:
				continue
			key = re.sub(r"[^a-z0-9]+", "-", parsed["series"].lower()).strip("-")
			entry = series.setdefault(key, (parsed["series"], []))
			entry[1].append((parsed["season"], parsed["episode"], path))
		for name, files in series.values():
			files.sort()
		return series

	def series_item(self, key, name, files, details=False):
		info = self.series_info(name)
		picture = self.image((info or {}).get("poster_path"))
		if not picture and files:
			picture = self.picture(files[0][2]).get("thumbnail", "")
		description = (info or {}).get("overview") or ""
		if details and info:
			description = "\n".join([description, "", self.genres(info),
				self.people(info)])
		seasons = len(set(season for season, _, _ in files))
		return {
			"provider": self.id, "kind": "series", "id": "series:" + key,
			"title": clean((info or {}).get("name") or name),
			"description": multiline(description) if details
				else clean(description),
			"channel": "%d × %d" % (seasons, len(files)),
			"channel_id": "",
			"duration": 0,
			"year": as_int(str((info or {}).get("first_air_date") or "")[:4],
				0),
			"date": "", "views": 0,
			"rating": float((info or {}).get("vote_average") or 0),
			"poster": picture, "thumbnail": picture,
			"season": seasons, "episode": len(files),
		}

	def episode_items(self, key, name, files):
		info = self.series_info(name)
		seasons = {}
		items = []
		for season, episode, path in files:
			found = {}
			if info:
				if season not in seasons:
					seasons[season] = {e.get("episode_number"): e for e in
						self.season_info(info["id"], season).get("episodes")
						or []}
				found = seasons[season].get(episode) or {}
			frame = {} if found.get("still_path") else self.picture(path)
			picture = self.image(found.get("still_path"), "w300") \
				or frame.get("thumbnail", "")
			title = "%dx%02d" % (season, episode)
			if found.get("name"):
				title += " · " + found["name"]
			items.append({
				"provider": self.id, "kind": "episode", "id": path,
				"title": clean(title),
				"description": multiline(found.get("overview") or ""),
				"channel": clean((info or {}).get("name") or name),
				"channel_id": "series:" + key,
				"duration": as_int(found.get("runtime"), 0) * 60
					or as_int(frame.get("duration"), 0),
				"year": as_int(str(found.get("air_date") or "")[:4], 0),
				"date": str(found.get("air_date") or ""), "views": 0,
				"rating": float(found.get("vote_average") or 0),
				"poster": picture, "thumbnail": picture,
				"season": season, "episode": episode,
			})
		return items

	def channel(self, channel_id, count):
		if channel_id == "movies":
			paths = [path for path in self.files()
				if self.parse(path)["kind"] == "movie"]
			# a few at a time: TMDB answers slowly, the AC100 waits
			from concurrent.futures import ThreadPoolExecutor
			with ThreadPoolExecutor(4) as pool:
				items = list(pool.map(self.movie, paths))
			return sorted(items, key=lambda item: item["title"].lower())
		series = self.episodes()
		if channel_id == "series":
			from concurrent.futures import ThreadPoolExecutor
			with ThreadPoolExecutor(4) as pool:
				items = list(pool.map(lambda entry: self.series_item(
					entry[0], entry[1][0], entry[1][1]), series.items()))
			return sorted(items, key=lambda item: item["title"].lower())
		if channel_id.startswith("series:"):
			key = channel_id[len("series:"):]
			if key in series:
				return self.episode_items(key, *series[key])
		return []

	def details(self, media_id, kind="movie"):
		if media_id.startswith("series:"):
			key = media_id[len("series:"):]
			series = self.episodes()
			if key not in series:
				return None
			return self.series_item(key, *series[key], details=True)
		if not os.path.isfile(media_id):
			return None
		if self.parse(media_id)["kind"] == "episode":
			parsed = self.parse(media_id)
			key = re.sub(r"[^a-z0-9]+", "-",
				parsed["series"].lower()).strip("-")
			series = self.episodes()
			for item in self.episode_items(key, *series.get(key,
					(parsed["series"], []))):
				if item["id"] == media_id:
					return item
		return self.movie(media_id, details=True)

	def find(self, media_id, kind="movie"):
		return media_id if os.path.isfile(media_id) else None


register(LibraryProvider)


def job_search(provider, tag, count, query, order=""):
	def search():
		wait_for_play()
		# the newest first, where the source can give them so
		if order == "newest":
			try:
				return provider.search(query, count, order)
			except TypeError:
				pass
		return provider.search(query, count)

	try:
		name = "%s search %d %s" % (provider.id, count, query)
		if order:
			name += " " + order
		# the latest: looked for again after ten minutes, and never shown
		# more than an hour old
		if order == "newest":
			items = cached(name, search, 10 * 60, 60 * 60)
		else:
			items = cached(name, search)
		answer({"type": "results", "tag": tag, "items": items})
	except Exception:
		answer({"type": "nothing", "tag": tag})


def job_channel(provider, tag, count, channel_id):
	try:
		name = "%s channel %d %s" % (provider.id, count, channel_id)
		# what the list depends on besides (the key of TMDB for the library):
		# another key, another list
		salt = getattr(provider, "cache_salt", None)
		if salt is not None:
			name += " " + salt()
		if channel_id.startswith(":news:"):
			# the news: looked for again after ten minutes, never shown
			# more than an hour old
			items = cached(name, lambda: (wait_for_play(),
				provider.channel(channel_id, count))[1], 10 * 60, 60 * 60)
		elif getattr(provider, "cache_channels", True):
			items = cached(name, lambda: (wait_for_play(),
				provider.channel(channel_id, count))[1])
		else:
			wait_for_play()
			items = provider.channel(channel_id, count)
		message = {"type": "results", "tag": tag, "items": items}
		about = kept_about(provider.id, channel_id)
		if about:
			message["about"] = about
		answer(message)
	except Exception:
		answer({"type": "nothing", "tag": tag})


def job_details(provider, media_id, kind):
	with playing():
		job_details_now(provider, media_id, kind)


def job_details_now(provider, media_id, kind):
	try:
		name = "%s details %s %s" % (provider.id, kind, media_id)
		item = cached_details(name,
			lambda: provider.details(media_id, kind))
	except Exception:
		item = None
	answer({"type": "details", "id": media_id, "item": item})


def job_find(provider, media_id, kind):
	with playing():
		job_find_now(provider, media_id, kind)


# what Cinema should know of a video found: "javascript", it could not be
# had above 360p without a runtime of JavaScript
NOTES = {}


def job_find_now(provider, media_id, kind):
	try:
		url = provider.find(media_id, kind)
	except Exception:
		url = None
	note = NOTES.pop(media_id, "")
	if url:
		answer({"type": "found", "id": media_id, "url": url, "note": note})
	else:
		answer({"type": "lost", "id": media_id})


def job_subtitles(provider, media_id, kind):
	try:
		tracks = provider.subtitles(media_id, kind)
	except Exception:
		tracks = []
	answer({"type": "subtitles", "id": media_id, "tracks": tracks})


def job_subtitle(provider, media_id, kind, language, automatic):
	try:
		text = provider.subtitle(media_id, kind, language, automatic)
	except Exception:
		text = None
	# a failed fetch is said so, rather than an empty track Cinema would
	# keep for the whole video
	answer({"type": "subtitle", "id": media_id, "language": language,
		"automatic": automatic, "text": text or "", "failed": not text})


def job_download(provider, media_id, folder):
	last = [-1, 0.0]

	def progress(percent, **details):
		if media_id in CANCELLED:
			raise Cancelled()
		if media_id in PAUSED:
			raise Paused()
		# twice a second at most, or when the percent changes
		now = time.time()
		if percent == last[0] and now - last[1] < 0.5 \
				and not details.get("map"):
			return
		last[0] = percent
		last[1] = now
		message = {"type": "progress", "id": media_id, "percent": percent}
		message.update(details)
		answer(message)

	CANCELLED.discard(media_id)
	PAUSED.discard(media_id)
	try:
		path = provider.download(media_id, folder, progress)
	except Exception:
		path = None
	if path and media_id not in CANCELLED:
		# done, even if a pause came at its very end
		PAUSED.discard(media_id)
	if media_id in CANCELLED:
		CANCELLED.discard(media_id)
		PAUSED.discard(media_id)
		answer({"type": "cancelled", "id": media_id})
	elif media_id in PAUSED:
		PAUSED.discard(media_id)
		answer({"type": "paused", "id": media_id})
	elif path:
		answer({"type": "saved", "id": media_id, "path": path})
	else:
		answer({"type": "failed", "id": media_id})


def pause(media_id):
	"""A download stopped, what it got kept: "paused" once it is."""
	PAUSED.add(media_id)
	with CHILDREN_LOCK:
		process = RUNNING.get(media_id)
	if process is not None:
		try:
			process.terminate()
		except OSError:
			pass


def discard(request):
	"""What a download paused or waiting had got, thrown away."""
	media_id = request.get("id", "")
	folder = request.get("folder", "")
	provider = PROVIDERS.get(request.get("provider") or "")
	if provider is not None and hasattr(provider, "discard") and folder:
		ignore_errors(lambda: provider.discard(media_id, folder))
	# what yt-dlp and the others leave: "<name> [<id>].<ext>.part"
	if folder and media_id:
		marks = ["[%s]" % short_id(media_id)]
		if "/" not in media_id:
			marks.append("[%s]" % media_id)
		try:
			for name in os.listdir(folder):
				if ".part" in name and any(mark in name for mark in marks):
					os.remove(os.path.join(folder, name))
		except OSError:
			pass


def cancel(media_id):
	# a copy to the network folder
	if media_id in UPLOADS:
		UPLOADS[media_id] = True
		return
	CANCELLED.add(media_id)
	# a torrent may wait for its peers a long time between two lines
	with CHILDREN_LOCK:
		process = RUNNING.get(media_id)
	if process is not None:
		try:
			process.terminate()
		except OSError:
			pass


UPLOADS = {}		# id: True once cancelled


def ftp_open(target, user, password):
	"""An FTP connection in the folder of target, ftp://host[:port]/dir,
	the folder made if it is not there."""
	import ftplib
	parts = urllib.parse.urlparse(target)
	if parts.scheme != "ftp" or not parts.hostname:
		raise ValueError("not an ftp:// address")
	ftp = ftplib.FTP()
	ftp.encoding = "utf-8"
	ftp.connect(parts.hostname, parts.port or 21, timeout=20)
	ftp.login(user or "anonymous", password or "")
	for folder in [p for p in urllib.parse.unquote(parts.path).split("/") if p]:
		try:
			ftp.cwd(folder)
		except ftplib.error_perm:
			ftp.mkd(folder)
			ftp.cwd(folder)
	return ftp


def job_upload_check(target, user, password):
	"""Whether the network folder can be written to: a small file put
	there and taken away."""
	import io
	try:
		ftp = ftp_open(target, user, password)
		name = ".cinema-test-%d" % os.getpid()
		ftp.storbinary("STOR " + name, io.BytesIO(b"Cinema"))
		ftp.delete(name)
		ftp.quit()
		answer({"type": "upload checked", "ok": True, "error": ""})
	except Exception as error:
		answer({"type": "upload checked", "ok": False,
			"error": str(error)[:200]})


def job_upload(media_id, path, target, user, password):
	"""A downloaded video copied into the network folder, its progress
	told as it goes; "uploaded" says whether the copy there is whole (its
	size checked)."""
	name = os.path.basename(path)
	error = ""
	try:
		total = os.path.getsize(path)
		ftp = ftp_open(target, user, password)
		sent = [0, 0.0, -1]

		def block(data):
			if UPLOADS.get(media_id):
				raise Cancelled()
			sent[0] += len(data)
			now = time.time()
			percent = int(sent[0] * 100 / total) if total else 100
			if percent != sent[2] and now - sent[1] >= 0.5:
				sent[1] = now
				sent[2] = percent
				answer({"type": "uploading", "id": media_id,
					"percent": percent})

		with open(path, "rb") as file:
			ftp.storbinary("STOR " + name, file, 256 * 1024, block)
		try:
			ftp.voidcmd("TYPE I")
			there = ftp.size(name)
		except Exception:
			there = None
		ftp.quit()
		ok = there == total
		if not ok:
			error = "size %s of %d" % (there, total)
	except Cancelled:
		ok = False
		error = "cancelled"
	except Exception as failure:
		ok = False
		error = str(failure)[:200]
	UPLOADS.pop(media_id, None)
	answer({"type": "uploaded", "id": media_id, "ok": ok, "error": error})


ORGANIZE_FILE = "organize.json"


def organized_folders():
	"""The folders of films and series that downloads were put in order
	into (organize), for the library to read: from the cache."""
	if not cache:
		return []
	try:
		with open(os.path.join(cache, ORGANIZE_FILE), encoding="utf-8") as file:
			known = json.load(file)
	except (OSError, ValueError):
		return []
	return [path for path in (known.get("movies"), known.get("series"))
		if isinstance(path, str) and os.path.isdir(path)]


def organize(request):
	"""A download put where it belongs, under root (~/Cinema): a film in
	<movies>/Title (Year), an episode in <series>/Series/<season N>/Series
	SxxEyy, a video in <videos>/Channel/Title; named as TMDB names it when
	there is a key. Its new path, and a picture for it ("" if it has one):
	the poster from TMDB, or a frame of it."""
	path = request.get("path", "")
	root = request.get("root") or os.path.dirname(path)
	names = request.get("names") or {}
	movies = os.path.join(root, file_name(names.get("movies") or "Films"))
	series_root = os.path.join(root, file_name(names.get("series") or "Series"))
	videos = os.path.join(root, file_name(names.get("videos") or "Videos"))
	season_name = names.get("season") or "Season %d"
	if "%d" not in season_name:
		season_name = "Season %d"
	if cache:
		try:
			with open(os.path.join(cache, ORGANIZE_FILE), "w",
					encoding="utf-8") as file:
				json.dump({"movies": movies, "series": series_root}, file)
		except OSError:
			pass

	provider = request.get("provider") or ""
	kind = request.get("kind") or "video"
	title = request.get("title") or ""
	channel = request.get("channel") or ""
	extension = os.path.splitext(path)[1]
	library = PROVIDERS.get("library") or LibraryProvider()
	if library.local is None:
		library.local = PROVIDERS.get("local") or LocalProvider()
	poster = ""

	podcasts = os.path.join(root, file_name(names.get("podcasts")
		or "Podcasts"))
	stem = os.path.splitext(os.path.basename(path))[0]
	# what a telephone or a camera names what it records: not a film
	camera = re.match(r"^(IMG|VID|PXL|MVI|DSC|MOV|DJI|GOPR|GH\d)?[_-]?"
		r"(19|20)\d{6}[_-]?\d{6}", stem, re.IGNORECASE) is not None
	if provider == "podcast":
		# by the podcast, its picture kept already
		section = "podcasts"
		folder = os.path.join(podcasts, file_name(clean(channel) or "Podcast"))
		target = os.path.join(folder, file_name(clean(title) or stem)
			+ extension)
		need_picture = False
	elif provider in ("youtube", "peertube") or camera or (kind == "video"
			and provider not in ("torrent", "dlna", "local", "archive")):
		section = "videos"
		# a video of a channel, or a clip: by the channel; the videos of a
		# channel have their picture already, a clip gets a frame of it
		folder = os.path.join(videos, file_name(clean(channel) or provider
			or "Cinema"))
		target = os.path.join(folder, file_name(clean(title) or stem)
			+ extension)
		need_picture = provider not in ("youtube", "peertube")
	else:
		# a film or an episode: by its name; a title given (archive, a
		# server of the home network) says more than the name of a file
		name = os.path.splitext(os.path.basename(path))[0]
		if title and provider in ("archive", "dlna") \
				and not re.match(r"^\d{8}_\d{6}", title):
			name = title
		parsed = library.parse(os.path.join(os.path.dirname(path),
			name + extension))
		if parsed["kind"] == "episode":
			section = "series"
			info = library.series_info(parsed["series"]) \
				if library.key() else None
			series = clean((info or {}).get("name") or parsed["series"]) \
				or "Series"
			poster = (info or {}).get("poster_path") or ""
			folder = os.path.join(series_root, file_name(series),
				file_name(season_name % parsed["season"]))
			target = os.path.join(folder, "%s S%02dE%02d%s" % (
				file_name(series), parsed["season"], parsed["episode"],
				extension))
		else:
			section = "movies"
			info = library.movie_info(parsed["title"], parsed.get("year")) \
				if library.key() else None
			found = clean((info or {}).get("title") or parsed["title"]) \
				or name
			year = as_int(str((info or {}).get("release_date") or "")[:4],
				parsed.get("year") or 0)
			poster = (info or {}).get("poster_path") or ""
			target = os.path.join(movies, file_name(
				"%s (%d)" % (found, year) if year else found) + extension)
		need_picture = True

	# not over another file
	if os.path.abspath(target) != os.path.abspath(path):
		stem, ending = os.path.splitext(target)
		number = 2
		while os.path.exists(target):
			target = "%s (%d)%s" % (stem, number, ending)
			number += 1
		os.makedirs(os.path.dirname(target), exist_ok=True)
		try:
			os.rename(path, target)
		except OSError:
			shutil.move(path, target)
		# the folders it leaves empty go, up to root
		folder = os.path.dirname(path)
		top = os.path.abspath(root)
		while os.path.abspath(folder).startswith(top + os.sep):
			try:
				if os.listdir(folder):
					break
				os.rmdir(folder)
			except OSError:
				break
			folder = os.path.dirname(folder)

	picture = ""
	if need_picture and cache:
		if poster:
			picture = os.path.join(cache, "organize-%s.jpg"
				% hashlib.sha1(poster.encode()).hexdigest()[:16])
			if not os.path.exists(picture):
				try:
					request_ = urllib.request.Request(library.image(poster),
						headers={"User-Agent": USER_AGENT})
					with urllib.request.urlopen(request_, timeout=30) as response:
						data = response.read(5 * 1024 * 1024)
					with open(picture, "wb") as file:
						file.write(data)
				except Exception:
					picture = ""
		if not picture:
			# a frame of it
			found = library.local.item(target, True) or {}
			picture = (found.get("thumbnail") or "").replace("file://", "")
	return target, picture, section


def job_organize(request):
	media_id = request.get("id", "")
	path = request.get("path", "")
	try:
		target, picture, section = organize(request)
	except Exception:
		target, picture, section = path, "", ""
	# a torrent played as it came goes on from where it is now
	with TORRENTS_LOCK:
		if media_id in TORRENTS and target != path:
			TORRENTS[media_id]["final"] = target
	answer({"type": "organized", "id": media_id, "from": path,
		"path": target, "picture": picture, "section": section})


def job_youtube_check():
	"""Whether the account of YouTube is there: the subscriptions read
	with the cookies of WebPositive."""
	ACCOUNT["enabled"] = True
	youtube = PROVIDERS.get("youtube")
	error = ""
	ok = False
	try:
		if youtube is None:
			error = "yt-dlp is missing"
		elif not os.path.exists(WEBPOSITIVE_COOKIES):
			error = "no cookies of WebPositive"
		else:
			forget_youtube_cookies()
			cookies = youtube_cookies()
			youtube.videos_of(YOUTUBE_FEEDS[":subscriptions"], 3,
				cookies=cookies)
			ok = True
	except Exception as failure:
		error = str(failure).splitlines()[0][:200] if str(failure) else \
			type(failure).__name__
	if not ok:
		ACCOUNT["enabled"] = False
		forget_youtube_cookies()
	answer({"type": "youtube checked", "ok": ok, "error": error})


def job_check():
	library = PROVIDERS.get("library")
	ok = bool(library and library.check())
	answer({"type": "checked", "ok": ok})


def work(request):
	if "height" in request:
		HEIGHT[0] = max(0, as_int(request.get("height"), 0))
	if "limit" in request:
		LIMIT[0] = max(0, as_int(request.get("limit"), 0))
	if request.get("type") == "cancel":
		cancel(request.get("id", ""))
		return
	if request.get("type") == "pause":
		pause(request.get("id", ""))
		return
	if request.get("type") == "discard":
		discard(request)
		return
	if request.get("type") == "check":
		threading.Thread(target=job_check, daemon=True).start()
		return
	if request.get("type") == "youtube account":
		ACCOUNT["enabled"] = bool(request.get("enabled"))
		if not ACCOUNT["enabled"]:
			forget_youtube_cookies()
		return
	if request.get("type") == "youtube check":
		threading.Thread(target=job_youtube_check, daemon=True).start()
		return
	if request.get("type") == "organize":
		threading.Thread(target=job_organize, args=(request,),
			daemon=True).start()
		return
	if request.get("type") in ("upload", "upload check"):
		arguments = (request.get("target", ""), request.get("user", ""),
			request.get("password", ""))
		if request.get("type") == "upload":
			UPLOADS[request.get("id", "")] = False
			threading.Thread(target=job_upload, args=(request.get("id", ""),
				request.get("path", "")) + arguments, daemon=True).start()
		else:
			threading.Thread(target=job_upload_check, args=arguments,
				daemon=True).start()
		return
	provider = PROVIDERS.get(request.get("provider") or DEFAULT_PROVIDER)
	if provider is None and request.get("type") == "download":
		# a torrent without aria2c, say: not something for another source
		answer({"type": "failed", "id": request.get("id", "")})
		return
	# a source named that is not there (the home network before its servers
	# answered, torrents without aria2c): not asked of YouTube instead,
	# which would run yt-dlp on an address it cannot know
	if provider is None and not request.get("provider"):
		provider = PROVIDERS.get(DEFAULT_PROVIDER)
	if provider is None:
		# no source at all: an answer still, or Cinema would wait for it
		kind = request.get("type")
		media_id = request.get("id", "")
		tag = as_int(request.get("tag"), -1)
		if kind in ("search", "channel"):
			answer({"type": "nothing", "tag": tag})
		elif kind == "details":
			answer({"type": "details", "id": media_id, "item": None})
		elif kind == "find":
			answer({"type": "lost", "id": media_id})
		elif kind == "subtitles":
			answer({"type": "subtitles", "id": media_id, "tracks": []})
		elif kind == "subtitle":
			answer({"type": "subtitle", "id": media_id,
				"language": request.get("language", ""),
				"automatic": bool(request.get("automatic")), "text": "",
				"failed": True})
		return

	kind = request.get("type")
	tag = as_int(request.get("tag"), -1)
	count = as_int(request.get("count"), 24)

	if kind == "search":
		thread = (job_search,
			(provider, tag, count, request.get("query", ""),
				request.get("order", "")))
	elif kind == "channel":
		thread = (job_channel,
			(provider, tag, count, request.get("channel", "")))
	elif kind == "details":
		thread = (job_details, (provider, request.get("id", ""),
			request.get("kind", "video")))
	elif kind == "find":
		thread = (job_find, (provider, request.get("id", ""),
			request.get("kind", "video")))
	elif kind == "subtitles":
		thread = (job_subtitles, (provider, request.get("id", ""),
			request.get("kind", "video")))
	elif kind == "subtitle":
		thread = (job_subtitle, (provider, request.get("id", ""),
			request.get("kind", "video"), request.get("language", ""),
			bool(request.get("automatic"))))
	elif kind == "download":
		thread = (job_download, (provider, request.get("id", ""),
			request.get("folder", "")))
	else:
		return
	threading.Thread(target=lambda: ignore_errors(lambda: thread[0](*thread[1])),
		daemon=True).start()


def watch_parent():
	"""Goes away with Cinema, even when it crashed and the pipe stays open
	somewhere: a helper alone only holds memory."""
	parent = os.getppid()
	while True:
		time.sleep(5)
		if os.getppid() != parent:
			stop_children()
			os._exit(0)


def prune_cache():
	"""What the cache keeps for nothing: the small pictures and posters of
	videos not seen for two months (got again if they are), lists and pages
	older than a month (stale after a week anyway), and the addresses of
	videos that expired. The pictures of the library and what TMDB said
	stay: they are slow to make again, or needed without the network."""
	time.sleep(120)
	wait_for_play()
	if not cache:
		return
	now = time.time()
	removed = 0
	for name in os.listdir(cache):
		path = os.path.join(cache, name)
		if name.startswith(("local-", "tmdb-")) or not os.path.isfile(path):
			continue
		if name.endswith(".jpg"):
			most = 60 * 24 * 60 * 60
		elif name.startswith(("videos-", "details-", "about-")) \
				and name.endswith(".json"):
			most = 30 * 24 * 60 * 60
		elif name.startswith("stream-") and name.endswith(".json"):
			# they last six hours or so
			most = 24 * 60 * 60
		elif ".part" in name:
			# what a write that was cut short left
			most = 24 * 60 * 60
		else:
			continue
		try:
			if now - os.path.getmtime(path) > most:
				os.remove(path)
				removed += 1
		except OSError:
			pass
		if removed % 50 == 49:
			# a pause now and then: the AC100 has other things to do
			time.sleep(0.2)


def ignore_errors(function):
	try:
		function()
	except Exception:
		pass


def serve():
	# the names of files that are not UTF-8 go to Cinema as they are
	sys.stdout.reconfigure(errors="surrogateescape")
	threading.Thread(target=watch_parent, daemon=True).start()
	answer({"type": "providers",
		"providers": [provider.describe() for provider in PROVIDERS.values()]})
	answer({"type": "ready"})
	youtube = PROVIDERS.get("youtube")
	if youtube is not None:
		threading.Thread(target=lambda: ignore_errors(youtube.warm_up),
			daemon=True).start()
	threading.Thread(target=lambda: ignore_errors(prune_cache),
		daemon=True).start()
	threading.Thread(target=lambda: ignore_errors(look_for_servers),
		daemon=True).start()
	for raw in sys.stdin.buffer:
		line = raw.decode("utf-8", "surrogateescape").strip()
		if not line:
			continue
		try:
			request = json.loads(line)
		except ValueError:
			continue
		if isinstance(request, dict):
			try:
				work(request)
			except Exception:
				pass
	# Cinema is gone: so are its downloads
	stop_children()


if __name__ == "__main__":
	try:
		serve()
	finally:
		stop_children()
