/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "DetailView.h"

#include <ctype.h>

#include <algorithm>
#include <stdio.h>
#include <math.h>
#include <cmath>

#include <Bitmap.h>
#include <Catalog.h>
#include <Cursor.h>
#include <Region.h>
#include <Url.h>

#include "Format.h"
#include "Messages.h"
#include "Provider.h"
#include "Style.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Detail"


using namespace Style;


static float
line_height(const BFont& font)
{
	font_height height;
	font.GetHeight(&height);
	return ceilf(height.ascent + height.descent + height.leading);
}


static void
wrap_paragraph(const BString& text, const BFont& font, float width,
	std::vector<BString>& lines)
{
	BString rest(text);
	rest.Trim();
	if (rest.IsEmpty()) {
		lines.push_back("");
		return;
	}
	while (!rest.IsEmpty()) {
		if (font.StringWidth(rest.String()) <= width) {
			lines.push_back(rest);
			break;
		}
		int32 length = rest.Length();
		int32 cut = 0;
		for (int32 i = 1; i <= length; i++) {
			if (font.StringWidth(rest.String(), i) > width)
				break;
			if (rest.ByteAt(i - 1) == ' ')
				cut = i - 1;
		}
		if (cut <= 0) {
			// one word longer than the line: cut it
			int32 i = 1;
			for (; i <= length; i++)
				if (font.StringWidth(rest.String(), i) > width)
					break;
			cut = std::max<int32>(1, i - 1);
		}
		BString line(rest.String(), cut);
		line.Trim();
		lines.push_back(line);
		rest.Remove(0, cut);
		rest.Trim();
	}
}


static void
wrap_lines(const BString& text, const BFont& font, float width,
	std::vector<BString>& lines)
{
	lines.clear();
	int32 start = 0;
	int32 length = text.Length();
	while (start <= length) {
		int32 end = start;
		while (end < length && text.ByteAt(end) != '\n')
			end++;
		BString paragraph(text.String() + start, end - start);
		wrap_paragraph(paragraph, font, width, lines);
		if (end >= length)
			break;
		start = end + 1;
	}
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


DetailView::DetailView(BMessenger target)
	:
	BView("details", B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE),
	fTarget(target),
	fPicture(NULL),
	fScroll(0),
	fLineHeight(0),
	fHover(0),
	fLoading(false)
{
	SetViewColor(kBackground);
}


DetailView::~DetailView()
{
	delete fPicture;
}


void
DetailView::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Update();
}


void
DetailView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_Update();
	Invalidate();
}


void
DetailView::SetItem(const MediaItem& item)
{
	bool same = fItem.id == item.id;
	// another channel: whether it is followed, the window says
	if (fItem.channelId != item.channelId)
		fFollowed = -1;
	if (!same) {
		delete fPicture;
		fPicture = NULL;
		fScroll = 0;
	}
	fItem = item;
	_Update();
	Invalidate();
}


void
DetailView::SetPicture(BBitmap* picture)
{
	if (picture == fPicture)
		return;
	delete fPicture;
	fPicture = picture;
	Invalidate(fPosterFrame);
}


void
DetailView::SetProgress(const char* id, int32 progress)
{
	if (fItem.id != id || fItem.progress == progress)
		return;
	fItem.progress = progress;
	Invalidate(fDownloadFrame);
}


void
DetailView::SetDownloaded(const char* id, bool downloaded)
{
	if (fItem.id != id)
		return;
	fItem.downloaded = downloaded;
	fItem.progress = -1;
	if (!downloaded)
		fItem.path = "";
	// the button of the Trash comes or goes
	_Update();
	Invalidate();
}


void
DetailView::SetFavorite(const char* id, bool favorite)
{
	if (fItem.id != id || fItem.favorite == favorite)
		return;
	fItem.favorite = favorite;
	Invalidate(fStarFrame);
}


void
DetailView::SetLoading(bool loading)
{
	if (fLoading == loading)
		return;
	fLoading = loading;
	Invalidate(fDescFrame);
}


