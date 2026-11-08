/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "VideoGrid.h"

#include <algorithm>
#include <math.h>

#include <Bitmap.h>
#include <Cursor.h>
#include <Catalog.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <Window.h>

#include "Format.h"
#include "Messages.h"
#include "Style.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Grid"


using namespace Style;


static const float kBaseMinTileWidth = 260;
static const float kBaseGap = 20;
static const float kBaseTitleHeight = 92;
	// two lines of title, one with the channel, one with the views and how
	// long ago it came out
static const float kTitleSize = 15;
static const float kChannelSize = 13;
static const float kBaseRadius = 6;


VideoGrid::VideoGrid()
	:
	BView("videos", B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE
		| B_FULL_UPDATE_ON_RESIZE | B_SCROLL_VIEW_AWARE),
	fSelected(0),
	fHover(-1),
	fHoverChannel(-1),
	fDots(0),
	fShelf(false),
	fShelfTileWidth(0),
	fAnimation(NULL)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


VideoGrid::~VideoGrid()
{
	delete fAnimation;
	_Clear();
}


void
VideoGrid::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (!fShelf)
		MakeFocus(true);
	_UpdateScrollBar();
}


void
VideoGrid::_Clear()
{
	for (MediaItem& video : fVideos)
		delete video.picture;
	fVideos.clear();
}


void
VideoGrid::SetVideos(const std::vector<MediaItem>& videos)
{
	_Clear();
	fVideos = videos;
	fSelected = 0;
	fHover = -1;
	fHoverChannel = -1;
	fMessage = "";
	ScrollTo(0, 0);
	_UpdateScrollBar();
	Invalidate();
}


void
VideoGrid::SetShelfMode(float tileWidth)
{
	fShelf = true;
	fShelfTileWidth = tileWidth;
	_UpdateScrollBar();
	Invalidate();
}


float
VideoGrid::ShelfHeightFor(float tileWidth)
{
	float pictureHeight = roundf(tileWidth * 9 / 16);
	return 2 * S(kBaseGap) + pictureHeight + S(kBaseTitleHeight);
}


float
VideoGrid::ShelfHeight() const
{
	return ShelfHeightFor(fShelfTileWidth);
}


bool
VideoGrid::HasVideo(const char* id) const
{
	for (const MediaItem& video : fVideos)
		if (video.id == id)
			return true;
	return false;
}


void
VideoGrid::SetPicture(const char* id, BBitmap* picture)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			delete fVideos[i].picture;
			fVideos[i].picture = picture;
			Invalidate(_TileFrame(i));
			return;
		}
	}
	delete picture;
}


void
VideoGrid::SetProgress(const char* id, int32 progress)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			if (fVideos[i].progress != progress) {
				fVideos[i].progress = progress;
				Invalidate(_TileFrame(i));
			}
			return;
		}
	}
}


void
VideoGrid::SetStatus(const char* id, const char* status)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			if (fVideos[i].status != status) {
				fVideos[i].status = status;
				Invalidate(_TileFrame(i));
			}
			return;
		}
	}
}


void
VideoGrid::SetWatched(const char* id, bool watched, int32 resume)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			fVideos[i].watched = watched;
			fVideos[i].resume = resume;
			Invalidate(_TileFrame(i));
			return;
		}
	}
}


bool
VideoGrid::NextOf(const char* id, MediaItem& next) const
{
	for (size_t i = 0; i + 1 < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			next = fVideos[i + 1];
			// the picture stays with the grid
			next.picture = NULL;
			return true;
		}
	}
	return false;
}


void
VideoGrid::Select(const char* id)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			_Select(i);
			return;
		}
	}
}


void
VideoGrid::SetDownloaded(const char* id, bool downloaded)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			fVideos[i].downloaded = downloaded;
			fVideos[i].progress = -1;
			Invalidate(_TileFrame(i));
			return;
		}
	}
}


void
VideoGrid::SetFavorite(const char* id, bool favorite)
{
	for (size_t i = 0; i < fVideos.size(); i++) {
		if (fVideos[i].id == id) {
			fVideos[i].favorite = favorite;
			Invalidate(_TileFrame(i));
		}
	}
}


