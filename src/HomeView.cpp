/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "HomeView.h"

#include <algorithm>

#include <Bitmap.h>
#include <Catalog.h>
#include <Cursor.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <Window.h>

#include "Messages.h"
#include "Style.h"
#include "VideoGrid.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Home"


using namespace Style;


static const float kBaseTileWidth = 210;
static const float kBaseHeaderHeight = 36;
static const float kBaseTop = 10;
static const float kBaseGap = 4;
static const float kBaseBottom = 8;
static const float kSeeAllSize = 14;


namespace {


class ShelfHeader : public BView {
public:
								ShelfHeader(const char* title,
									const BMessage& seeAll)
									:
									BView("shelf", B_WILL_DRAW
										| B_FULL_UPDATE_ON_RESIZE),
									fTitle(title),
									fSeeAll(seeAll),
									fHover(false)
									{
										SetViewColor(kBackground);
									}

	virtual	void				Draw(BRect updateRect)
									{
										BFont font = Font(20, true);
										BRect link = _LinkFrame();
										float space = link.IsValid()
											? link.Width() + S(16) : 0;
										BString title(fTitle);
										font.TruncateString(&title,
											B_TRUNCATE_END,
											Bounds().Width() - space);
										DrawText(this, title.String(),
											BPoint(0, S(24)), font, kText);

										if (link.IsValid()) {
											BFont small = Font(kSeeAllSize);
											float baseline = S(23);
											DrawText(this,
												B_TRANSLATE("See all"),
												BPoint(link.left, baseline),
												small,
												fHover ? kText : kAccent);
											if (fHover) {
												SetHighColor(kText);
												StrokeLine(
													BPoint(link.left,
														baseline + S(2)),
													BPoint(link.right,
														baseline + S(2)));
											}
										}
									}

	virtual	void				MouseDown(BPoint where)
									{
										if (!_LinkFrame().Contains(where))
											return;
										int32 buttons =
											B_PRIMARY_MOUSE_BUTTON;
										Window()->CurrentMessage()->FindInt32(
											"buttons", &buttons);
										if ((buttons
												& B_PRIMARY_MOUSE_BUTTON) != 0)
											Window()->PostMessage(&fSeeAll);
									}

	virtual	void				MouseMoved(BPoint where,
										uint32 transit,
										const BMessage* dragMessage)
									{
										bool hover = transit != B_EXITED_VIEW
											&& _LinkFrame().Contains(where);
										if (hover == fHover)
											return;
										fHover = hover;
										Invalidate(_LinkFrame().
											InsetByCopy(-1, -S(2)));
										BCursor cursor(hover
											? B_CURSOR_ID_FOLLOW_LINK
											: B_CURSOR_ID_SYSTEM_DEFAULT);
										SetViewCursor(&cursor);
									}

	virtual	BSize				MinSize()
									{
										return BSize(S(100),
											S(kBaseHeaderHeight));
									}

	virtual	BSize				PreferredSize()
									{
										return BSize(S(100),
											S(kBaseHeaderHeight));
									}

private:
			BRect				_LinkFrame() const
									{
										if (fSeeAll.what == 0)
											return BRect();
										BFont font = Font(kSeeAllSize);
										float width = font.StringWidth(
											B_TRANSLATE("See all"));
										return BRect(Bounds().right - width,
											0, Bounds().right,
											Bounds().Height());
									}

			BString				fTitle;
			BMessage			fSeeAll;
			bool				fHover;
};


}	// namespace


HomeView::HomeView()
	:
	BView("home", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE
		| B_SCROLL_VIEW_AWARE),
	fFocusedShelf(0),
	fContentHeight(0),
	fLayingOut(false)
{
	SetViewColor(kBackground);
}


HomeView::~HomeView()
{
	_Clear();
}


void
HomeView::_Clear()
{
	for (Shelf& shelf : fShelves) {
		RemoveChild(shelf.group);
		delete shelf.group;
	}
	fShelves.clear();
}


