/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "CategoryBar.h"

#include <Catalog.h>
#include <InterfaceDefs.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Window.h>

#include <string.h>

#include "Messages.h"
#include "Style.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Categories"


using namespace Style;


static const float kBaseHeight = 44;
static const float kBasePadding = 14;
static const float kBaseGap = 2;
static const float kFontSize = 15;
static const float kBaseChannelGap = 16;
static const float kBaseAddSize = 22;
static const float kBaseMoreWidth = 26;
static const float kBaseMoreGap = 6;


CategoryBar::CategoryBar(const std::vector<Category>& categories)
	:
	BView("categories", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fCategories(categories),
	fSelected(categories.empty() ? -1 : 0),
	fFocused(0),
	fAddHover(false),
	fMoreHover(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	SetFlags(Flags() | B_NAVIGABLE);
}


BSize
CategoryBar::MinSize()
{
	return BSize(100, S(kBaseHeight));
}


BSize
CategoryBar::PreferredSize()
{
	float width = 0;
	if (!fChannel.IsEmpty())
		width = _AddFrame().right;
	else if (_Count() > 0)
		width = _ButtonFrame(_Count() - 1).right;
	return BSize(width, S(kBaseHeight));
}


int32
CategoryBar::_Count() const
{
	return fCategories.size() + 2 + (fChannel.IsEmpty() ? 0 : 1);
}


const char*
CategoryBar::_Name(int32 index) const
{
	if (index == HomeIndex())
		return B_TRANSLATE("Home");
	if (index == DownloadsIndex())
		return B_TRANSLATE("Downloaded");
	if (index >= 2 && index < (int32)fCategories.size() + 2)
		return fCategories[index - 2].name.String();
	return fChannel.String();
}


BRect
CategoryBar::_ButtonFrame(int32 index) const
{
	BFont font = Font(kFontSize, true);
	float left = 0;
	for (int32 i = 0; i <= index; i++) {
		// a gap with a line before the categories and before the channel
		if (i == 2 && !fCategories.empty())
			left += S(kBaseChannelGap);
		else if (!fChannel.IsEmpty() && i == ChannelTab())
			left += S(kBaseChannelGap);
		if (i < index)
			left += font.StringWidth(_Name(i)) + 2 * S(kBasePadding) + S(kBaseGap);
	}
	float width = font.StringWidth(_Name(index)) + 2 * S(kBasePadding);
	return BRect(left, 0, left + width, S(kBaseHeight) - 1);
}


BRect
CategoryBar::_AddFrame() const
{
	if (fChannel.IsEmpty())
		return BRect();
	BRect tab = _ButtonFrame(ChannelTab());
	float top = (S(kBaseHeight) - S(kBaseAddSize)) / 2;
	return BRect(tab.right, top, tab.right + S(kBaseAddSize), top + S(kBaseAddSize));
}


BRect
CategoryBar::_MoreFrame() const
{
	float top = (S(kBaseHeight) - S(kBaseMoreWidth)) / 2;
	return BRect(Bounds().right - S(kBaseMoreWidth), top, Bounds().right,
		top + S(kBaseMoreWidth));
}


bool
CategoryBar::_HasMore() const
{
	if (_Count() <= 0)
		return false;
	return _ButtonFrame(_Count() - 1).right > Bounds().right;
}


float
CategoryBar::_FitLimit() const
{
	if (!_HasMore())
		return Bounds().right;
	return Bounds().right - S(kBaseMoreWidth) - S(kBaseMoreGap);
}


void
CategoryBar::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(kBar);
	FillRect(updateRect);

	float limit = _FitLimit();
	bool more = _HasMore();

	// tabs: the chosen one is brighter and underlined
	for (int32 i = 0; i < _Count(); i++) {
		BRect frame = _ButtonFrame(i);
		if (frame.right > limit)
			break;
		bool channel = !fChannel.IsEmpty() && i == ChannelTab();
		bool selected = i == fSelected || channel;
		bool separated = (i == 2 && !fCategories.empty()) || channel;
		if (separated) {
			// a line apart from the other groups
			SetDrawingMode(B_OP_COPY);
			SetHighColor(kCard);
			float x = frame.left - S(kBaseChannelGap) / 2;
			StrokeLine(BPoint(x, frame.top + S(12)),
				BPoint(x, frame.bottom - S(12)));
		}
		DrawCentered(this, _Name(i), frame, Font(kFontSize, selected),
			selected ? kText : kTextDim);
		if (selected) {
			SetDrawingMode(B_OP_COPY);
			SetHighColor(kAccent);
			FillRect(BRect(frame.left + S(kBasePadding) / 2,
				frame.bottom - S(2), frame.right - S(kBasePadding) / 2,
				frame.bottom));
		}
	}

	// the tabs that did not fit
	if (more) {
		BRect frame = _MoreFrame();
		bool hiddenSelected = fSelected >= 0 && fSelected < _Count()
			&& _ButtonFrame(fSelected).right > limit;
		if (fMoreHover)
			FillRound(this, frame, frame.Height() / 2, kCard);
		DrawCentered(this, "\xC2\xBB", frame, Font(20, true),
			fMoreHover ? kText : (hiddenSelected ? kAccent : kTextDim));
	}

	// the button that keeps the channel among the tabs
	BRect add = _AddFrame();
	if (add.IsValid() && add.right <= limit) {
		if (fAddHover)
			FillRound(this, add, add.Height() / 2, kCard);
		SetDrawingMode(B_OP_COPY);
		SetHighColor(fAddHover ? kText : kTextDim);
		SetPenSize(S(2));
		BPoint center(add.left + add.Width() / 2, add.top + add.Height() / 2);
		StrokeLine(center - BPoint(S(5), 0), center + BPoint(S(5), 0));
		StrokeLine(center - BPoint(0, S(5)), center + BPoint(0, S(5)));
		SetPenSize(1);
	}

	if (IsFocus() && fFocused >= 0 && fFocused < _Count()) {
		BRect frame = _ButtonFrame(fFocused);
		if (frame.right <= limit)
			StrokeRound(this, frame.InsetByCopy(1, S(6)), S(5), 1, kAccent);
	}
}