void
VideoGrid::SetMessage(const char* message)
{
	_Clear();
	// nothing left to point at
	fSelected = 0;
	fHover = -1;
	fHoverChannel = -1;
	fMessage = message;
	ScrollTo(0, 0);
	_UpdateScrollBar();
	Invalidate();
}


void
VideoGrid::SetBusy(const char* title)
{
	fBusy = title != NULL ? title : "";
	delete fAnimation;
	fAnimation = NULL;
	if (title != NULL) {
		fAnimation = new BMessageRunner(BMessenger(this),
			new BMessage(kMsgAnimate), 400000);
	}
	Invalidate();
}


int32
VideoGrid::_Columns() const
{
	if (fShelf) {
		float width = Bounds().Width() - 2 * S(kBaseGap) + S(kBaseGap);
		return std::max<int32>(1,
			(int32)(width / (fShelfTileWidth + S(kBaseGap))));
	}
	float width = Bounds().Width() + 1 - S(kBaseGap);
	return std::max<int32>(1, (int32)(width / (S(kBaseMinTileWidth) + S(kBaseGap))));
}


int32
VideoGrid::_VisibleCount() const
{
	if (!fShelf)
		return fVideos.size();
	return std::min<int32>(fVideos.size(), _Columns());
}


BRect
VideoGrid::_TileFrame(int32 index) const
{
	if (fShelf) {
		float pictureHeight = roundf(fShelfTileWidth * 9 / 16);
		float tileHeight = pictureHeight + S(kBaseTitleHeight);
		float left = S(kBaseGap) + (index % _Columns()) * (fShelfTileWidth + S(kBaseGap));
		return BRect(left, S(kBaseGap), left + fShelfTileWidth,
			S(kBaseGap) + tileHeight);
	}
	int32 columns = _Columns();
	float tileWidth = (Bounds().Width() + 1 - S(kBaseGap)) / columns - S(kBaseGap);
	float pictureHeight = roundf(tileWidth * 9 / 16);
	float tileHeight = pictureHeight + S(kBaseTitleHeight);
	float left = S(kBaseGap) + (index % columns) * (tileWidth + S(kBaseGap));
	float top = S(kBaseGap) + (index / columns) * (tileHeight + S(kBaseGap));
	return BRect(left, top, left + tileWidth, top + tileHeight);
}


int32
VideoGrid::_TileAt(BPoint where) const
{
	for (int32 i = 0; i < _VisibleCount(); i++) {
		if (_TileFrame(i).Contains(where))
			return i;
	}
	return -1;
}


static float
line_height(const BFont& font)
{
	font_height height;
	font.GetHeight(&height);
	return ceilf(height.ascent + height.descent + height.leading);
}


BRect
VideoGrid::_ChannelFrame(int32 index) const
{
	if (index < 0 || index >= (int32)fVideos.size())
		return BRect();
	const MediaItem& video = fVideos[index];
	if (video.channel.IsEmpty() || video.channelId.IsEmpty())
		return BRect();
	BRect frame = _TileFrame(index);
	BFont font = Font(kChannelSize);
	float top = frame.bottom - S(kBaseTitleHeight) + S(8)
		+ 2 * line_height(Font(kTitleSize)) + S(4);
	float width = std::min(frame.Width(),
		font.StringWidth(video.channel.String()));
	return BRect(frame.left, top, frame.left + width,
		top + line_height(font));
}


int32
VideoGrid::_ChannelAt(BPoint where) const
{
	int32 index = _TileAt(where);
	if (index >= 0 && _ChannelFrame(index).Contains(where))
		return index;
	return -1;
}


void
VideoGrid::_UpdateScrollBar()
{
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (bar == NULL)
		return;
	if (fShelf) {
		bar->SetRange(0, 0);
		bar->SetProportion(1);
		return;
	}
	float content = fVideos.empty() ? 0
		: _TileFrame(fVideos.size() - 1).bottom + S(kBaseGap);
	float visible = Bounds().Height() + 1;
	float range = std::max(0.0f, content - visible);
	bar->SetRange(0, range);
	bar->SetProportion(content > 0 ? std::min(1.0f, visible / content) : 1);
	bar->SetSteps(S(40), visible * 0.9f);
}


void
VideoGrid::FrameResized(float width, float height)
{
	_UpdateScrollBar();
}