void
DetailView::_Update()
{
	BRect bounds = Bounds();
	fTitleLines.clear();
	fLines.clear();
	fLinks.clear();
	fMeta = _Meta();
	fPosterFrame = BRect();
	fTextFrame = BRect();
	fDescFrame = BRect();
	fMetaFrame = BRect();
	fPlayFrame = BRect();
	fDownloadFrame = BRect();
	fBackFrame = BRect();
	fStarFrame = BRect();
	fTrashFrame = BRect();
	fChannelFrame = BRect();
	fFollowFrame = BRect();

	if (bounds.Width() < S(80) || bounds.Height() < S(80))
		return;

	const float pad = S(36);
	const float gap = S(32);
	BFont titleFont = Font(28, true);
	BFont channelFont = Font(16);
	BFont descFont = Font(14);
	BFont metaFont = Font(13);
	fLineHeight = line_height(descFont);

	fBackFrame = BRect(pad, pad, pad + S(88), pad + S(30));
	// the star, next to the way back
	fStarFrame = BRect(fBackFrame.right + S(10), fBackFrame.top,
		fBackFrame.right + S(10) + fBackFrame.Height(), fBackFrame.bottom);
	float top = fBackFrame.bottom + S(20);
	float bottom = bounds.bottom - pad;
	if (bottom - top < S(100))
		return;

	fTextFrame = BRect(pad, top, bounds.right - pad, bottom);
	bool stacked = bounds.Width() < S(780);
	if (stacked) {
		float width = bounds.Width() - 2 * pad;
		float maxHeight = (bottom - top) * 0.42f;
		if (width * 9.0f / 16.0f > maxHeight)
			width = maxHeight * 16.0f / 9.0f;
		fPosterFrame = BRect(pad, top, pad + width,
			top + width * 9.0f / 16.0f);
		fTextFrame = BRect(pad, fPosterFrame.bottom + S(16),
			bounds.right - pad, bottom);
	} else {
		float width = (bounds.Width() - 2 * pad - gap) * 0.46f;
		width = std::max(S(220), std::min(width, S(620)));
		if (width * 9.0f / 16.0f > bottom - top)
			width = (bottom - top) * 16.0f / 9.0f;
		fPosterFrame = BRect(pad, top, pad + width,
			top + width * 9.0f / 16.0f);
		fTextFrame = BRect(fPosterFrame.right + gap, top,
			bounds.right - pad, bottom);
	}

	float width = fTextFrame.Width();
	if (width < S(80))
		return;

	if (!fItem.title.IsEmpty())
		wrap_lines(fItem.title, titleFont, width, fTitleLines);
	float titleHeight = fTitleLines.size() * line_height(titleFont);
	float y = fTextFrame.top;
	if (!fItem.channel.IsEmpty()) {
		float channelWidth = std::min(width,
			channelFont.StringWidth(fItem.channel.String()));
		// with the button to follow it at its right, when there is room
		float followWidth = 0;
		if (!fItem.channelId.IsEmpty() && fFollowed >= 0) {
			BFont buttonFont = Font(14, true);
			followWidth = std::max(buttonFont.StringWidth(
				B_TRANSLATE("Follow")), buttonFont.StringWidth(
				B_TRANSLATE("Following"))) + S(28);
			if (followWidth + S(12) > width / 2)
				followWidth = 0;
		}
		channelWidth = std::min(channelWidth,
			width - (followWidth > 0 ? followWidth + S(12) : 0));
		float lineTop = y + titleHeight + S(6);
		float rowHeight = std::max(line_height(channelFont),
			followWidth > 0 ? S(30) : 0.0f);
		float textTop = lineTop + (rowHeight - line_height(channelFont)) / 2;
		fChannelFrame = BRect(fTextFrame.left, textTop,
			fTextFrame.left + channelWidth,
			textTop + line_height(channelFont));
		if (followWidth > 0) {
			fFollowFrame = BRect(fChannelFrame.right + S(12), lineTop,
				fChannelFrame.right + S(12) + followWidth,
				lineTop + rowHeight);
		}
	}
	float metaTop = fChannelFrame.IsValid()
		? std::max(fChannelFrame.bottom, fFollowFrame.IsValid()
			? fFollowFrame.bottom : 0.0f) + S(2)
		: y + titleHeight + S(6);
	if (!fMeta.IsEmpty()) {
		fMetaFrame = BRect(fTextFrame.left, metaTop,
			fTextFrame.right, metaTop + line_height(metaFont));
	}

	float descTop = (fMetaFrame.IsValid() ? fMetaFrame.bottom : metaTop)
		+ S(18);
	float descBottom = fTextFrame.bottom - S(40) - S(16);
	if (descBottom > descTop) {
		fDescFrame = BRect(fTextFrame.left, descTop,
			fTextFrame.right - S(14), descBottom);
		if (!fItem.description.IsEmpty()) {
			wrap_lines(fItem.description, descFont, fDescFrame.Width(),
				fLines);
			for (int32 i = 0; i < (int32)fLines.size(); i++)
				_FindLinks(i);
		}
	}

	float buttonTop = fTextFrame.bottom - S(40);
	if (buttonTop > fTextFrame.top) {
		float playWidth = S(140);
		float downloadWidth = S(190);
		bool trash = _CanTrash();
		float trashWidth = trash ? Font(14, true).StringWidth(
			B_TRANSLATE("Move to Trash")) + S(36) : 0;
		float available = fTextFrame.Width();
		int32 count = trash ? 3 : 2;
		if (playWidth + downloadWidth + trashWidth + S(12) * (count - 1)
				> available) {
			float share = std::max(S(80),
				(available - S(12) * (count - 1)) / count);
			playWidth = downloadWidth = share;
			if (trash)
				trashWidth = share;
		}
		fPlayFrame = BRect(fTextFrame.left, buttonTop,
			fTextFrame.left + playWidth, fTextFrame.bottom);
		fDownloadFrame = BRect(fPlayFrame.right + S(12), buttonTop,
			std::min(fTextFrame.right,
				fPlayFrame.right + S(12) + downloadWidth),
			fTextFrame.bottom);
		if (trash) {
			fTrashFrame = BRect(fDownloadFrame.right + S(12), buttonTop,
				std::min(fTextFrame.right,
					fDownloadFrame.right + S(12) + trashWidth),
				fTextFrame.bottom);
		}
		// live television cannot be downloaded
		if (fItem.kind == "live")
			fDownloadFrame = BRect();
	}

	_ClampScroll();
}


