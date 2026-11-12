/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "PlayerView.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Bitmap.h>
#include <Catalog.h>
#include <Menu.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <Window.h>

#include "CinemaWindow.h"
#include "Messages.h"
#include "Player.h"
#include "Style.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Player"


using namespace Style;


static const bigtime_t kControlsTime = 3000000;
static const bigtime_t kStep = 10000000;
static const bigtime_t kBigStep = 60000000;
static const float kBaseBarHeight = 56;
static const float kBaseTitleHeight = 44;
static const float kBaseSubtitleSize = 28;


static bool
overlay_filters()
{
	const char* value = getenv("CINEMA_OVERLAY_FILTER");
	return value == NULL || (value[0] != '\0' && strcmp(value, "0") != 0);
}


static void
wrap_text(const BFont& font, float maxWidth, const BString& text,
	std::vector<BString>& lines)
{
	int32 start = 0;
	while (start <= text.Length()) {
		int32 end = text.FindFirst('\n', start);
		BString paragraph;
		if (end < 0)
			paragraph = text.String() + start;
		else
			paragraph = BString(text.String() + start, end - start);
		paragraph.Trim();

		if (!paragraph.IsEmpty()) {
			BString line;
			int32 position = 0;
			while (position < paragraph.Length()) {
				while (position < paragraph.Length()
					&& paragraph[position] == ' ')
					position++;
				int32 wordEnd = position;
				while (wordEnd < paragraph.Length()
					&& paragraph[wordEnd] != ' ')
					wordEnd++;
				if (wordEnd == position)
					break;
				BString word(paragraph.String() + position,
					wordEnd - position);
				position = wordEnd;

				BString candidate(line);
				if (!candidate.IsEmpty())
					candidate << ' ';
				candidate << word;
				if (line.IsEmpty()
					|| font.StringWidth(candidate.String()) <= maxWidth)
					line = candidate;
				else {
					lines.push_back(line);
					line = word;
				}
			}
			while (font.StringWidth(line.String()) > maxWidth
				&& line.Length() > 1) {
				int32 fit = line.Length();
				while (fit > 1 && font.StringWidth(
					BString(line.String(), fit).String()) > maxWidth)
					fit--;
				lines.push_back(BString(line.String(), fit));
				line = line.String() + fit;
				line.Trim();
			}
			if (!line.IsEmpty())
				lines.push_back(line);
		}

		if (end < 0)
			break;
		start = end + 1;
	}
}


static BString
time_text(bigtime_t time)
{
	int32 seconds = (int32)(std::max((bigtime_t)0, time) / 1000000);
	BString text;
	if (seconds >= 3600) {
		text.SetToFormat("%" B_PRId32 ":%02" B_PRId32 ":%02" B_PRId32,
			seconds / 3600, seconds / 60 % 60, seconds % 60);
	} else {
		text.SetToFormat("%" B_PRId32 ":%02" B_PRId32, seconds / 60,
			seconds % 60);
	}
	return text;
}


PlayerView::PlayerView(Player* player)
	:
	// frame events: FrameResized() puts the overlay and the subtitles where
	// the new size wants them
	BView("player", B_WILL_DRAW | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE
		| B_FRAME_EVENTS),
	fPlayer(player),
	fOpened(false),
	fDirect(false),
	fOverlay(NULL),
	fOverlayShown(false),
	fControls(true),
	fControlsUntil(0),
	fLastMouse(-1, -1),
	fTicker(NULL),
	fSubtitleRect(0, 0, -1, -1),
	fSubtitleWidth(-1),
	fSubtitleHeight(-1),
	fSubtitleControls(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


PlayerView::~PlayerView()
{
	delete fTicker;
}


void
PlayerView::AttachedToWindow()
{
	BView::AttachedToWindow();
	delete fTicker;
	BMessage tick(kMsgPlayerTick);
	fTicker = new BMessageRunner(BMessenger(this), &tick, 500000);
}


void
PlayerView::DetachedFromWindow()
{
	delete fTicker;
	fTicker = NULL;
	BView::DetachedFromWindow();
}


void
PlayerView::Start(const char* title)
{
	fTitle = title;
	fOpened = false;
	fSubtitleLines.clear();
	fSubtitleShown = "";
	fSubtitleRect.Set(0, 0, -1, -1);
	fSubtitleWidth = -1;
	fSubtitleHeight = -1;
	fSubtitleControls = false;
	fAsking = false;
	_ShowControls();
	Invalidate();
}


void
PlayerView::SetOpened(bool opened)
{
	fOpened = opened;
	_ShowControls();
	Invalidate();
}


void
PlayerView::SetOverlay(BBitmap* overlay)
{
	ClearOverlay();
	if (overlay == NULL)
		return;
	fOverlay = overlay;
	fOverlayShown = false;
	_PlaceOverlay();
}


void
PlayerView::RefreshSubtitles()
{
	_UpdateSubtitle();
	Invalidate(_SubtitleButtonFrame());
}


void
PlayerView::DrawOverlay(BView* target)
{
	if (target == NULL)
		return;
	_DrawSubtitle(target);
	if (fControls || fPlayer->IsPaused())
		_DrawControls(target);
	_DrawStatistics(target);
}


void
PlayerView::SetDirect(bool direct)
{
	if (fDirect == direct)
		return;
	fDirect = direct;
	if (!fDirect)
		Invalidate();
}


bool
PlayerView::HasOverlayContent() const
{
	return !fSubtitleLines.empty() || fControls || fPlayer->IsPaused()
		|| !fStatsLines.empty();
}


void
PlayerView::ClearOverlay()
{
	if (fOverlay != NULL)
		ClearViewOverlay();
	fOverlay = NULL;
	fOverlayShown = false;
	Invalidate();
}


/*!	Where the screen puts the pictures: the color key marks it, and the
	controls drawn over it stay.
*/
bool
PlayerView::_PlaceOverlay()
{
	if (fOverlay == NULL)
		return true;
	BRect source = fOverlay->Bounds();
	BRect destination = _PictureFrame(fOverlay);
	uint32 options = overlay_filters() && (destination.Width() < source.Width()
		|| destination.Height() < source.Height())
		? B_OVERLAY_FILTER_HORIZONTAL | B_OVERLAY_FILTER_VERTICAL : 0;
	if (SetViewOverlay(fOverlay, source, destination, &fKey,
		B_FOLLOW_TOP | B_FOLLOW_LEFT, options) != B_OK) {
		ClearViewOverlay();
		fOverlay = NULL;
		fOverlayShown = false;
		Invalidate();
		fPlayer->DisableOverlay();
		return false;
	}
	Invalidate();
	return true;
}


void
PlayerView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	// given again from scratch: set a second time with the same bitmap, the
	// overlay of the AC100 kept its old size and place, and the new key
	// color around it showed as a green frame
	if (fOverlay != NULL)
		ClearViewOverlay();
	_PlaceOverlay();
	_UpdateSubtitle();
}