void
VideoGrid::MakeFocus(bool focus)
{
	BView::MakeFocus(focus);
	Invalidate();
	if (focus && fShelf && Window() != NULL)
		Window()->PostMessage(kMsgShelfFocus);
}


void
VideoGrid::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(kBackground);
	FillRect(updateRect);

	BRect bounds = Bounds();
	if (fVideos.empty()) {
		DrawCentered(this, fMessage.String(), BRect(bounds.left,
			bounds.top + bounds.Height() / 2 - 40, bounds.right,
			bounds.top + bounds.Height() / 2), Font(18), kTextDim);
	} else {
		for (int32 i = 0; i < _VisibleCount(); i++) {
			BRect frame = _TileFrame(i);
			if (frame.Intersects(updateRect))
				_DrawTile(i, frame);
		}
	}

	if (!fBusy.IsEmpty())
		_DrawBusy();
}


static BString
duration_text(int32 seconds)
{
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


void
VideoGrid::_DrawTile(int32 index, BRect frame)
{
	const MediaItem& video = fVideos[index];
	bool selected = index == fSelected && (!fShelf || IsFocus());
	bool hover = index == fHover;

	BRect picture(frame.left, frame.top, frame.right,
		frame.bottom - S(kBaseTitleHeight));
	FillRound(this, picture, S(kBaseRadius), kCard);
	if (video.picture != NULL) {
		SetDrawingMode(B_OP_COPY);
		BRect source = video.picture->Bounds();
		BRect target = picture;
		float aspect = (source.Width() + 1) / (source.Height() + 1);
		if (aspect < 1.4f) {
			// a poster, upright: whole, in the middle, not stretched
			float width = (picture.Height() + 1) * aspect;
			target.left = picture.left + roundf((picture.Width() - width) / 2);
			target.right = target.left + width - 1;
		}
		DrawBitmap(video.picture, source, target, B_FILTER_BITMAP_BILINEAR);
	} else {
		BRect play(0, 0, S(36), S(36));
		play.OffsetTo(picture.left + (picture.Width() - S(36)) / 2,
			picture.top + (picture.Height() - S(36)) / 2);
		DrawPlay(this, play, kTextDim);
	}
	if (hover) {
		// a play button in the middle, under the mouse only
		float size = std::min(picture.Width(), picture.Height()) * 0.22f;
		BRect circle(0, 0, size, size);
		circle.OffsetTo(picture.left + (picture.Width() - size) / 2,
			picture.top + (picture.Height() - size) / 2);
		FillRound(this, circle, size / 2, (rgb_color){ 0, 0, 0, 150 });
		DrawPlay(this, circle.InsetByCopy(size * 0.32f, size * 0.3f), kText);
	}
	if (selected)
		StrokeRound(this, picture.InsetByCopy(-S(3), -S(3)),
			S(kBaseRadius) + S(2), S(2), kAccent);

	if (video.resume > 0 && !video.watched && video.progress < 0) {
		// how far it was watched, red as on the video sites
		BRect bar(picture.left, picture.bottom - S(3), picture.right,
			picture.bottom);
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 160);
		FillRect(bar);
		bar.right = bar.left + bar.Width() * std::min<int32>(video.resume,
			1000) / 1000;
		SetDrawingMode(B_OP_COPY);
		SetHighColor(229, 57, 53);
		FillRect(bar);
	}
	if (video.watched) {
		// a check in the corner: seen already
		BRect mark(picture.right - S(26), picture.top + S(6),
			picture.right - S(6), picture.top + S(26));
		FillRound(this, mark, S(10), (rgb_color){ 0, 0, 0, 190 });
		SetDrawingMode(B_OP_COPY);
		SetHighColor(kText);
		SetPenSize(S(2));
		StrokeLine(BPoint(mark.left + S(5), mark.top + S(10)),
			BPoint(mark.left + S(9), mark.top + S(14)));
		StrokeLine(BPoint(mark.left + S(9), mark.top + S(14)),
			BPoint(mark.left + S(15), mark.top + S(6)));
		SetPenSize(1);
	}

	if (video.favorite) {
		// a star in the corner: among the favorites
		BRect mark(picture.left + S(6), picture.top + S(6),
			picture.left + S(28), picture.top + S(28));
		FillRound(this, mark, S(11), (rgb_color){ 0, 0, 0, 190 });
		BPoint center(mark.left + mark.Width() / 2,
			mark.top + mark.Height() / 2 + S(1));
		BPoint points[10];
		for (int32 i = 0; i < 10; i++) {
			float radius = i % 2 == 0 ? S(8.5f) : S(3.6f);
			float angle = -M_PI / 2 + i * M_PI / 5;
			points[i] = center + BPoint(radius * cosf(angle),
				radius * sinf(angle));
		}
		SetDrawingMode(B_OP_OVER);
		SetHighColor(255, 196, 0);
		FillPolygon(points, 10);
	}

	if (video.progress >= 0) {
		// the download, as a bar along the bottom of the picture
		BRect bar(picture.left, picture.bottom - S(3), picture.right,
			picture.bottom);
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 160);
		FillRect(bar);
		bar.right = bar.left + bar.Width() * std::min<int32>(video.progress,
			100) / 100;
		SetDrawingMode(B_OP_COPY);
		SetHighColor(kAccent);
		FillRect(bar);
	}
	if (video.downloaded && video.path.IsEmpty()) {
		// a mark in the other corner: it can be watched offline
		BRect mark(picture.left + S(6), picture.bottom - S(24),
			picture.left + S(24), picture.bottom - S(6));
		FillRound(this, mark, S(3), (rgb_color){ 0, 0, 0, 200 });
		SetDrawingMode(B_OP_COPY);
		SetHighColor(kText);
		SetPenSize(S(2));
		BPoint center(mark.left + mark.Width() / 2, mark.top + S(4));
		StrokeLine(center, center + BPoint(0, S(8)));
		StrokeLine(center + BPoint(-S(4), S(4)), center + BPoint(0, S(8)));
		StrokeLine(center + BPoint(S(4), S(4)), center + BPoint(0, S(8)));
		StrokeLine(BPoint(mark.left + S(4), mark.bottom - S(3)),
			BPoint(mark.right - S(4), mark.bottom - S(3)));
		SetPenSize(1);
	}

	// the length, in a corner of the picture, when it is known
	if (video.duration > 0) {
		BFont small = Font(12, true);
		BString length = duration_text(video.duration);
		float width = small.StringWidth(length.String()) + S(10);
		BRect badge(picture.right - S(6) - width, picture.bottom - S(24),
			picture.right - S(6), picture.bottom - S(6));
		FillRound(this, badge, S(3), (rgb_color){ 0, 0, 0, 200 });
		DrawCentered(this, length.String(), badge, small, kText);
	}

	// the title, on two lines at most
	BFont font = Font(kTitleSize);
	font_height height;
	font.GetHeight(&height);
	float lineHeight = line_height(font);
	BString rest(video.title);
	float y = picture.bottom + S(8) + height.ascent;
	for (int line = 0; line < 2 && !rest.IsEmpty(); line++) {
		BString text(rest);
		// break after the last word that fits, or cut the line short
		int32 cut = text.Length();
		while (cut > 0 && font.StringWidth(text.String(), cut)
				> frame.Width())
			cut = text.FindLast(' ', cut - 1);
		if (line == 1 || cut <= 0 || cut == text.Length()) {
			font.TruncateString(&text, B_TRUNCATE_END, frame.Width());
			rest = "";
		} else {
			text.Truncate(cut);
			rest.Remove(0, cut);
			rest.Trim();
		}
		DrawText(this, text.String(), BPoint(frame.left, y), font, kText);
		y += lineHeight;
	}

	// a download on its way: how far it is, in place of the channel
	if (!video.status.IsEmpty() && video.progress >= 0) {
		BFont small = Font(kChannelSize);
		font_height smallHeight;
		small.GetHeight(&smallHeight);
		BString status(video.status);
		small.TruncateString(&status, B_TRUNCATE_END, frame.Width());
		float baseline = frame.bottom - S(kBaseTitleHeight) + S(8)
			+ 2 * line_height(Font(kTitleSize)) + S(4) + smallHeight.ascent;
		DrawText(this, status.String(), BPoint(frame.left, baseline), small,
			kAccent);
		return;
	}

	// the channel, below: a link to its latest videos
	BRect channel = _ChannelFrame(index);
	if (channel.IsValid()) {
		BFont small = Font(kChannelSize);
		font_height smallHeight;
		small.GetHeight(&smallHeight);
		BString name(video.channel);
		small.TruncateString(&name, B_TRUNCATE_END, frame.Width());
		bool link = index == fHoverChannel;
		float baseline = channel.top + smallHeight.ascent;
		DrawText(this, name.String(), BPoint(channel.left, baseline), small,
			link ? kText : kTextDim);
		if (link) {
			StrokeLine(BPoint(channel.left, baseline + S(2)),
				BPoint(channel.right, baseline + S(2)));
		}
	}

	// the views and how long ago, below, as far as the source tells them
	BString info = views_and_age(video.views, video.date);
	if (!info.IsEmpty()) {
		BFont small = Font(kChannelSize);
		font_height smallHeight;
		small.GetHeight(&smallHeight);
		small.TruncateString(&info, B_TRUNCATE_END, frame.Width());
		float baseline = frame.bottom - S(kBaseTitleHeight) + S(8)
			+ 2 * line_height(Font(kTitleSize)) + S(4) + line_height(small)
			+ S(2) + smallHeight.ascent;
		DrawText(this, info.String(), BPoint(frame.left, baseline), small,
			kTextDim);
	}
}