void
DetailView::_ClampScroll()
{
	float total = fLines.size() * fLineHeight;
	float visible = fDescFrame.IsValid() ? fDescFrame.Height() : 0;
	float most = std::max(0.0f, total - visible);
	fScroll = std::max(0.0f, std::min(fScroll, most));
}


void
DetailView::_Scroll(float delta)
{
	fScroll += delta;
	_ClampScroll();
	if (fDescFrame.IsValid())
		Invalidate(fDescFrame.InsetByCopy(0, -1));
}


BString
DetailView::_Meta() const
{
	BString text = fItem.date;
	BString age = age_text(fItem.date);
	if (!age.IsEmpty())
		text << " (" << age << ")";
	if (text.IsEmpty() && fItem.year > 0)
		text.SetToFormat("%" B_PRId32, fItem.year);
	if (fItem.duration > 0) {
		if (!text.IsEmpty())
			text << "  ·  ";
		text << duration_text(fItem.duration);
	}
	if (fItem.views > 0) {
		if (!text.IsEmpty())
			text << "  ·  ";
		text << views_text(fItem.views);
	}
	return text;
}


BString
DetailView::_DownloadLabel() const
{
	if (fItem.downloaded || !fItem.path.IsEmpty())
		return B_TRANSLATE("Downloaded");
	if (fItem.progress >= 0) {
		BString label(B_TRANSLATE("Downloading" B_UTF8_ELLIPSIS));
		BString percent;
		percent.SetToFormat(" %" B_PRId32 "%%", fItem.progress);
		label << percent;
		return label;
	}
	return B_TRANSLATE("Download");
}


bool
DetailView::_CanTrash() const
{
	return (fItem.downloaded || !fItem.path.IsEmpty()) && fItem.kind != "live";
}


bool
DetailView::_CanDownload() const
{
	return !fItem.downloaded && fItem.path.IsEmpty() && fItem.progress < 0
		&& fItem.kind != "live";
}