BRect
PlayerView::_PictureFrame(const BBitmap* picture) const
{
	BRect bounds = Bounds();
	BRect source = picture->Bounds();
	float scale = std::min((bounds.Width() + 1) / (source.Width() + 1),
		(bounds.Height() + 1) / (source.Height() + 1));
	float width = (source.Width() + 1) * scale;
	float height = (source.Height() + 1) * scale;
	BRect frame(0, 0, width - 1, height - 1);
	frame.OffsetTo(bounds.left + roundf((bounds.Width() + 1 - width) / 2),
		bounds.top + roundf((bounds.Height() + 1 - height) / 2));
	return frame;
}


BRect
PlayerView::_BarFrame() const
{
	return _BarFrame(Bounds());
}


BRect
PlayerView::_BarFrame(const BRect& bounds) const
{
	return BRect(bounds.left, bounds.bottom - S(kBaseBarHeight) + 1,
		bounds.right, bounds.bottom);
}


BRect
PlayerView::_TrackFrame() const
{
	return _TrackFrame(Bounds());
}


BRect
PlayerView::_TrackFrame(const BRect& bounds) const
{
	// where the time goes, between the button and the times
	// the times, the star and CC at the right
	BRect bar = _BarFrame(bounds);
	return BRect(bar.left + S(56), bar.top + S(22), bar.right - S(256),
		bar.top + S(30));
}


BRect
PlayerView::_SubtitleButtonFrame() const
{
	return _SubtitleButtonFrame(Bounds());
}


BRect
PlayerView::_SubtitleButtonFrame(const BRect& bounds) const
{
	BRect bar = _BarFrame(bounds);
	return BRect(bar.right - S(64), bar.top + S(12), bar.right - S(16),
		bar.bottom - S(12));
}


/*!	The volume at the right, between the title and the bar, when there is
	room for it.
*/
BRect
PlayerView::_VolumeFrame(const BRect& bounds) const
{
	float top = bounds.top + S(kBaseTitleHeight) + S(12);
	float bottom = bounds.bottom - S(kBaseBarHeight) - S(12);
	float height = std::min(S(180), bottom - top);
	if (height < S(90))
		return BRect();
	float middle = (top + bottom) / 2;
	return BRect(bounds.right - S(16) - S(48), middle - height / 2,
		bounds.right - S(16), middle + height / 2);
}


/*!	Where the level is drawn in it, and clicked: the percent below. */
BRect
PlayerView::_VolumeTrack(const BRect& bounds) const
{
	BRect panel = _VolumeFrame(bounds);
	if (!panel.IsValid())
		return BRect();
	float center = (panel.left + panel.right) / 2;
	return BRect(center - S(3), panel.top + S(14), center + S(3),
		panel.bottom - S(34));
}


BRect
PlayerView::_FavoriteButtonFrame() const
{
	return _FavoriteButtonFrame(Bounds());
}


/*!	At the left of CC. */
BRect
PlayerView::_FavoriteButtonFrame(const BRect& bounds) const
{
	BRect frame = _SubtitleButtonFrame(bounds);
	frame.OffsetBy(-S(56), 0);
	return frame;
}