void
VideoGrid::_DrawBusy()
{
	BRect bounds = Bounds();
	SetDrawingMode(B_OP_ALPHA);
	SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	SetHighColor(0, 0, 0, 200);
	FillRect(bounds);

	float centerY = bounds.top + bounds.Height() / 2;
	BString dots;
	for (int i = 0; i < fDots % 4; i++)
		dots << ".";
	BString text(B_TRANSLATE("Getting the video ready"));
	text << dots;
	DrawCentered(this, text.String(), BRect(bounds.left, centerY - S(60),
		bounds.right, centerY - S(20)), Font(22, true), kText);
	BString title(fBusy);
	BFont font = Font(16);
	font.TruncateString(&title, B_TRUNCATE_END, bounds.Width() - S(80));
	DrawCentered(this, title.String(), BRect(bounds.left, centerY,
		bounds.right, centerY + S(30)), font, kTextDim);
}


void
VideoGrid::_Select(int32 index)
{
	int32 count = _VisibleCount();
	if (count <= 0)
		return;
	index = std::max<int32>(0, std::min<int32>(index, count - 1));
	if (index == fSelected)
		return;
	Invalidate(_TileFrame(fSelected).InsetByCopy(-S(6), -S(6)));
	fSelected = index;
	BRect frame = _TileFrame(fSelected).InsetByCopy(-S(6), -S(6));
	Invalidate(frame);

	BMessage selected(kMsgSelected);
	selected.AddString("id", fVideos[fSelected].id);
	Window()->PostMessage(&selected);

	if (!fShelf) {
		_UpdateScrollBar();
		BRect bounds = Bounds();
		if (frame.top < bounds.top)
			ScrollTo(0, std::max(0.0f, frame.top - S(kBaseGap)));
		else if (frame.bottom > bounds.bottom) {
			ScrollTo(0, std::max(0.0f,
				frame.bottom + S(kBaseGap) - bounds.Height()));
		}
	}
}