void
DetailView::_FindLinks(int32 lineIndex)
{
	const BString& line = fLines[lineIndex];
	static const char* kPrefixes[] = { "http://", "https://", "www." };
	int32 pos = 0;
	while (pos < line.Length()) {
		int32 start = -1;
		for (const char* prefix : kPrefixes) {
			int32 found = line.IFindFirst(prefix, pos);
			if (found >= 0 && (start < 0 || found < start))
				start = found;
		}
		if (start < 0)
			break;
		if (start > 0) {
			char previous = line.ByteAt(start - 1);
			if (isalnum((unsigned char)previous) || previous == '_'
				|| previous == '-') {
				pos = start + 1;
				continue;
			}
		}
		int32 end = start;
		while (end < line.Length()
			&& !isspace((unsigned char)line.ByteAt(end)))
			end++;
		while (end > start) {
			char last = line.ByteAt(end - 1);
			if (last == '.' || last == ',' || last == ';' || last == ':'
				|| last == '!' || last == '?' || last == ')'
				|| last == ']' || last == '}' || last == '\''
				|| last == '"')
				end--;
			else
				break;
		}
		if (end <= start + 4) {
			pos = start + 1;
			continue;
		}
		BString url;
		line.CopyInto(url, start, end - start);
		BString open(url);
		if (url.IStartsWith("www."))
			open.Prepend("https://");
		Link link;
		link.line = lineIndex;
		link.start = start;
		link.end = end;
		link.url = open;
		fLinks.push_back(link);
		pos = end;
	}
}


BString
DetailView::_LinkAt(BPoint where) const
{
	if (!fDescFrame.Contains(where))
		return BString();
	float lineTop = fDescFrame.top - fScroll;
	int32 lineIndex = (int32)floorf((where.y - lineTop) / fLineHeight);
	if (lineIndex < 0 || lineIndex >= (int32)fLines.size())
		return BString();
	BFont font = Font(14);
	const BString& line = fLines[lineIndex];
	for (const Link& link : fLinks) {
		if (link.line != lineIndex)
			continue;
		float left = fDescFrame.left
			+ font.StringWidth(line.String(), link.start);
		float right = fDescFrame.left
			+ font.StringWidth(line.String(), link.end);
		if (where.x >= left - 1 && where.x <= right + 1)
			return link.url;
	}
	return BString();
}


void
DetailView::_OpenLink(const BString& url)
{
	BString id = YouTubeVideoId(url.String());
	if (!id.IsEmpty()) {
		BMessage details(kMsgShowDetails);
		details.AddString("provider", "youtube");
		details.AddString("kind", "video");
		details.AddString("id", id);
		fTarget.SendMessage(&details);
		return;
	}
	BUrl link(url.String(), false);
	if (link.IsValid())
		link.OpenWithPreferredApplication();
}


void
DetailView::_DrawDescriptionLine(int32 index, BPoint at, const BFont& font)
{
	const BString& line = fLines[index];
	int32 pos = 0;
	for (const Link& link : fLinks) {
		if (link.line != index)
			continue;
		if (link.start > pos) {
			BString text(line.String() + pos, link.start - pos);
			DrawText(this, text.String(),
				BPoint(at.x + font.StringWidth(line.String(), pos), at.y),
				font, kText);
		}
		BString text;
		line.CopyInto(text, link.start, link.end - link.start);
		float left = at.x + font.StringWidth(line.String(), link.start);
		float right = at.x + font.StringWidth(line.String(), link.end);
		DrawText(this, text.String(), BPoint(left, at.y), font, kAccent);
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(kAccent);
		StrokeLine(BPoint(left, at.y + S(2)), BPoint(right, at.y + S(2)));
		pos = link.end;
	}
	if (pos < line.Length()) {
		BString text(line.String() + pos, line.Length() - pos);
		DrawText(this, text.String(),
			BPoint(at.x + font.StringWidth(line.String(), pos), at.y),
			font, kText);
	}
}