void
HomeView::SetShelves(const std::vector<ShelfSpec>& shelves)
{
	_Clear();
	fMessage = "";
	for (const ShelfSpec& spec : shelves) {
		ShelfHeader* header = new ShelfHeader(spec.title, spec.seeAll);
		VideoGrid* row = new VideoGrid;
		row->SetShelfMode(S(kBaseTileWidth));
		// what was watched or downloaded has no views nor date: no line
		// kept empty for them
		bool compact = !spec.videos.empty();
		for (const MediaItem& video : spec.videos) {
			if (video.views > 0 || !video.date.IsEmpty())
				compact = false;
		}
		row->SetCompact(compact);
		float rowHeight = row->ShelfHeight();
		row->SetExplicitMinSize(BSize(B_SIZE_UNSET, rowHeight));
		row->SetExplicitMaxSize(BSize(B_SIZE_UNSET, rowHeight));
		if (!spec.videos.empty())
			row->SetVideos(spec.videos);
		else if (!spec.message.IsEmpty())
			row->SetMessage(spec.message);

		BGroupView* group = new BGroupView(B_VERTICAL, S(kBaseGap));
		group->SetViewColor(kBackground);
		BLayoutBuilder::Group<>(group)
			.SetInsets(S(24), S(kBaseTop), S(24), S(kBaseBottom))
			.Add(header)
			.Add(row);
		AddChild(group);
		fShelves.push_back({group, row});
	}
	ScrollTo(0, 0);
	_Relayout();
	Invalidate();
}


void
HomeView::SetShelfVideos(int32 index, const std::vector<MediaItem>& videos)
{
	if (index < 0 || index >= (int32)fShelves.size())
		return;
	fShelves[index].row->SetVideos(videos);
}


void
HomeView::SetShelfMessage(int32 index, const char* message)
{
	if (index < 0 || index >= (int32)fShelves.size())
		return;
	fShelves[index].row->SetMessage(message);
}


void
HomeView::SetPicture(const char* id, BBitmap* picture)
{
	bool found = false;
	for (Shelf& shelf : fShelves) {
		if (!shelf.row->HasVideo(id))
			continue;
		if (!found) {
			shelf.row->SetPicture(id, picture);
			found = true;
		} else {
			BBitmap* copy = picture != NULL
				? new BBitmap(picture) : NULL;
			shelf.row->SetPicture(id, copy);
		}
	}
	if (!found)
		delete picture;
}


void
HomeView::SetProgress(const char* id, int32 progress)
{
	for (Shelf& shelf : fShelves)
		shelf.row->SetProgress(id, progress);
}


void
HomeView::SetFavorite(const char* id, bool favorite)
{
	for (Shelf& shelf : fShelves)
		shelf.row->SetFavorite(id, favorite);
}


void
HomeView::SetDownloaded(const char* id, bool downloaded)
{
	for (Shelf& shelf : fShelves)
		shelf.row->SetDownloaded(id, downloaded);
}


void
HomeView::SetBusy(const char* id, const char* title)
{
	int32 target = -1;
	for (size_t i = 0; i < fShelves.size(); i++)
		if (fShelves[i].row->IsFocus()
			&& fShelves[i].row->HasVideo(id)) {
			target = i;
			break;
		}
	if (target < 0)
		for (size_t i = 0; i < fShelves.size(); i++)
			if (fShelves[i].row->HasVideo(id)) {
				target = i;
				break;
			}
	for (size_t i = 0; i < fShelves.size(); i++)
		fShelves[i].row->SetBusy((int32)i == target ? title : NULL);
}


void
HomeView::ClearBusy()
{
	for (Shelf& shelf : fShelves)
		shelf.row->SetBusy(NULL);
}


void
HomeView::FailPending(const char* message)
{
	for (Shelf& shelf : fShelves)
		if (shelf.row->IsEmpty())
			shelf.row->SetMessage(message);
}