void
VideoGrid::_Play(int32 index)
{
	if (index < 0 || index >= (int32)fVideos.size() || !fBusy.IsEmpty())
		return;
	BMessage play(kMsgPlay);
	play.AddString("id", fVideos[index].id);
	play.AddString("title", fVideos[index].title);
	play.AddString("provider", fVideos[index].provider);
	play.AddString("kind", fVideos[index].kind);
	play.AddString("channel", fVideos[index].channel);
	play.AddString("channel id", fVideos[index].channelId);
	play.AddInt32("duration", fVideos[index].duration);
	if (!fVideos[index].path.IsEmpty())
		play.AddString("path", fVideos[index].path);
	Window()->PostMessage(&play);
}


void
VideoGrid::_Download(int32 index)
{
	if (index < 0 || index >= (int32)fVideos.size())
		return;
	const MediaItem& video = fVideos[index];
	if (video.downloaded || video.progress >= 0 || !video.path.IsEmpty()
		|| video.kind == "live")
		return;
	BMessage download = _DownloadMessage(index);
	Window()->PostMessage(&download);
}


void
VideoGrid::_ToggleFavorite(int32 index)
{
	if (index < 0 || index >= (int32)fVideos.size())
		return;
	BMessage favorite = _DetailsMessage(index);
	favorite.what = kMsgToggleFavorite;
	Window()->PostMessage(&favorite);
}