void
DetailView::_DrawPoster()
{
	FillRound(this, fPosterFrame, S(8), kCard);
	if (fPicture != NULL && fPicture->IsValid()
		&& fPicture->Bounds().Height() > 0) {
		BRect source = fPicture->Bounds();
		float target = fPosterFrame.Width() / fPosterFrame.Height();
		float current = source.Width() / source.Height();
		if (current > target) {
			float width = source.Height() * target;
			source.InsetBy((source.Width() - width) / 2, 0);
		} else if (current < target) {
			float height = source.Width() / target;
			source.InsetBy(0, (source.Height() - height) / 2);
		}
		SetDrawingMode(B_OP_COPY);
		DrawBitmap(fPicture, source, fPosterFrame,
			B_FILTER_BITMAP_BILINEAR);
	} else {
		float size = std::min(fPosterFrame.Width(),
			fPosterFrame.Height()) * 0.18f;
		BRect play(0, 0, size, size);
		play.OffsetTo(fPosterFrame.left + (fPosterFrame.Width() - size) / 2,
			fPosterFrame.top + (fPosterFrame.Height() - size) / 2);
		DrawPlay(this, play, kTextDim);
	}
	StrokeRound(this, fPosterFrame, S(8), 1, (rgb_color){ 70, 70, 76, 255 });
}


void
DetailView::_DrawButton(BRect frame, const char* label, bool primary,
	bool enabled, bool hover)
{
	rgb_color fill = primary ? kAccent : kCard;
	if (enabled && hover) {
		fill.red = std::min<int>(255, fill.red + 18);
		fill.green = std::min<int>(255, fill.green + 18);
		fill.blue = std::min<int>(255, fill.blue + 18);
	}
	if (!enabled)
		fill = (rgb_color){ 46, 46, 50, 255 };
	FillRound(this, frame, S(8), fill);
	if (!primary) {
		rgb_color line = enabled ? kTextDim
			: (rgb_color){ 70, 70, 76, 255 };
		StrokeRound(this, frame, S(8), 1, line);
	}
	DrawCentered(this, label, frame, Font(14, true),
		enabled ? kText : kTextDim);
}


void
DetailView::_DrawPlayButton()
{
	BString label(B_TRANSLATE("Play"));
	BFont font = Font(14, true);
	float textWidth = font.StringWidth(label.String());
	float icon = S(12);
	float gap = S(8);
	float total = icon + gap + textWidth;
	float left = fPlayFrame.left + (fPlayFrame.Width() - total) / 2;
	font_height height;
	font.GetHeight(&height);
	float baseline = fPlayFrame.top
		+ (fPlayFrame.Height() + height.ascent - height.descent) / 2;

	rgb_color fill = kAccent;
	if (fHover == 2) {
		fill.red = std::min<int>(255, fill.red + 18);
		fill.green = std::min<int>(255, fill.green + 18);
		fill.blue = std::min<int>(255, fill.blue + 18);
	}
	FillRound(this, fPlayFrame, S(8), fill);
	BRect iconFrame(left, baseline - height.ascent + S(2), left + icon,
		baseline + height.descent - S(2));
	DrawPlay(this, iconFrame, kText);
	DrawText(this, label.String(), BPoint(left + icon + gap, baseline),
		font, kText);
}