bool
HomeView::MoveFocus(const char* id, int32 delta)
{
	int32 current = -1;
	for (size_t i = 0; i < fShelves.size(); i++) {
		if (fShelves[i].row->IsFocus()
			&& fShelves[i].row->HasVideo(id)) {
			current = i;
			break;
		}
	}
	if (current < 0)
		for (size_t i = 0; i < fShelves.size(); i++)
			if (fShelves[i].row->HasVideo(id)) {
				current = i;
				break;
			}
	if (current < 0)
		return false;

	int32 target = std::max<int32>(0,
		std::min<int32>(current + delta, fShelves.size() - 1));
	for (Shelf& item : fShelves)
		item.row->Invalidate();
	Shelf& shelf = fShelves[target];
	shelf.row->MakeFocus(true);
	fFocusedShelf = target;

	_UpdateScrollBar();
	BView* parent = Parent();
	float visible = parent != NULL
		? static_cast<BScrollView*>(parent)->Bounds().Height()
		: Bounds().Height();
	float top = shelf.group->Frame().top;
	float bottom = shelf.group->Frame().bottom;
	float value = Bounds().top;
	if (top < value)
		ScrollTo(0, std::max(0.0f, top));
	else if (bottom > value + visible)
		ScrollTo(0, bottom - visible);
	return true;
}


void
HomeView::MakeFirstFocus()
{
	fFocusedShelf = 0;
	if (fShelves.empty()) {
		MakeFocus(true);
	} else {
		ScrollTo(0, 0);
		for (Shelf& shelf : fShelves)
			shelf.row->Invalidate();
		fShelves[0].row->MakeFocus(true);
	}
}


void
HomeView::MakeLastFocus()
{
	if (fShelves.empty()) {
		MakeFocus(true);
		return;
	}
	int32 target = std::max<int32>(0,
		std::min<int32>(fFocusedShelf, fShelves.size() - 1));
	for (Shelf& item : fShelves)
		item.row->Invalidate();
	Shelf& shelf = fShelves[target];
	shelf.row->MakeFocus(true);

	BView* parent = Parent();
	float visible = parent != NULL
		? static_cast<BScrollView*>(parent)->Bounds().Height()
		: Bounds().Height();
	float top = shelf.group->Frame().top;
	float bottom = shelf.group->Frame().bottom;
	float value = Bounds().top;
	if (top < value)
		ScrollTo(0, std::max(0.0f, top));
	else if (bottom > value + visible)
		ScrollTo(0, bottom - visible);
}


void
HomeView::RememberFocus()
{
	for (size_t i = 0; i < fShelves.size(); i++)
		if (fShelves[i].row->IsFocus()) {
			fFocusedShelf = (int32)i;
			return;
		}
}


void
HomeView::SetMessage(const char* message)
{
	_Clear();
	fMessage = message != NULL ? message : "";
	fContentHeight = 0;
	ScrollTo(0, 0);
	_Relayout();
	Invalidate();
}


void
HomeView::_Relayout()
{
	if (fLayingOut)
		return;
	fLayingOut = true;
	float width = std::max(1.0f, Bounds().Width());
	float shelfHeight = S(kBaseTop) + S(kBaseHeaderHeight) + S(kBaseGap)
		+ VideoGrid::ShelfHeightFor(S(kBaseTileWidth)) + S(kBaseBottom);
	float y = 0;
	for (Shelf& shelf : fShelves) {
		shelf.group->MoveTo(0, y);
		shelf.group->ResizeTo(width, shelfHeight);
		y += shelfHeight;
	}
	fContentHeight = y;
	_UpdateScrollBar();
	fLayingOut = false;
}


void
HomeView::_UpdateScrollBar()
{
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (bar == NULL)
		return;
	BView* parent = Parent();
	BScrollView* scroll = parent != NULL
		? static_cast<BScrollView*>(parent) : NULL;
	float visible = (scroll != NULL ? scroll->Bounds().Height()
		: Bounds().Height()) + 1;
	float range = std::max(0.0f, fContentHeight - visible);
	bar->SetRange(0, range);
	bar->SetProportion(fContentHeight > 0
		? std::min(1.0f, visible / fContentHeight) : 1);
	bar->SetSteps(S(40), visible * 0.9f);
}


void
HomeView::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Relayout();
}


void
HomeView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(kBackground);
	FillRect(updateRect);

	if (fShelves.empty() && !fMessage.IsEmpty()) {
		BRect bounds = Bounds();
		DrawCentered(this, fMessage.String(), BRect(bounds.left,
			bounds.top + bounds.Height() / 2 - S(40), bounds.right,
			bounds.top + bounds.Height() / 2), Font(18), kTextDim);
	}

}


void
HomeView::FrameResized(float width, float height)
{
	_Relayout();
	Invalidate();
}