void
VideoGrid::_ShowDetails(int32 index)
{
	if (index < 0 || index >= (int32)fVideos.size())
		return;
	BMessage details = _DetailsMessage(index);
	Window()->PostMessage(&details);
}


BMessage
VideoGrid::_DetailsMessage(int32 index) const
{
	const MediaItem& video = fVideos[index];
	BMessage details(kMsgShowDetails);
	details.AddString("provider", video.provider);
	details.AddString("kind", video.kind);
	details.AddString("id", video.id);
	details.AddString("title", video.title);
	details.AddString("description", video.description);
	details.AddString("channel", video.channel);
	details.AddString("channel id", video.channelId);
	details.AddInt32("duration", video.duration);
	details.AddInt32("year", video.year);
	details.AddString("date", video.date);
	details.AddInt64("views", video.views);
	details.AddDouble("rating", video.rating);
	details.AddString("poster", video.poster);
	details.AddString("thumbnail", video.thumbnail);
	details.AddString("path", video.path);
	details.AddBool("downloaded", video.downloaded);
	details.AddInt32("progress", video.progress);
	return details;
}


BMessage
VideoGrid::_DownloadMessage(int32 index) const
{
	const MediaItem& video = fVideos[index];
	BMessage download(kMsgDownload);
	download.AddString("provider", video.provider);
	download.AddString("kind", video.kind);
	download.AddString("id", video.id);
	download.AddString("title", video.title);
	download.AddString("channel", video.channel);
	download.AddString("channel id", video.channelId);
	download.AddInt32("duration", video.duration);
	return download;
}


void
VideoGrid::_ShowMenu(int32 index, BPoint where)
{
	const MediaItem& video = fVideos[index];
	BPopUpMenu* menu = new BPopUpMenu("video", false, false);
	BMessage* details = new BMessage(_DetailsMessage(index));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Details"), details));
	menu->AddSeparatorItem();
	BMessage* play = new BMessage(kMsgPlay);
	play->AddString("id", video.id);
	play->AddString("title", video.title);
	play->AddString("provider", video.provider);
	play->AddString("kind", video.kind);
	play->AddString("channel", video.channel);
	play->AddString("channel id", video.channelId);
	play->AddInt32("duration", video.duration);
	if (!video.path.IsEmpty())
		play->AddString("path", video.path);
	menu->AddItem(new BMenuItem(B_TRANSLATE("Play"), play));
	BMessage* favorite = new BMessage(_DetailsMessage(index));
	favorite->what = kMsgToggleFavorite;
	menu->AddItem(new BMenuItem(video.favorite
		? B_TRANSLATE("Remove from favorites")
		: B_TRANSLATE("Add to favorites"), favorite));

	// live television cannot be downloaded
	if (video.path.IsEmpty() && video.kind != "live") {
		BMessage* download = new BMessage(_DownloadMessage(index));
		const char* label = B_TRANSLATE("Download");
		if (video.downloaded)
			label = B_TRANSLATE("Already downloaded");
		else if (video.progress >= 0)
			label = B_TRANSLATE("Downloading" B_UTF8_ELLIPSIS);
		BMenuItem* item = new BMenuItem(label, download);
		item->SetEnabled(!video.downloaded && video.progress < 0);
		menu->AddItem(item);
		if (video.progress >= 0) {
			BMessage* stop = new BMessage(kMsgCancelDownload);
			stop->AddString("id", video.id);
			menu->AddItem(new BMenuItem(B_TRANSLATE("Stop the download"),
				stop));
		}
	} else if (!video.path.IsEmpty()) {
		BMessage* trash = new BMessage(kMsgTrash);
		trash->AddString("id", video.id);
		trash->AddString("path", video.path);
		trash->AddString("title", video.title);
		menu->AddItem(new BMenuItem(B_TRANSLATE("Move to Trash"), trash));
	}
	menu->SetTargetForItems(Window());
	ScaleMenu(menu);
	menu->Go(ConvertToScreen(where), true, false, true);
}