void
DetailView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(kBackground);
	FillRect(updateRect);

	_DrawPoster();

	// the way back, above the picture
	FillRound(this, fBackFrame, S(8), fHover == 1 ? kCard : kBar);
	StrokeRound(this, fBackFrame, S(8), 1, kTextDim);
	BFont backFont = Font(13);
	BString backLabel(B_TRANSLATE("Back"));
	float backText = backFont.StringWidth(backLabel.String());
	float arrow = S(9);
	float gap = S(8);
	float total = arrow + gap + backText;
	float left = fBackFrame.left + (fBackFrame.Width() - total) / 2;
	font_height backHeight;
	backFont.GetHeight(&backHeight);
	float baseline = fBackFrame.top
		+ (fBackFrame.Height() + backHeight.ascent - backHeight.descent)
			/ 2;
	SetDrawingMode(B_OP_ALPHA);
	SetHighColor(kText);
	FillTriangle(BPoint(left + arrow, baseline - backHeight.ascent + S(2)),
		BPoint(left + arrow, baseline + backHeight.descent - S(2)),
		BPoint(left, baseline));
	DrawText(this, backLabel.String(), BPoint(left + arrow + gap, baseline),
		backFont, kText);
	_DrawStar();

	// the title
	BFont titleFont = Font(28, true);
	font_height titleHeight;
	titleFont.GetHeight(&titleHeight);
	float lineY = fTextFrame.top + titleHeight.ascent;
	float titleLine = line_height(titleFont);
	for (const BString& line : fTitleLines) {
		DrawText(this, line.String(),
			BPoint(fTextFrame.left, lineY), titleFont, kText);
		lineY += titleLine;
	}

	// the channel, a link to its other videos
	if (fChannelFrame.IsValid()) {
		BFont channelFont = Font(16);
		font_height channelHeight;
		channelFont.GetHeight(&channelHeight);
		bool linked = !fItem.channelId.IsEmpty();
		bool hover = linked && fHover == 4;
		float channelY = fChannelFrame.top + channelHeight.ascent;
		DrawText(this, fItem.channel.String(),
			BPoint(fChannelFrame.left, channelY), channelFont,
			hover ? kText : (linked ? kAccent : kTextDim));
		if (linked) {
			SetDrawingMode(B_OP_ALPHA);
			SetHighColor(hover ? kText : kAccent);
			StrokeLine(BPoint(fChannelFrame.left, channelY + S(2)),
				BPoint(fChannelFrame.right, channelY + S(2)));
		}
	}

	// when it was published, how long it is, how many watched it
	if (fMetaFrame.IsValid() && !fMeta.IsEmpty()) {
		BFont metaFont = Font(13);
		font_height metaHeight;
		metaFont.GetHeight(&metaHeight);
		DrawText(this, fMeta.String(),
			BPoint(fMetaFrame.left, fMetaFrame.top + metaHeight.ascent),
			metaFont, kTextDim);
	}

	// the description, which scrolls
	if (fDescFrame.IsValid()) {
		BFont descFont = Font(14);
		font_height descHeight;
		descFont.GetHeight(&descHeight);
		BRegion clip(fDescFrame);
		ConstrainClippingRegion(&clip);
		if (fLoading && fItem.description.IsEmpty()) {
			BString loading(B_TRANSLATE("Getting the details"
				B_UTF8_ELLIPSIS));
			DrawText(this, loading.String(),
				BPoint(fDescFrame.left, fDescFrame.top + descHeight.ascent),
				descFont, kTextDim);
		} else {
			float y = fDescFrame.top + descHeight.ascent - fScroll;
			for (int32 i = 0; i < (int32)fLines.size(); i++) {
				_DrawDescriptionLine(i, BPoint(fDescFrame.left, y),
					descFont);
				y += fLineHeight;
			}
		}
		ConstrainClippingRegion(NULL);

		float total = fLines.size() * fLineHeight;
		if (total > fDescFrame.Height()) {
			float track = fDescFrame.Height();
			float thumb = std::max(S(24), track * track / total);
			float most = total - track;
			float top = fDescFrame.top
				+ (track - thumb) * (fScroll / most);
			FillRound(this, BRect(fDescFrame.right + S(6), top,
				fDescFrame.right + S(10), top + thumb), S(2), kTextDim);
		}
	}

	// what can be done with it
	_DrawPlayButton();
	if (fDownloadFrame.IsValid()) {
		_DrawButton(fDownloadFrame, _DownloadLabel().String(), false,
			_CanDownload(), fHover == 3);
	}
	if (fFollowFrame.IsValid()) {
		_DrawButton(fFollowFrame, fFollowed > 0 ? B_TRANSLATE("Following")
			: B_TRANSLATE("Follow"), fFollowed <= 0, true, fHover == 7);
	}
	if (fTrashFrame.IsValid()) {
		_DrawButton(fTrashFrame, B_TRANSLATE("Move to Trash"), false, true,
			fHover == 6);
	}
	if (fItem.progress >= 0) {
		BRect bar(fDownloadFrame.left + S(8), fDownloadFrame.bottom - S(6),
			fDownloadFrame.right - S(8), fDownloadFrame.bottom - S(3));
		SetDrawingMode(B_OP_COPY);
		SetHighColor(30, 30, 34);
		FillRect(bar);
		bar.right = bar.left + bar.Width()
			* std::min<int32>(100, std::max<int32>(0, fItem.progress)) / 100;
		SetHighColor(kAccent);
		FillRect(bar);
	}
}


int32
DetailView::_ButtonAt(BPoint where) const
{
	if (fBackFrame.Contains(where))
		return 1;
	if (fPlayFrame.Contains(where))
		return 2;
	if (fDownloadFrame.Contains(where) && _CanDownload())
		return 3;
	if (fChannelFrame.Contains(where) && !fItem.channelId.IsEmpty())
		return 4;
	if (fStarFrame.Contains(where))
		return 5;
	if (fTrashFrame.Contains(where))
		return 6;
	if (fFollowFrame.Contains(where))
		return 7;
	return 0;
}