void
PlayerView::SetFavorite(int32 favorite)
{
	if (favorite == fFavorite)
		return;
	fFavorite = favorite;
	if (Window() != NULL)
		Invalidate(_FavoriteButtonFrame());
}


void
PlayerView::Draw(BRect updateRect)
{
	if (fDirect)
		return;
	SetDrawingMode(B_OP_COPY);
	SetHighColor(0, 0, 0);

	if (fOverlay != NULL && fOverlayShown) {
		// the screen draws the picture where the key color is
		BRect frame = _PictureFrame(fOverlay);
		BRect bounds = Bounds();
		FillRect(BRect(bounds.left, bounds.top, bounds.right, frame.top - 1));
		FillRect(BRect(bounds.left, frame.bottom + 1, bounds.right,
			bounds.bottom));
		FillRect(BRect(bounds.left, frame.top, frame.left - 1, frame.bottom));
		FillRect(BRect(frame.right + 1, frame.top, bounds.right,
			frame.bottom));
		// paused: the picture itself, which a screenshot can take, rather
		// than the key color the screen replaces
		BBitmap* still = fPlayer->IsPaused() ? fPlayer->PausedPicture()
			: NULL;
		if (still != NULL)
			DrawBitmap(still, still->Bounds(), frame);
		else {
			SetHighColor(fKey);
			FillRect(frame);
		}
		_DrawSubtitle(this);
		if (fControls || fPlayer->IsPaused())
			_DrawControls(this);
		_DrawStatistics(this);
		_DrawQuestion(this);
		return;
	}

	BBitmap* picture = fOverlay != NULL ? NULL : fPlayer->LockFrame();
	if (picture != NULL) {
		BRect frame = _PictureFrame(picture);
		// the black around the picture only
		BRect bounds = Bounds();
		FillRect(BRect(bounds.left, bounds.top, bounds.right, frame.top - 1));
		FillRect(BRect(bounds.left, frame.bottom + 1, bounds.right,
			bounds.bottom));
		FillRect(BRect(bounds.left, frame.top, frame.left - 1, frame.bottom));
		FillRect(BRect(frame.right + 1, frame.top, bounds.right,
			frame.bottom));
		// no filtering: on a slow machine it costs more than the decoding
		DrawBitmap(picture, picture->Bounds(), frame);
		fPlayer->NoteDrawn();
	} else
		FillRect(Bounds());
	if (fOverlay == NULL)
		fPlayer->UnlockFrame();

	_DrawSubtitle(this);
	if (picture == NULL) {
		DrawCentered(this, B_TRANSLATE("Getting the video ready"), Bounds(),
			Font(18), kTextDim);
	}
	if (fControls || fPlayer->IsPaused() || picture == NULL)
		_DrawControls(this);
	_DrawStatistics(this);
	_DrawQuestion(this);
}


/*!	Asked once, on a graphics card Cinema does not know: does the video show
	through its overlay, or only the green of its key color? The answer
	comes as kMsgOverlayAnswer. NULL takes the question away.
*/
void
PlayerView::SetOverlayQuestion(const char* card)
{
	bool asking = card != NULL;
	if (asking == fAsking && (!asking || fCard == card))
		return;
	if (fAsking)
		Invalidate(_QuestionFrame());
	fAsking = asking;
	fCard = card != NULL ? card : "";
	if (fAsking)
		Invalidate(_QuestionFrame());
}


BRect
PlayerView::_QuestionFrame() const
{
	BRect bounds = Bounds();
	BFont font = Font(16, true);
	BFont small = Font(13);
	float width = std::max(font.StringWidth(B_TRANSLATE("Can you see the "
		"video?")), small.StringWidth(fCard.String()));
	width = std::min(bounds.Width() - S(32), std::max(width, S(220)) + S(40));
	float top = bounds.top + S(kBaseTitleHeight) + S(16);
	float left = bounds.left + roundf((bounds.Width() - width) / 2);
	return BRect(left, top, left + width, top + S(118));
}


BRect
PlayerView::_YesFrame() const
{
	BRect frame = _QuestionFrame();
	float middle = (frame.left + frame.right) / 2;
	return BRect(middle - S(8) - S(90), frame.bottom - S(46),
		middle - S(8), frame.bottom - S(14));
}


BRect
PlayerView::_NoFrame() const
{
	BRect frame = _QuestionFrame();
	float middle = (frame.left + frame.right) / 2;
	return BRect(middle + S(8), frame.bottom - S(46), middle + S(8) + S(90),
		frame.bottom - S(14));
}