void
CategoryBar::KeyDown(const char* bytes, int32 count)
{
	if (count <= 0)
		return;
	switch (bytes[0]) {
		case B_LEFT_ARROW:
			_MoveFocus(-1);
			break;
		case B_RIGHT_ARROW:
			_MoveFocus(1);
			break;
		case B_ENTER:
		case B_SPACE:
			_Activate();
			break;
		case B_ESCAPE:
		case B_TAB:
			Window()->PostMessage(kMsgFocusContent);
			break;
		default:
			BView::KeyDown(bytes, count);
	}
}


void
CategoryBar::MakeFocus(bool focus)
{
	if (focus) {
		if (fSelected >= 0)
			fFocused = fSelected;
		else if (!fChannel.IsEmpty())
			fFocused = ChannelTab();
		else
			fFocused = HomeIndex();
		_ClampFocus();
	}
	BView::MakeFocus(focus);
	Invalidate();
}


void
CategoryBar::FocusSelected()
{
	MakeFocus(true);
}


void
CategoryBar::_MoveFocus(int32 delta)
{
	if (_Count() <= 0)
		return;
	int32 index = fFocused;
	for (int32 i = 0; i < _Count(); i++) {
		index += delta;
		if (index < 0)
			index = _Count() - 1;
		else if (index >= _Count())
			index = 0;
		if (_IsVisible(index)) {
			if (index != fFocused) {
				fFocused = index;
				Invalidate();
			}
			return;
		}
	}
}


void
CategoryBar::_Activate()
{
	if (fFocused == HomeIndex())
		Window()->PostMessage(kMsgHome);
	else if (fFocused == DownloadsIndex())
		Window()->PostMessage(kMsgShowDownloads);
	else if (fFocused >= 2 && fFocused < ChannelTab()) {
		BMessage message(kMsgCategory);
		message.AddInt32("index", fFocused - 2);
		Window()->PostMessage(&message);
	} else if (fFocused == ChannelTab())
		Window()->PostMessage(kMsgFocusContent);
}


bool
CategoryBar::_IsVisible(int32 index) const
{
	return index >= 0 && index < _Count()
		&& _ButtonFrame(index).right <= _FitLimit();
}


void
CategoryBar::_ClampFocus()
{
	if (_Count() <= 0) {
		fFocused = -1;
		return;
	}
	if (_IsVisible(fFocused))
		return;
	for (int32 i = _Count() - 1; i >= 0; i--) {
		if (_IsVisible(i)) {
			fFocused = i;
			return;
		}
	}
}


void
CategoryBar::MouseDown(BPoint where)
{
	int32 buttons = B_PRIMARY_MOUSE_BUTTON;
	Window()->CurrentMessage()->FindInt32("buttons", &buttons);

	float limit = _FitLimit();
	if (_HasMore() && _MoreFrame().Contains(where)) {
		_ShowMore(where);
		return;
	}
	BRect add = _AddFrame();
	if (add.IsValid() && add.right <= limit && add.Contains(where)) {
		Window()->PostMessage(kMsgAddChannel);
		return;
	}
	BRect home = _ButtonFrame(HomeIndex());
	if (home.right <= limit && home.Contains(where)) {
		Window()->PostMessage(kMsgHome);
		return;
	}
	BRect downloads = _ButtonFrame(DownloadsIndex());
	if (downloads.right <= limit && downloads.Contains(where)) {
		Window()->PostMessage(kMsgShowDownloads);
		return;
	}
	for (size_t i = 0; i < fCategories.size(); i++) {
		BRect frame = _ButtonFrame(CategoryTab(i));
		if (frame.right > limit)
			break;
		if (frame.Contains(where)) {
			if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
				_ShowMenu(i, where);
				return;
			}
			BMessage message(kMsgCategory);
			message.AddInt32("index", i);
			Window()->PostMessage(&message);
			return;
		}
	}
}