void
DetailView::_InvalidateButton(int32 which)
{
	switch (which) {
		case 1:
			Invalidate(fBackFrame);
			break;
		case 2:
			Invalidate(fPlayFrame);
			break;
		case 3:
			Invalidate(fDownloadFrame);
			break;
		case 4:
			Invalidate(fChannelFrame.InsetByCopy(-1, -3));
			break;
		case 5:
			Invalidate(fStarFrame);
			break;
		case 6:
			Invalidate(fTrashFrame);
			break;
		case 7:
			Invalidate(fFollowFrame);
			break;
	}
}


void
DetailView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	int32 hover = transit == B_EXITED_VIEW ? 0 : _ButtonAt(where);
	if (hover != fHover) {
		_InvalidateButton(fHover);
		fHover = hover;
		_InvalidateButton(fHover);
	}
	bool over = hover != 0
		|| (transit != B_EXITED_VIEW
			&& (fPosterFrame.Contains(where) || !_LinkAt(where).IsEmpty()));
	BCursor cursor(over ? B_CURSOR_ID_FOLLOW_LINK
		: B_CURSOR_ID_SYSTEM_DEFAULT);
	SetViewCursor(&cursor);
}


void
DetailView::MouseDown(BPoint where)
{
	MakeFocus(true);
	int32 button = _ButtonAt(where);
	if (button == 0 && fPosterFrame.Contains(where)) {
		button = 2;
	} else if (button == 0) {
		BString url = _LinkAt(where);
		if (!url.IsEmpty()) {
			_OpenLink(url);
			return;
		}
	}
	switch (button) {
		case 1:
			_Back();
			break;
		case 2:
			_Play();
			break;
		case 3:
			_Download();
			break;
		case 4:
			_Channel();
			break;
		case 5:
			_ToggleFavorite();
			break;
		case 6:
			_Trash();
			break;
		case 7:
			_ToggleFollow();
			break;
	}
}


void
DetailView::KeyDown(const char* bytes, int32 count)
{
	switch (bytes[0]) {
		case B_ESCAPE:
			_Back();
			break;
		case B_ENTER:
		case B_SPACE:
			_Play();
			break;
		case B_UP_ARROW:
			_Scroll(-fLineHeight);
			break;
		case B_DOWN_ARROW:
			_Scroll(fLineHeight);
			break;
		case B_PAGE_UP:
			_Scroll(-fDescFrame.Height() * 0.9f);
			break;
		case B_PAGE_DOWN:
			_Scroll(fDescFrame.Height() * 0.9f);
			break;
		case B_HOME:
			_Scroll(-1e6f);
			break;
		case B_END:
			_Scroll(1e6f);
			break;
		case 'd':
		case 'D':
			if (_CanDownload())
				_Download();
			break;
		case 'c':
		case 'C':
			_Channel();
			break;
		case 's':
		case 'S':
			_ToggleFavorite();
			break;
		case B_DELETE:
			if (_CanTrash())
				_Trash();
			break;
		case 'f':
		case 'F':
			fTarget.SendMessage(kMsgFullScreen);
			break;
		default:
			BView::KeyDown(bytes, count);
	}
}


void
DetailView::MessageReceived(BMessage* message)
{
	if (message->what == B_MOUSE_WHEEL_CHANGED) {
		float delta = 0;
		if (message->FindFloat("be:wheel_delta_y", &delta) == B_OK)
			_Scroll(delta * 40);
		return;
	}
	BView::MessageReceived(message);
}


void
DetailView::_Play()
{
	BMessage play(kMsgPlay);
	play.AddString("id", fItem.id);
	play.AddString("title", fItem.title);
	play.AddString("provider", fItem.provider);
	play.AddString("kind", fItem.kind);
	play.AddString("channel", fItem.channel);
	play.AddString("channel id", fItem.channelId);
	play.AddInt32("duration", fItem.duration);
	if (!fItem.path.IsEmpty())
		play.AddString("path", fItem.path);
	fTarget.SendMessage(&play);
}