void
PlayerView::_DrawQuestion(BView* target)
{
	if (!fAsking)
		return;
	BRect frame = _QuestionFrame();
	FillRound(target, frame, S(10), (rgb_color){ 20, 20, 24, 235 });
	BFont font = Font(16, true);
	BFont small = Font(13);
	font_height height;
	font.GetHeight(&height);
	DrawCentered(target, B_TRANSLATE("Can you see the video?"),
		BRect(frame.left, frame.top + S(12), frame.right,
			frame.top + S(36)), font, kText);
	BString card(fCard);
	small.TruncateString(&card, B_TRUNCATE_MIDDLE, frame.Width() - S(24));
	DrawCentered(target, card.String(), BRect(frame.left, frame.top + S(38),
		frame.right, frame.top + S(58)), small, kTextDim);
	FillRound(target, _YesFrame(), S(8), kAccent);
	DrawCentered(target, B_TRANSLATE("Yes"), _YesFrame(), Font(14, true),
		kText);
	FillRound(target, _NoFrame(), S(8), (rgb_color){ 60, 60, 66, 255 });
	DrawCentered(target, B_TRANSLATE("No"), _NoFrame(), Font(14, true),
		kText);
}


/*!	The lines of the statistics, from the player, and where they go: at
	the top left, under the title.
*/
void
PlayerView::_UpdateStatistics()
{
	std::vector<BString> lines;
	PlayerStatistics stats;
	if (!fShowStats || !fPlayer->IsOpen())
		;	// none
	else if (!fPlayer->Statistics(stats))
		lines.push_back(B_TRANSLATE("No statistics for this video"));
	else {
		BString line;
		line.SetToFormat("%" B_PRId32 "x%" B_PRId32 " %s \xC2\xB7 %.0f fps",
			stats.width, stats.height, stats.videoCodec.String(), stats.fps);
		if (stats.bitRate > 0)
			line << " \xC2\xB7 " << (int32)(stats.bitRate / 1000) << " kb/s";
		lines.push_back(line);
		if (!stats.audioCodec.IsEmpty()) {
			line.SetToFormat("%s %.1f kHz %" B_PRId32 " ch",
				stats.audioCodec.String(), stats.sampleRate / 1000.0,
				stats.channels);
			lines.push_back(line);
		}
		const char* source = B_TRANSLATE("From the web");
		if (strcmp(stats.source, "file") == 0)
			source = B_TRANSLATE("From a file");
		else if (strcmp(stats.source, "relay") == 0)
			source = B_TRANSLATE("From YouTube, through the helper");
		else if (strcmp(stats.source, "torrent") == 0)
			source = B_TRANSLATE("From a torrent, as it downloads");
		else if (strcmp(stats.source, "stream") == 0)
			source = B_TRANSLATE("Live stream");
		line = source;
		line << " \xC2\xB7 " << (stats.overlay
			? B_TRANSLATE("video overlay") : B_TRANSLATE("drawn by Cinema"));
		lines.push_back(line);
		const char* decoding = B_TRANSLATE("Decoding every picture");
		if (stats.skipLevel == 1)
			decoding = B_TRANSLATE("Decoding without the deblocking filter");
		else if (stats.skipLevel == 2)
			decoding = B_TRANSLATE("Decoding the reference pictures only");
		else if (stats.skipLevel >= 3)
			decoding = B_TRANSLATE("Decoding the key frames only");
		line = decoding;
		BString dropped(B_TRANSLATE("%count% dropped"));
		BString count;
		count << stats.dropped;
		dropped.ReplaceFirst("%count%", count.String());
		line << " \xC2\xB7 " << dropped;
		lines.push_back(line);
		BString load(B_TRANSLATE("Processor: Cinema %cinema%%, all %all%%"));
		BString number;
		number.SetToFormat("%.0f", stats.cinemaLoad);
		load.ReplaceFirst("%cinema%", number.String());
		number.SetToFormat("%.0f", stats.systemLoad);
		load.ReplaceFirst("%all%", number.String());
		lines.push_back(load);
		BString sound(B_TRANSLATE("Sound ready: %seconds% s"));
		number.SetToFormat("%.1f", stats.soundAhead);
		sound.ReplaceFirst("%seconds%", number.String());
		if (stats.evenVolume) {
			BString gain(B_TRANSLATE("even volume %gain% dB"));
			number.SetToFormat("%+.1f", stats.gainDB);
			gain.ReplaceFirst("%gain%", number.String());
			sound << " \xC2\xB7 " << gain;
		}
		lines.push_back(sound);
	}

	BFont font = Font(12);
	font_height height;
	font.GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent + S(2));
	float width = 0;
	for (const BString& line : lines)
		width = std::max(width, font.StringWidth(line.String()));
	BRect frame;
	if (!lines.empty()) {
		frame.left = Bounds().left + S(12);
		frame.top = Bounds().top + S(kBaseTitleHeight) + S(8);
		frame.right = frame.left + ceilf(width) + S(16);
		frame.bottom = frame.top + lineHeight * lines.size() + S(10);
	}
	if (lines == fStatsLines && frame == fStatsRect)
		return;
	if (fStatsRect.IsValid())
		Invalidate(fStatsRect);
	fStatsLines = lines;
	fStatsRect = frame;
	if (fStatsRect.IsValid())
		Invalidate(fStatsRect);
}