void
VideoGrid::KeyDown(const char* bytes, int32 count)
{
	int32 columns = _Columns();
	if (fShelf && (bytes[0] == B_UP_ARROW || bytes[0] == B_DOWN_ARROW)) {
		if (_VisibleCount() <= 0)
			return;
		BMessage move(kMsgShelfMove);
		move.AddString("id", fVideos[fSelected].id);
		move.AddInt32("delta", bytes[0] == B_UP_ARROW ? -1 : 1);
		Window()->PostMessage(&move);
		return;
	}
	switch (bytes[0]) {
		case B_LEFT_ARROW:
			_Select(fSelected - 1);
			break;
		case B_RIGHT_ARROW:
			_Select(fSelected + 1);
			break;
		case B_UP_ARROW:
			_Select(fSelected - columns);
			break;
		case B_DOWN_ARROW:
			_Select(fSelected + columns);
			break;
		case B_ENTER:
		case B_SPACE:
			_Play(fSelected);
			break;
		case 'd':
		case 'D':
			_Download(fSelected);
			break;
		case 'i':
		case 'I':
			_ShowDetails(fSelected);
			break;
		case 's':
		case 'S':
		case '*':
			_ToggleFavorite(fSelected);
			break;
		case 'f':
		case 'F':
			Window()->PostMessage(kMsgFullScreen);
			break;
		default:
			BView::KeyDown(bytes, count);
	}
}


void
VideoGrid::MouseDown(BPoint where)
{
	MakeFocus(true);
	int32 channel = _ChannelAt(where);
	if (channel >= 0) {
		BMessage show(kMsgChannel);
		show.AddString("channel", fVideos[channel].channel);
		show.AddString("channel id", fVideos[channel].channelId);

		int32 buttons = B_PRIMARY_MOUSE_BUTTON;
		Window()->CurrentMessage()->FindInt32("buttons", &buttons);
		if ((buttons & B_SECONDARY_MOUSE_BUTTON) == 0) {
			Window()->PostMessage(&show);
			return;
		}
		BMessage* add = new BMessage(show);
		add->what = kMsgAddChannel;
		BPopUpMenu* menu = new BPopUpMenu("channel", false, false);
		menu->AddItem(new BMenuItem(B_TRANSLATE("Show the latest videos"),
			new BMessage(show)));
		menu->AddItem(new BMenuItem(B_TRANSLATE("Add to the tabs"), add));
		menu->SetTargetForItems(Window());
		ScaleMenu(menu);
		menu->Go(ConvertToScreen(where), true, false, true);
		return;
	}
	int32 index = _TileAt(where);
	if (index < 0)
		return;
	_Select(index);
	int32 buttons = B_PRIMARY_MOUSE_BUTTON;
	Window()->CurrentMessage()->FindInt32("buttons", &buttons);
	if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0)
		_ShowMenu(index, where);
	else
		_Play(index);
}


void
VideoGrid::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	int32 channel = transit == B_EXITED_VIEW ? -1 : _ChannelAt(where);
	if (channel != fHoverChannel) {
		if (fHoverChannel >= 0)
			Invalidate(_ChannelFrame(fHoverChannel).InsetByCopy(-1, -S(4)));
		fHoverChannel = channel;
		if (fHoverChannel >= 0)
			Invalidate(_ChannelFrame(fHoverChannel).InsetByCopy(-1, -S(4)));
		BCursor cursor(channel >= 0
			? B_CURSOR_ID_FOLLOW_LINK : B_CURSOR_ID_SYSTEM_DEFAULT);
		SetViewCursor(&cursor);
	}

	int32 index = transit == B_EXITED_VIEW ? -1 : _TileAt(where);
	if (index == fHover)
		return;
	if (fHover >= 0)
		Invalidate(_TileFrame(fHover));
	fHover = index;
	if (fHover >= 0)
		Invalidate(_TileFrame(fHover));
}


void
VideoGrid::MessageReceived(BMessage* message)
{
	if (message->what == kMsgAnimate) {
		fDots++;
		Invalidate();
		return;
	}
	BView::MessageReceived(message);
}