void
DetailView::_Download()
{
	if (!_CanDownload())
		return;
	BMessage download(kMsgDownload);
	download.AddString("provider", fItem.provider);
	download.AddString("kind", fItem.kind);
	download.AddString("id", fItem.id);
	download.AddString("title", fItem.title);
	download.AddString("channel", fItem.channel);
	download.AddString("channel id", fItem.channelId);
	download.AddInt32("duration", fItem.duration);
	fTarget.SendMessage(&download);
}


void
DetailView::SetFollowed(const char* channelId, int32 followed)
{
	if (fItem.channelId != channelId || fFollowed == followed)
		return;
	bool layout = (fFollowed < 0) != (followed < 0);
	fFollowed = followed;
	if (layout) {
		_Update();
		Invalidate();
	} else
		Invalidate(fFollowFrame);
}


/*!	Its channel kept among the tabs, its latest videos on the home, or taken
	away from them.
*/
void
DetailView::_ToggleFollow()
{
	if (fItem.channelId.IsEmpty() || fFollowed < 0)
		return;
	BMessage follow(kMsgFollowChannel);
	follow.AddString("channel", fItem.channel);
	follow.AddString("channel id", fItem.channelId);
	follow.AddString("provider", fItem.provider);
	follow.AddBool("follow", fFollowed == 0);
	fTarget.SendMessage(&follow);
}


void
DetailView::_Channel()
{
	if (fItem.channelId.IsEmpty())
		return;
	BMessage channel(kMsgChannel);
	channel.AddString("channel", fItem.channel);
	channel.AddString("channel id", fItem.channelId);
	fTarget.SendMessage(&channel);
}


/*!	The star, filled when the video is among the favorites. */
void
DetailView::_DrawStar()
{
	if (!fStarFrame.IsValid())
		return;
	FillRound(this, fStarFrame, S(8), fHover == 5 ? kCard : kBar);
	StrokeRound(this, fStarFrame, S(8), 1, kTextDim);
	BPoint center(fStarFrame.left + fStarFrame.Width() / 2,
		fStarFrame.top + fStarFrame.Height() / 2 + S(1));
	float outer = fStarFrame.Height() * 0.34f;
	float inner = outer * 0.42f;
	BPoint points[10];
	for (int32 i = 0; i < 10; i++) {
		float radius = i % 2 == 0 ? outer : inner;
		float angle = -M_PI / 2 + i * M_PI / 5;
		points[i] = BPoint(center.x + radius * cosf(angle),
			center.y + radius * sinf(angle));
	}
	SetDrawingMode(B_OP_ALPHA);
	if (fItem.favorite) {
		SetHighColor(kAccent);
		FillPolygon(points, 10);
	} else {
		SetHighColor(kText);
		SetPenSize(S(1.5f));
		StrokePolygon(points, 10);
		SetPenSize(1);
	}
}


void
DetailView::_ToggleFavorite()
{
	if (fItem.id.IsEmpty())
		return;
	// the fields Favorites::Toggle() keeps, as the grid sends them
	BMessage favorite(kMsgToggleFavorite);
	favorite.AddString("id", fItem.id);
	favorite.AddString("provider", fItem.provider);
	favorite.AddString("kind", fItem.kind);
	favorite.AddString("title", fItem.title);
	favorite.AddString("description", fItem.description);
	favorite.AddString("channel", fItem.channel);
	favorite.AddString("channel id", fItem.channelId);
	favorite.AddInt32("duration", fItem.duration);
	favorite.AddInt32("year", fItem.year);
	favorite.AddString("poster", fItem.poster);
	favorite.AddString("thumbnail", fItem.thumbnail);
	if (!fItem.path.IsEmpty())
		favorite.AddString("path", fItem.path);
	fTarget.SendMessage(&favorite);
}


void
DetailView::_Trash()
{
	if (!_CanTrash())
		return;
	BMessage trash(kMsgTrash);
	trash.AddString("id", fItem.id);
	trash.AddString("title", fItem.title);
	// the window knows where a video downloaded from a list is
	trash.AddString("path", fItem.path);
	fTarget.SendMessage(&trash);
}


void
DetailView::_Back()
{
	fTarget.SendMessage(kMsgCloseDetails);
}