void
PlayerView::_DrawStatistics(BView* target)
{
	if (fStatsLines.empty() || !fStatsRect.IsValid())
		return;
	bool composite = target != this;
	FillRound(target, fStatsRect, S(6), (rgb_color){ 0, 0, 0, 170 },
		composite);
	BFont font = Font(12);
	font_height height;
	font.GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent + S(2));
	float y = fStatsRect.top + S(5) + height.ascent;
	for (const BString& line : fStatsLines) {
		DrawText(target, line.String(), BPoint(fStatsRect.left + S(8), y),
			font, kText, composite);
		y += lineHeight;
	}
}


void
PlayerView::_DrawControls(BView* target)
{
	BRect bounds = target->Bounds();
	bool composite = target != this;

	// the title, at the top
	BRect top(bounds.left, bounds.top, bounds.right,
		bounds.top + S(kBaseTitleHeight) - 1);
	target->SetDrawingMode(B_OP_ALPHA);
	target->SetBlendingMode(B_PIXEL_ALPHA,
		composite ? B_ALPHA_COMPOSITE : B_ALPHA_OVERLAY);
	target->SetHighColor(0, 0, 0, 170);
	target->FillRect(top);
	// the volume, upright at the right as the arrow keys that change it
	BFont small = Font(14);
	font_height smallHeight;
	small.GetHeight(&smallHeight);
	BRect panel = _VolumeFrame(bounds);
	if (panel.IsValid()) {
		FillRound(target, panel, S(10), (rgb_color){ 0, 0, 0, 170 },
			composite);
		BRect level = _VolumeTrack(bounds);
		FillRound(target, level, S(3), (rgb_color){ 255, 255, 255, 60 },
			composite);
		if (fPlayer->Volume() > 0) {
			BRect done = level;
			done.top = level.bottom - level.Height() * fPlayer->Volume();
			FillRound(target, done, S(3), kAccent, composite);
		}
		BString percent;
		percent.SetToFormat("%d%%", (int)roundf(fPlayer->Volume() * 100));
		BRect text(panel.left, level.bottom + S(4), panel.right,
			panel.bottom - S(4));
		DrawCentered(target, percent.String(), text, small, kText, composite);
	}

	BString title(fTitle);
	BFont font = Font(16, true);
	font.TruncateString(&title, B_TRUNCATE_END,
		top.Width() - S(32));
	font_height height;
	font.GetHeight(&height);
	DrawText(target, title.String(), BPoint(top.left + S(16),
		top.top + (S(kBaseTitleHeight) + height.ascent - height.descent) / 2),
		font, kText, composite);

	// the bar: play or pause, where it is, the times
	BRect bar = _BarFrame(bounds);
	target->SetDrawingMode(B_OP_ALPHA);
	target->SetBlendingMode(B_PIXEL_ALPHA,
		composite ? B_ALPHA_COMPOSITE : B_ALPHA_OVERLAY);
	target->SetHighColor(0, 0, 0, 170);
	target->FillRect(bar);

	BRect button(bar.left + S(18), bar.top + S(16), bar.left + S(38),
		bar.top + S(36));
	target->SetHighColor(kText);
	if (fPlayer->IsPaused())
		DrawPlay(target, button, kText, composite);
	else {
		target->FillRect(BRect(button.left + S(2), button.top,
			button.left + S(7), button.bottom));
		target->FillRect(BRect(button.right - S(7), button.top,
			button.right - S(2), button.bottom));
	}

	bigtime_t position = fPlayer->Position();
	bigtime_t duration = fPlayer->Duration();
	BRect track = _TrackFrame(bounds);
	FillRound(target, track, S(4), (rgb_color){ 255, 255, 255, 60 },
		composite);
	if (duration > 0) {
		BRect done = track;
		done.right = track.left + track.Width()
			* std::min(1.0, (double)position / duration);
		if (done.right > done.left)
			FillRound(target, done, S(4), kAccent, composite);
	}

	BString times = time_text(position);
	if (duration > 0)
		times << " / " << time_text(duration);
	small.GetHeight(&height);
	DrawText(target, times.String(), BPoint(track.right + S(16),
		track.top + (track.Height() + height.ascent - height.descent) / 2),
		small, kText, composite);

	// the subtitles: off, or the chosen track
	BRect subtitle = _SubtitleButtonFrame(bounds);
	bool on = fPlayer->SelectedSubtitle() >= 0
		&& fPlayer->SubtitleCount() > 0;
	FillRound(target, subtitle, S(6), on
		? kAccent : (rgb_color){ 255, 255, 255, 40 }, composite);
	DrawCentered(target, "CC", subtitle, Font(14, true),
		on ? kText : kTextDim, composite);

	// the star of the video, as on its page
	if (fFavorite >= 0) {
		BRect star = _FavoriteButtonFrame(bounds);
		FillRound(target, star, S(6), (rgb_color){ 255, 255, 255, 40 },
			composite);
		BPoint center(star.left + star.Width() / 2,
			star.top + star.Height() / 2 + S(1));
		float outer = star.Height() * 0.36f;
		float inner = outer * 0.42f;
		BPoint points[10];
		for (int32 i = 0; i < 10; i++) {
			float radius = i % 2 == 0 ? outer : inner;
			float angle = -M_PI / 2 + i * M_PI / 5;
			points[i] = BPoint(center.x + radius * cosf(angle),
				center.y + radius * sinf(angle));
		}
		target->SetDrawingMode(B_OP_ALPHA);
		target->SetBlendingMode(B_PIXEL_ALPHA,
			composite ? B_ALPHA_COMPOSITE : B_ALPHA_OVERLAY);
		if (fFavorite > 0) {
			target->SetHighColor(kAccent);
			target->FillPolygon(points, 10);
		} else {
			target->SetHighColor(kText);
			target->SetPenSize(S(1.5f));
			target->StrokePolygon(points, 10);
			target->SetPenSize(1);
		}
	}
}