void
CategoryBar::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	float limit = _FitLimit();
	BRect add = _AddFrame();
	bool addHover = transit != B_EXITED_VIEW && add.IsValid()
		&& add.right <= limit && add.Contains(where);
	if (addHover != fAddHover) {
		fAddHover = addHover;
		SetToolTip(addHover
			? B_TRANSLATE("Add this channel to the tabs") : NULL);
		if (addHover)
			ShowToolTip(ToolTip());
		Invalidate(add);
	}

	bool moreHover = transit != B_EXITED_VIEW && _HasMore()
		&& _MoreFrame().Contains(where);
	if (moreHover != fMoreHover) {
		fMoreHover = moreHover;
		Invalidate(_MoreFrame());
	}
}


void
CategoryBar::_ShowMenu(int32 index, BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu("tab", false, false);
	// a search: in which order its videos come
	if (index >= 0 && index < (int32)fCategories.size()
		&& !fCategories[index].IsChannel()) {
		const BString& order = fCategories[index].order;
		static const struct { const char* order; const char* label; } kOrders[]
			= {
			{ "newest", B_TRANSLATE_MARK("Newest first") },
			{ "relevant", B_TRANSLATE_MARK("Most relevant first") },
		};
		for (const auto& choice : kOrders) {
			BMessage* message = new BMessage(kMsgCategoryOrder);
			message->AddInt32("index", index);
			message->AddString("order", choice.order);
			BMenuItem* item = new BMenuItem(
				B_TRANSLATE_NOCOLLECT(choice.label), message);
			// as the source gives them is the most relevant first
			item->SetMarked(order == choice.order || (order.IsEmpty()
				&& strcmp(choice.order, "relevant") == 0));
			menu->AddItem(item);
		}
		menu->AddSeparatorItem();
	}
	BMessage* message = new BMessage(kMsgRemoveCategory);
	message->AddInt32("index", index);
	menu->AddItem(new BMenuItem(B_TRANSLATE("Remove from the tabs"),
		message));
	menu->SetTargetForItems(Window());
	ScaleMenu(menu);
	menu->Go(ConvertToScreen(where), true, false, true);
}


void
CategoryBar::_ShowMore(BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu("more", false, false);
	BMenuItem* item = new BMenuItem(_Name(HomeIndex()),
		new BMessage(kMsgHome));
	item->SetMarked(fSelected == HomeIndex());
	menu->AddItem(item);
	item = new BMenuItem(_Name(DownloadsIndex()),
		new BMessage(kMsgShowDownloads));
	item->SetMarked(fSelected == DownloadsIndex());
	menu->AddItem(item);
	if (!fCategories.empty())
		menu->AddSeparatorItem();
	for (size_t i = 0; i < fCategories.size(); i++) {
		BMessage* message = new BMessage(kMsgCategory);
		message->AddInt32("index", i);
		item = new BMenuItem(fCategories[i].name.String(), message);
		item->SetMarked(fSelected == CategoryTab(i));
		menu->AddItem(item);
	}
	if (!fCategories.empty()) {
		menu->AddSeparatorItem();
		BPopUpMenu* remove = new BPopUpMenu(
			B_TRANSLATE("Remove from the tabs"));
		for (size_t i = 0; i < fCategories.size(); i++) {
			BMessage* message = new BMessage(kMsgRemoveCategory);
			message->AddInt32("index", i);
			remove->AddItem(new BMenuItem(
				fCategories[i].name.String(), message));
		}
		remove->SetTargetForItems(Window());
		menu->AddItem(new BMenuItem(remove));
	}
	menu->SetTargetForItems(Window());
	ScaleMenu(menu);
	menu->Go(ConvertToScreen(where), true, false, true);
}


void
CategoryBar::SetCategories(const std::vector<Category>& categories)
{
	fCategories = categories;
	if (IsFocus())
		_ClampFocus();
	InvalidateLayout();
	Invalidate();
}


void
CategoryBar::SetSelected(int32 index)
{
	fSelected = index;
	SetChannel(NULL);
	if (IsFocus()) {
		fFocused = index >= 0 ? index : HomeIndex();
		_ClampFocus();
	}
	Invalidate();
}


void
CategoryBar::SetChannel(const char* name)
{
	BString channel(name);
	if (channel == fChannel)
		return;
	fChannel = channel;
	fAddHover = false;
	fMoreHover = false;
	if (!fChannel.IsEmpty())
		fSelected = -1;
	if (IsFocus()) {
		fFocused = fChannel.IsEmpty() ? HomeIndex() : ChannelTab();
		_ClampFocus();
	}
	InvalidateLayout();
	Invalidate();
}
