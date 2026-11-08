# Cinema

Cinema is a video player for [Haiku](https://www.haiku-os.org) and
[RenkuOS](https://github.com/RenkuOS). It finds and plays YouTube videos (with
[yt-dlp](https://github.com/yt-dlp/yt-dlp)), the public domain films and
cartoons of the [Internet Archive](https://archive.org), the free video sites
of [PeerTube](https://joinpeertube.org), live TV channels, torrents and the
videos in your own folders, all in a player of its own. It runs on x86_64, on
32-bit x86, and on 32-bit ARM with RenkuOS, where it was tuned on a Toshiba
AC100.

![The videos of a category](screenshots/main.png)

## What it does

Cinema opens on a home screen of shelves: what you were watching, what you
downloaded, and a row for each tab at the top. A tab is either a search kept
for later, or a channel whose latest videos it shows. The channel's name is
under each of its videos, with how many views it has and how long ago it came
out: a click on the name shows its latest videos. To follow a channel, press
**Follow** next to its name on the page of one of its videos, the **+** button
next to its tab, or right click the name: it becomes a tab, and its latest
videos a shelf of the home. A right click on a tab removes it, and so does
**Following** on the page. Each video has a page of its own, with its details
and views and the buttons to play or download it.

The menu next to the search field chooses where to search, YouTube, the
Internet Archive or PeerTube, and `@peertube words` in the field does it for a
single search. The tabs are kept in `~/config/settings/Cinema/categories`, one
per line, as `Name = what to search on YouTube` or `Name = channel:<id>`.
`@source` in front takes another source, as in
`Classic films = @archive channel:feature_films` (a collection of the Internet
Archive), `Blender = @peertube channel:blender_open_movies@video.blender.org`
or `Home videos = @local channel:/boot/home/Videos`.

Videos play in Cinema, from where they were left; a check marks what was
watched and a red bar what was started. Space pauses, the left and right arrows
go back and on by 10 seconds, up and down (or the mouse wheel) change the
volume, shown upright at the right, **F** goes to full screen, **Esc** closes
the video and **M** plays it in MediaPlayer instead. Subtitles come from
YouTube, from a subtitle file next to the video, or from the text tracks inside
it: the **CC** button chooses one and **V** turns them on and off. The star
next to it, or **S**, keeps the video among the favorites. The video
quality is in the settings: Automatic means 360p on 32-bit ARM and 720p
elsewhere, and you can ask for anything from 360p to 1080p. Above 360p YouTube
sends the picture and the sound apart, and `ffmpeg` puts them back together for
the player, so those videos take longer to start (about a minute and a half on
the AC100).

The first time, Cinema offers a **TV** tab with the free channels of your
country, from the public lists of [iptv-org](https://github.com/iptv-org/iptv).
The settings add the channels of other countries or any M3U list, and an `.m3u`
file dropped on the window works too. Channels for adults are left out. Live TV
plays with FFmpeg, which decodes less on slow machines to keep up, and uses the
hardware video overlay where there is one.

Drop a folder on the window and its videos become a tab, newest first, with
their length and a picture when `ffprobe` and `ffmpeg` are installed; drop a
single video to play it. The films and series in those folders also fill the
**Movies** and **TV series** tabs. Cinema reads their names from the files
(`Title (1968)`, `Title.1968.720p`, `Show.S01E03`, `Show - 1x03`, or a
`Season 1` folder). With a free [TMDB](https://www.themoviedb.org) key in the
settings (an API key or a read access token) it adds posters, plots, casts and
the names of the episodes, and keeps them for a month so the library opens
without the network. A series opens on the next episode to watch, and an
episode watched to the end goes on to the next one.

Any video can be downloaded to watch offline with **D** or a right click. The
files go to `~/Cinema`, with their title, channel and length in attributes, and
show in the **Downloaded** tab, which works without the network; a downloaded
video plays from its file wherever it appears. Paste a magnet link in the
search field, or drop a `.torrent` file on the window, and the biggest video it
shares is downloaded with `aria2c` into the same tab, where you can follow and
stop it; a stopped or interrupted download picks up where it was. Use it for
what you may share, such as the films of the Blender Foundation or the torrents
of the Internet Archive. **S** or a right click gives a video or a channel a
star, and the stars have a **Favorites** tab of their own.

The settings, in the **≡** menu or with Alt+,, hold the TMDB key, the folders
of the library, the TV lists, the video quality, whether videos play in Cinema
or in MediaPlayer and from where they were left, and where a search goes first.
**Even volume**, on unless you turn it off, brings films and TV, which are
mastered well below music to leave room for their peaks, up to the level of
music, and keeps an explosion from drowning the dialogue after it; it costs
about half a percent of one core of the AC100.

Everything works from the keyboard: the arrows choose, **Enter** or **Space**
plays, **D** downloads, **S** gives a star, **F** toggles full screen and
**Esc** goes back. **Tab** or **F6** moves to the tabs at the top, where the
arrows choose one and **Enter** opens it. A gamepad works too: the D-pad or the
left stick chooses, **A** plays, **B** goes back, **X** downloads, **Y** toggles
full screen, and **L1**, **Select** or **Start** goes to the tabs.

Cinema speaks English, Italian, German, French, Spanish, Portuguese,
Ukrainian, Korean, Japanese and Simplified Chinese; the last three need a CJK
font such as `noto_sans_cjk_jp`.

![The latest videos of a channel](screenshots/channel.png)

## Installing

The [releases](https://github.com/atomozero/Cinema/releases) have packages for
x86_64, for 32-bit x86 (`x86_gcc2`, built for the gcc 13 side of the hybrid)
and for 32-bit ARM (ARMv7 with VFPv3-D16, such as the AC100):

```sh
pkgman install cinema-1.4.1-1-x86_64.hpkg
```

On x86_64 and 32-bit x86, pkgman brings Python, the yt-dlp module and FFmpeg
6 from HaikuPorts too. HaikuPorts has no yt-dlp for ARM, so there you put the
single-file release of yt-dlp in the `PATH`:

```sh
mkdir -p ~/config/non-packaged/bin
curl -L -o ~/config/non-packaged/bin/yt-dlp \
	https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp
chmod +x ~/config/non-packaged/bin/yt-dlp
```

Cinema keeps an unpacked, compiled copy of that file in its cache, so that
yt-dlp loads in about three seconds instead of nine on the AC100. YouTube changes
often, and when videos stop being found a newer yt-dlp usually helps:
`pkgman update`, or the same `curl` again.

## What it needs

Cinema needs `python3` and yt-dlp, either the Python module
(`yt_dlp_python3.14`, matching your Python) or the single-file `yt-dlp` in the
`PATH`, and `curl` for the pictures. yt-dlp is loaded once, by
`cinema-helper.py`, a small helper that Cinema starts and talks to.

Live TV links the FFmpeg libraries: `ffmpeg6` on x86_64, `ffmpeg` on ARM. The
`ffmpeg` and `ffprobe` commands are used for YouTube above 360p and for the
length and the picture of your own videos, and `aria2c` for torrents.

## Building

```sh
make
```

It links the FFmpeg libraries, so on x86_64 install `ffmpeg6_devel` first.
`packaging/build-hpkg.sh`, run on Haiku or RenkuOS, builds the package for the
machine's architecture. Without a package, put `objects/Cinema` and
`helper/cinema-helper.py` together in a folder such as
`~/config/non-packaged/apps/Cinema/`, and the catalogs from
`objects/catalogs/` in
`~/config/non-packaged/data/locale/catalogs/x-vnd.atomozero-Cinema/`.

If Cinema ever stops answering, `~/config/cache/Cinema/freeze-reports.txt`
says what each of its threads was doing then: worth attaching to a report.

To translate, `make catkeys` writes `locales/en.catkeys`, and
`tools/update-catkeys.py <language>` brings a translation up to date with it.

## Credits

This product uses the TMDB API but is not endorsed or certified by TMDB.

Live TV is played after the FFmpeg player of
[R Television](https://github.com/rainygirl/rtelevision), by Lee JunHaeng (MIT
license), and its channels come from the lists of
[iptv-org](https://github.com/iptv-org/iptv).

## License

MIT, see [LICENSE](LICENSE).