void
PlayerView::_SubtitleLayout(const BString& text,
	std::vector<BString>& lines, BRect& frame) const
{
	lines.clear();
	frame.Set(0, 0, -1, -1);
	if (text.IsEmpty())
		return;

	BFont font = Font(kBaseSubtitleSize, true);
	float maxWidth = std::max(S(120), Bounds().Width() - S(64));
	wrap_text(font, maxWidth, text, lines);
	if (lines.empty())
		return;

	float width = 0;
	for (const BString& line : lines)
		width = std::max(width, font.StringWidth(line.String()));

	font_height height;
	font.GetHeight(&height);
	float lineHeight = height.ascent + height.descent + S(8);
	float total = lines.size() * lineHeight + S(14);
	float bottom = Bounds().bottom - S(14);
	if (fControls || fPlayer->IsPaused())
		bottom -= S(kBaseBarHeight);
	float top = std::max(Bounds().top + S(14), bottom - total);
	float center = (Bounds().left + Bounds().right) / 2;
	frame.Set(center - width / 2 - S(16), top, center + width / 2 + S(16),
		bottom);
}


void
PlayerView::_DrawSubtitle(BView* target)
{
	if (fSubtitleLines.empty() || !fSubtitleRect.IsValid())
		return;
	bool composite = target != this;

	BFont font = Font(kBaseSubtitleSize, true);
	font_height height;
	font.GetHeight(&height);
	float lineHeight = height.ascent + height.descent + S(8);
	float y = fSubtitleRect.top + S(7) + height.ascent;
	for (const BString& line : fSubtitleLines) {
		float width = font.StringWidth(line.String());
		float x = (target->Bounds().left + target->Bounds().right
			- width) / 2;
		BRect background(x - S(12), y - height.ascent - S(3),
			x + width + S(12), y + height.descent + S(3));
		FillRound(target, background, S(6),
			(rgb_color){ 0, 0, 0, 190 }, composite);
		DrawText(target, line.String(), BPoint(x, y), font, kText,
			composite);
		y += lineHeight;
	}
}


void
PlayerView::_UpdateSubtitle()
{
	BString text = fPlayer->IsOpen()
		? fPlayer->SubtitleText(fPlayer->Position()) : "";
	bool withControls = fControls || fPlayer->IsPaused();
	float width = Bounds().Width();
	float height = Bounds().Height();
	if (text == fSubtitleShown && width == fSubtitleWidth
		&& height == fSubtitleHeight && withControls == fSubtitleControls)
		return;

	std::vector<BString> lines;
	BRect frame;
	_SubtitleLayout(text, lines, frame);

	if (fSubtitleRect.IsValid() && !fSubtitleShown.IsEmpty())
		Invalidate(fSubtitleRect);
	if (!text.IsEmpty())
		Invalidate(frame);
	fSubtitleShown = text;
	fSubtitleLines = lines;
	fSubtitleRect = frame;
	fSubtitleWidth = width;
	fSubtitleHeight = height;
	fSubtitleControls = withControls;
}


void
PlayerView::_RequestSelectedSubtitle()
{
	int32 index = fPlayer->SelectedSubtitle();
	SubtitleTrackInfo info;
	if (index < 0 || !fPlayer->SubtitleTrack(index, &info)
		|| info.loaded || info.language.IsEmpty())
		return;

	BMessage request(kMsgSubtitleRequest);
	request.AddString("language", info.language);
	request.AddBool("automatic", info.automatic);
	Window()->PostMessage(&request);
}


void
PlayerView::_ChooseSubtitle(int32 index)
{
	fPlayer->SelectSubtitle(index);
	_RequestSelectedSubtitle();
	_UpdateSubtitle();
	Invalidate();
	_ShowControls();
	Window()->PostMessage(kMsgSubtitleChosen);
}


void
PlayerView::_ShowSubtitleMenu()
{
	BPopUpMenu menu(B_TRANSLATE("Subtitles"), false, true);

	BMessage* offMessage = new BMessage(kMsgSubtitlePick);
	offMessage->AddInt32("track", -1);
	BMenuItem* off = new BMenuItem(B_TRANSLATE("No subtitles"),
		offMessage);
	if (fPlayer->SelectedSubtitle() < 0)
		off->SetMarked(true);
	menu.AddItem(off);

	int32 count = fPlayer->SubtitleCount();
	for (int32 i = 0; i < count; i++) {
		SubtitleTrackInfo info;
		if (!fPlayer->SubtitleTrack(i, &info))
			continue;
		BString label = info.name;
		if (label.IsEmpty())
			label = info.language;
		if (label.IsEmpty())
			label = B_TRANSLATE("Subtitles");
		if (info.automatic)
			label << " (" << B_TRANSLATE("automatic") << ")";

		BMessage* message = new BMessage(kMsgSubtitlePick);
		message->AddInt32("track", i);
		BMenuItem* item = new BMenuItem(label.String(), message);
		if (i == fPlayer->SelectedSubtitle())
			item->SetMarked(true);
		menu.AddItem(item);
	}
	if (count == 0) {
		BMenuItem* none = new BMenuItem(
			B_TRANSLATE("No subtitles available"), NULL);
		none->SetEnabled(false);
		menu.AddItem(none);
	}

	Style::ScaleMenu(&menu);
	CinemaWindow* window = dynamic_cast<CinemaWindow*>(Window());
	if (window != NULL)
		window->SetDirectPaused(true);
	BRect button = _SubtitleButtonFrame();
	BMenuItem* chosen = menu.Go(ConvertToScreen(BPoint(button.left,
		button.top)), false, false, false);
	if (window != NULL)
		window->SetDirectPaused(false);
	if (chosen != NULL && chosen->Message() != NULL) {
		int32 track = -1;
		chosen->Message()->FindInt32("track", &track);
		_ChooseSubtitle(track);
	}
}


void
PlayerView::_ShowControls()
{
	bool appeared = !fControls;
	fControls = true;
	fControlsUntil = system_time() + kControlsTime;
	if (appeared)
		_UpdateSubtitle();
	Invalidate();
}


void
PlayerView::_TogglePause()
{
	fPlayer->SetPaused(!fPlayer->IsPaused());
	_UpdateSubtitle();
	_ShowControls();
	// the picture in place of the key color, or the key color back
	Invalidate();
}


void
PlayerView::_SeekBy(bigtime_t delta)
{
	fPlayer->Seek(fPlayer->Position() + delta);
	_UpdateSubtitle();
	_ShowControls();
}


void
PlayerView::KeyDown(const char* bytes, int32 count)
{
	int32 modifiers = 0;
	if (Window()->CurrentMessage() != NULL)
		Window()->CurrentMessage()->FindInt32("modifiers", &modifiers);
	bigtime_t step = (modifiers & B_SHIFT_KEY) != 0 ? kBigStep : kStep;

	switch (bytes[0]) {
		case B_SPACE:
		case B_ENTER:
			_TogglePause();
			break;
		case B_LEFT_ARROW:
			_SeekBy(-step);
			break;
		case B_RIGHT_ARROW:
			_SeekBy(step);
			break;
		case B_UP_ARROW:
			fPlayer->SetVolume(fPlayer->Volume() + 0.1f);
			_ShowControls();
			break;
		case B_DOWN_ARROW:
			fPlayer->SetVolume(fPlayer->Volume() - 0.1f);
			_ShowControls();
			break;
		case 'f':
		case 'F':
			Window()->PostMessage(kMsgFullScreen);
			break;
		case 'm':
		case 'M':
			Window()->PostMessage(kMsgPlayInMediaPlayer);
			break;
		case 'i':
		case 'I':
			fShowStats = !fShowStats;
			_UpdateStatistics();
			break;
		case 'o':
		case 'O':
			// only a green background: the picture drawn by Cinema instead
			Window()->PostMessage(kMsgToggleOverlay);
			break;
		case 's':
		case 'S':
			if (fFavorite >= 0) {
				Window()->PostMessage(kMsgPlayerFavorite);
				_ShowControls();
			}
			break;
		case 'v':
		case 'V':
			fPlayer->ToggleSubtitles();
			_RequestSelectedSubtitle();
			_UpdateSubtitle();
			Invalidate(_SubtitleButtonFrame());
			_ShowControls();
			Window()->PostMessage(kMsgSubtitleChosen);
			break;
		default:
			BView::KeyDown(bytes, count);
	}
}


void
PlayerView::MouseDown(BPoint where)
{
	MakeFocus(true);
	int32 clicks = 1;
	if (Window()->CurrentMessage() != NULL)
		Window()->CurrentMessage()->FindInt32("clicks", &clicks);

	if (fAsking && _QuestionFrame().Contains(where)) {
		bool yes = _YesFrame().Contains(where);
		if (yes || _NoFrame().Contains(where)) {
			BMessage answer(kMsgOverlayAnswer);
			answer.AddBool("works", yes);
			Window()->PostMessage(&answer);
		}
		return;
	}

	if ((fControls || fPlayer->IsPaused())
		&& _SubtitleButtonFrame().Contains(where)) {
		_ShowSubtitleMenu();
		return;
	}
	BRect volume = _VolumeFrame(Bounds());
	if ((fControls || fPlayer->IsPaused()) && volume.IsValid()
		&& volume.Contains(where)) {
		// from the bottom of the level, in tenths
		BRect track = _VolumeTrack(Bounds());
		float fraction = (track.bottom - where.y) / std::max(1.0f,
			track.Height());
		fPlayer->SetVolume(std::max(0.0f, std::min(1.0f, fraction)));
		_ShowControls();
		return;
	}
	if ((fControls || fPlayer->IsPaused()) && fFavorite >= 0
		&& _FavoriteButtonFrame().Contains(where)) {
		Window()->PostMessage(kMsgPlayerFavorite);
		_ShowControls();
		return;
	}

	BRect track = _TrackFrame().InsetByCopy(0, -S(10));
	if ((fControls || fPlayer->IsPaused()) && track.Contains(where)
		&& fPlayer->Duration() > 0) {
		double fraction = (where.x - track.left) / track.Width();
		fPlayer->Seek((bigtime_t)(fPlayer->Duration()
			* std::max(0.0, std::min(1.0, fraction))));
		_UpdateSubtitle();
		_ShowControls();
		return;
	}
	if (clicks >= 2) {
		Window()->PostMessage(kMsgFullScreen);
		// the first click paused it
		_TogglePause();
		return;
	}
	_TogglePause();
}


void
PlayerView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (where != fLastMouse) {
		fLastMouse = where;
		fMouseMovedAt = system_time();
		fCursorObscured = false;
		_ShowControls();
	}
}


void
PlayerView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_MOUSE_WHEEL_CHANGED:
		{
			// up louder, as the arrow key
			float delta = message->GetFloat("be:wheel_delta_y", 0);
			if (delta != 0) {
				fPlayer->SetVolume(fPlayer->Volume()
					+ (delta < 0 ? 0.1f : -0.1f));
				_ShowControls();
			}
			break;
		}
		case kMsgPlayerFrame:
			if (fOverlay == NULL) {
				_UpdateSubtitle();
				// locked whether there is a picture or not: the lock held
				// would stop the player, and Close() wait for it for ever
				BBitmap* picture = fPlayer->LockFrame();
				if (picture != NULL)
					Invalidate(_PictureFrame(picture));
				else
					Invalidate();
				fPlayer->UnlockFrame();
			} else if (!fOverlayShown) {
				// the first picture: the key color, once; taken too, so
				// that the next one comes whatever Draw() does
				fOverlayShown = true;
				_UpdateSubtitle();
				Invalidate();
				fPlayer->LockFrame();
				fPlayer->UnlockFrame();
			} else {
				// nothing to draw: only taken, so that the next one comes
				fPlayer->LockFrame();
				fPlayer->UnlockFrame();
			}
			break;
		case kMsgPlayerTick:
			// the time goes on, and the controls hide while it plays
			if (fControls && !fPlayer->IsPaused()
				&& system_time() > fControlsUntil && fOpened) {
				// once, to take them away
				fControls = false;
				Invalidate();
			} else if (fControls || fPlayer->IsPaused())
				Invalidate(_BarFrame());
			_UpdateSubtitle();
			if (fShowStats)
				_UpdateStatistics();
			// the pointer out of the picture after 10 s still, while it
			// plays: the app_server shows it again when the mouse moves
			{
				// where the pointer is now: MouseMoved() only tells once it
				// moves over this view, not where it already rests
				BPoint pointer;
				uint32 buttons;
				GetMouse(&pointer, &buttons, false);
				if (fMouseMovedAt == 0 || pointer != fTickPointer) {
					fTickPointer = pointer;
					fMouseMovedAt = system_time();
					fCursorObscured = false;
				}
			}
			if (!fCursorObscured && fOpened && !fPlayer->IsPaused()
				&& Window()->IsActive() && Bounds().Contains(fTickPointer)
				&& system_time() - fMouseMovedAt > 10000000) {
				be_app->ObscureCursor();
				fCursorObscured = true;
				if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
					fprintf(stderr, "player: pointer hidden\n");
			}
			{
				// CINEMA_TEST_SEEK=<seconds>: a step on that often, to try
				// seeking without the keys
				static const int32 every = getenv("CINEMA_TEST_SEEK") != NULL
					? atoi(getenv("CINEMA_TEST_SEEK")) : 0;
				// CINEMA_TEST_PAUSE=<seconds>: paused once after that long
				static const int32 pauseAfter
					= getenv("CINEMA_TEST_PAUSE") != NULL
						? atoi(getenv("CINEMA_TEST_PAUSE")) : 0;
				static bigtime_t pauseAt = 0;
				if (pauseAfter > 0 && fOpened && pauseAt >= 0) {
					if (pauseAt == 0)
						pauseAt = system_time() + pauseAfter * 1000000LL;
					else if (system_time() > pauseAt) {
						pauseAt = -1;
						_TogglePause();
					}
				}
				static bigtime_t next = 0;
				if (every > 0 && fOpened && !fPlayer->IsPaused()) {
					if (next == 0)
						next = system_time() + every * 1000000LL;
					else if (system_time() > next) {
						next = system_time() + every * 1000000LL;
						_SeekBy(kStep);
					}
				}
			}
			break;
		default:
			BView::MessageReceived(message);
	}
}
