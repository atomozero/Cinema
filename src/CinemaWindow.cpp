/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "CinemaWindow.h"

#include <Alert.h>
#include <Invoker.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <CardLayout.h>
#include <Entry.h>
#include <Button.h>
#include <Catalog.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageQueue.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <PopUpMenu.h>
#include <Url.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollView.h>
#include <TextControl.h>
#include <TranslationUtils.h>
#include <View.h>

#include "Pictures.h"
#include "CategoryBar.h"
#include "DetailView.h"
#include "Downloads.h"
#include "Favorites.h"
#include "Gamepad.h"
#include "Helper.h"
#include "History.h"
#include "LiveAddress.h"
#include "HomeView.h"
#include "Player.h"
#include "PlayerView.h"
#include "MenuButton.h"
#include "Positions.h"
#include "SettingsWindow.h"
#include "Messages.h"
#include "Style.h"
#include "TvLists.h"
#include "VideoGrid.h"
#include "Watchdog.h"
#include "Worker.h"
#include "PictureName.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Window"


using namespace Style;


static const char* kMediaPlayer = "application/x-vnd.Haiku-MediaPlayer";
static const int32 kResultCount = 24;
static const bigtime_t kPrefetchDelay = 1200000;
	// the time a video stays chosen before its file is looked for
static const bigtime_t kStreamLifetime = 3LL * 60 * 60 * 1000000;
	// YouTube's addresses expire after about six hours


static bool
direct_enabled()
{
	const char* value = getenv("CINEMA_DIRECT");
	return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}


CinemaWindow::CinemaWindow()
	:
	BDirectWindow(BRect(S(50), S(50), S(1000), S(650)),
		B_TRANSLATE_SYSTEM_NAME("Cinema"),
		B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE),
	fCategories(LoadCategories()),
	fProvider("youtube"),
	fDetailsShown(false),
	fHelper(NULL),
	fGamepad(NULL),
	fGeneration(0),
	fHomeGeneration(0),
	fHomeLoaded(false),
	fHomeActive(0),
	fDetailsReturn(0),
	fCard(2),
	fPrefetchRunner(NULL),
	fFullScreen(false),
	fDirectActive(false),
	fDirectFailed(false),
	fDirectStart(0),
	fDirectFallbackAsked(false),
	fDirectPaused(false),
	fDirectLock("direct draw"),
	fDirectConnected(false),
	fDirectBits(NULL),
	fDirectRowBytes(0),
	fDirectBpp(0),
	fDirectFormat(B_NO_COLOR_SPACE),
	fDirectBounds((clipping_rect){ 0, 0, -1, -1 }),
	fDirectUi(NULL),
	fDirectUiView(NULL),
	fDirectRunner(NULL)
{
	fCategoryBar = new CategoryBar(fCategories);
	fSearchField = new BTextControl("search", NULL, "",
		new BMessage(kMsgSearchField));
	BButton* searchButton = new BButton("find", B_TRANSLATE("Search"),
		new BMessage(kMsgSearchField));
	// where the search goes: filled when the helper tells its providers
	BPopUpMenu* sources = new BPopUpMenu("sources");
	Style::ScaleMenu(sources);
	fSourceField = new BMenuField("source", NULL, sources);
	fSourceField->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_UNSET,
		B_ALIGN_VERTICAL_CENTER));
	fSourceField->Hide();

	fGrid = new VideoGrid;
	BScrollView* scroll = new BScrollView("scroll", fGrid, 0, false, true,
		B_NO_BORDER);
	fDetail = new DetailView(BMessenger(this));
	fHome = new HomeView;
	BScrollView* homeScroll = new BScrollView("home-scroll", fHome, 0,
		false, true, B_NO_BORDER);

	// the home, the grid and the detail view take turns in the same place
	BView* pages = new BView("pages", B_WILL_DRAW);
	pages->SetViewColor(Style::kBackground);
	fCards = new BCardLayout();
	pages->SetLayout(fCards);
	pages->AddChild(scroll);
	pages->AddChild(fDetail);
	pages->AddChild(homeScroll);
	fSettings = Settings::Load();
	fSearchProvider = fSettings.searchSource;
	_OfferTv();
	AddShortcut(',', B_COMMAND_KEY, new BMessage(kMsgShowSettings));

	// the player, the fourth card
	fPlayer = new Player(BMessenger(this));
	// as it was left the last time
	fPlayer->SetVolume(fSettings.volume);
	fPlayer->SetOverlayAllowed(_OverlayDecision() != 0);
	fPlayer->SetNormalized(fSettings.evenVolume);
	fPlayer->SetSubtitlePreference(fSettings.subtitleLanguage.String());
	fPlayerView = new PlayerView(fPlayer);
	fPlayerShown = false;
	fPlayerReturn = 2;
	fPlayerSaver = NULL;
	pages->AddChild(fPlayerView);
	fCards->SetVisibleItem(2);
	fCard = 2;

	// the field as high as the text, not as the bar
	BLayoutItem* textItem = fSearchField->CreateTextViewLayoutItem();
	textItem->SetExplicitMinSize(BSize(S(320), B_SIZE_UNSET));
	textItem->SetExplicitMaxSize(BSize(S(460),
		textItem->PreferredSize().height));
	textItem->SetExplicitAlignment(BAlignment(B_ALIGN_USE_FULL_WIDTH,
		B_ALIGN_VERTICAL_CENTER));
	searchButton->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_UNSET,
		B_ALIGN_VERTICAL_CENTER));

	BGroupView* top = new BGroupView(B_HORIZONTAL, B_USE_SMALL_SPACING);
	fTopBar = top;
	top->SetViewColor(Style::kBar);
	BLayoutBuilder::Group<>(top)
		.SetInsets(S(12), S(6), S(12), S(6))
		.Add(fCategoryBar, 10)
		.Add(fSourceField, 0)
		.Add(textItem)
		.Add(searchButton)
		.Add(new MenuButton);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(top)
		.Add(pages, 10);
	Style::ScaleView(top);

	fWorker = new Worker(BMessenger(this));
	fWorker->Run();
	// to the preferred handler, so that the window gives the keys to the
	// focus view as it does with the keyboard's
	fGamepad = new Gamepad(BMessenger(NULL, this));

	// the helper is next to the application
	fHelper = new Helper(BMessenger(this));
	app_info info;
	BPath script;
	if (be_app->GetAppInfo(&info) == B_OK
		&& script.SetTo(&info.ref) == B_OK && script.GetParent(&script) == B_OK)
		script.Append("cinema-helper.py");
	// CINEMA_VIDEO_HEIGHT=720, say: that size for this run, to try it
	// without changing the settings
	const char* height = getenv("CINEMA_VIDEO_HEIGHT");
	fHelper->SetHeight(height != NULL ? atoi(height) : fSettings.videoHeight);
	fHelper->Start(script.Path(), fWorker->Cache());
	Watchdog::Start(fWorker->Cache(), BMessenger(this));

	// nearly the whole screen
	BRect frame = BScreen(this).Frame();
	MoveTo(frame.left + S(20), frame.top + S(30));
	ResizeTo(frame.Width() - S(40), frame.Height() - S(60));

	// which videos can be watched offline: their names only
	Downloads::Load(&fDownloaded, 0);
	_ShowHome();
	// the torrents interrupted when Cinema went away go on
	for (BMessage& download : Downloads::LoadPending()) {
		download.what = kMsgDownload;
		_Download(&download);
	}
	fGamepad->Start();
}


CinemaWindow::~CinemaWindow()
{
	// a quit that hangs is said too
	Watchdog::Enter('quit');
	delete fGamepad;
	delete fPrefetchRunner;
	delete fOverlayAsker;
	delete fPlayerSaver;
	delete fDirectRunner;
	fDirectRunner = NULL;
	if (fDirectUi != NULL) {
		if (fDirectUiView != NULL) {
			fDirectUi->RemoveChild(fDirectUiView);
			delete fDirectUiView;
		}
		delete fDirectUi;
	}
	delete fPlayer;
	delete fHelper;
}


void
CinemaWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
	Watchdog::Scope watched(message->what);
	// Esc goes back from the details, then leaves the full screen, and F
	// still toggles it on the details, wherever the focus is. F6 and Tab
	// move between the tabs and the videos, also with the gamepad.
	if (message->what == B_KEY_DOWN) {
		int32 key = 0;
		if (message->FindInt32("key", &key) == B_OK && key == B_F6_KEY) {
			_ToggleCategoryFocus();
			return;
		}
		const char* bytes;
		if (message->FindString("bytes", &bytes) == B_OK) {
			if (bytes[0] == B_TAB) {
				_ToggleCategoryFocus();
				return;
			}
			if (bytes[0] == B_ESCAPE) {
				if (fPlayerShown) {
					_ClosePlayer();
					return;
				}
				if (fCategoryBar->IsFocus()) {
					_FocusContent();
					return;
				}
				if (fDetailsShown) {
					_CloseDetails();
					return;
				}
				if (fFullScreen) {
					_SetFullScreen(false);
					return;
				}
			}
			// not while typing a search
			if (fDetailsShown && (bytes[0] == 'f' || bytes[0] == 'F')
				&& !fSearchField->TextView()->IsFocus()) {
				_SetFullScreen(!fFullScreen);
				return;
			}
		}
	}
	BDirectWindow::DispatchMessage(message, handler);
}


bool
CinemaWindow::QuitRequested()
{
	fGamepad->Stop();
	// where it was left, and no more sound
	_ClosePlayer();
	// no picture made once the application is gone; curl, which it may be
	// waiting for, is not waited for
	fWorker->Stop();
	fWorker->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
CinemaWindow::_Search(const char* query, const char* provider)
{
	_CloseDetails();
	fCards->SetVisibleItem((int32)0);
	fCard = 0;
	fGeneration++;
	fGrid->ClearChannelHeader();
	fGrid->SetMessage(B_TRANSLATE("Looking for the videos" B_UTF8_ELLIPSIS));
	fHelper->Search(fGeneration, kResultCount, query, provider);
}


/*!	The provider a video shown came from, or the default one. */
BString
CinemaWindow::_ProviderOf(const BString& id) const
{
	auto found = fItemProviders.find(id);
	return found != fItemProviders.end() ? found->second : fProvider;
}


/*!	YouTube if the helper has it, otherwise the first provider that searches:
	the others are there for when there are more.
*/
void
CinemaWindow::_SetProviders(BMessage* message)
{
	fProviders = ProvidersFromMessage(*message);

	BString chosen;
	for (const Provider& provider : fProviders)
		if (provider.id == "youtube")
			chosen = provider.id;
	if (chosen.IsEmpty())
		for (const Provider& provider : fProviders)
			if (provider.Supports("search")) {
				chosen = provider.id;
				break;
			}
	if (!chosen.IsEmpty() && chosen != fProvider) {
		fProvider = chosen;
		fHelper->SetProvider(fProvider.String());
	}
	_UpdateSources();
}


/*!	The menu of where to search, with every provider that searches; it only
	shows when there is a choice.
*/
void
CinemaWindow::_UpdateSources()
{
	BMenu* menu = fSourceField->Menu();
	while (BMenuItem* item = menu->RemoveItem((int32)0))
		delete item;
	if (fSearchProvider.IsEmpty())
		fSearchProvider = fProvider;

	int32 count = 0;
	for (const Provider& provider : fProviders) {
		if (!provider.Supports("search"))
			continue;
		BMessage* message = new BMessage(kMsgSearchSource);
		message->AddString("provider", provider.id);
		BMenuItem* item = new BMenuItem(provider.name.String(), message);
		item->SetTarget(this);
		item->SetMarked(provider.id == fSearchProvider);
		menu->AddItem(item);
		count++;
	}
	Style::ScaleMenu(menu);
	if (count > 1 && fSourceField->IsHidden())
		fSourceField->Show();
	else if (count <= 1 && !fSourceField->IsHidden())
		fSourceField->Hide();
}


void
CinemaWindow::_ShowCategory(int32 index)
{
	if (index < 0 || index >= (int32)fCategories.size())
		return;
	_CloseDetails();
	fCards->SetVisibleItem((int32)0);
	fCard = 0;
	fCategoryBar->SetSelected(fCategoryBar->CategoryTab(index));
	fChannelId = "";
	const Category& category = fCategories[index];
	if (category.IsChannel()) {
		fGeneration++;
		_SetChannelHeader(category.name.String(), category.channel.String(),
			category.provider.String());
		fGrid->SetMessage(B_TRANSLATE("Looking for the videos"
			B_UTF8_ELLIPSIS));
		fHelper->Channel(fGeneration, kResultCount, category.channel,
			category.provider.String());
	} else
		_Search(category.query.String(), category.provider.String());
	fGrid->MakeFocus(true);
}


void
CinemaWindow::_ShowChannel(const char* name, const char* id)
{
	if (id[0] == '\0')
		return;
	_CloseDetails();
	fCards->SetVisibleItem((int32)0);
	fCard = 0;
	// one of the tabs already?
	for (size_t i = 0; i < fCategories.size(); i++) {
		if (fCategories[i].channel == id) {
			_ShowCategory(i);
			return;
		}
	}
	fCategoryBar->SetChannel(name);
	fChannelName = name;
	fChannelId = id;
	auto provider = fChannelProviders.find(id);
	fChannelProvider = provider != fChannelProviders.end()
		? provider->second : BString();
	fGeneration++;
	_SetChannelHeader(name, id, fChannelProvider.String());
	fGrid->SetMessage(B_TRANSLATE("Looking for the videos" B_UTF8_ELLIPSIS));
	fHelper->Channel(fGeneration, kResultCount, id,
		fChannelProvider.String());
	fGrid->MakeFocus(true);
}


void
CinemaWindow::_AddChannel(const char* name, const char* id)
{
	if (name[0] == '\0' || id[0] == '\0')
		return;
	for (const Category& category : fCategories) {
		if (category.channel == id)
			return;
	}
	Category category;
	category.name = name;
	category.channel = id;
	auto provider = fChannelProviders.find(id);
	if (provider != fChannelProviders.end() && provider->second != fProvider)
		category.provider = provider->second;
	fCategories.push_back(category);
	SaveCategories(fCategories);

	int32 selected = fCategoryBar->Selected();
	fCategoryBar->SetCategories(fCategories);
	if (fChannelId == id) {
		// the tab of its own becomes one of the others
		fCategoryBar->SetSelected(
			fCategoryBar->CategoryTab(fCategories.size() - 1));
		fChannelId = "";
	} else if (selected >= 0)
		fCategoryBar->SetSelected(selected);
	_HomeChanged();
}


/*!	The tabs changed: the shelves of the home, which know them by their
	place, are made again, now or when the page of a video over them closes.
*/
/*!	The pictures of a video cost more than this processor has: a video of
	YouTube is found again a size smaller, 720p for 1080p and so on down to
	360p, and goes on from where it is once found, the old one playing
	meanwhile. Not a file, nor TV: they have one size only.
*/
void
CinemaWindow::_LowerQuality(int32 height)
{
	if (!fPlayerShown || fPlayingId.IsEmpty() || fPlayingKind == "live"
		|| _ProviderOf(fPlayingId) != "youtube"
		|| fFinding.count(fPlayingId) > 0)
		return;
	static const int32 kSizes[] = { 720, 480, 360 };
	int32 smaller = 0;
	for (int32 size : kSizes) {
		if (size < height) {
			smaller = size;
			break;
		}
	}
	if (smaller == 0)
		return;
	_SavePosition();
	fOpenAt = fPlayer->Position();
	fLowered = fPlayingId;
	fStreams.erase(fPlayingId);
	fHelper->SetHeight(smaller);
	fWaiting = fPlayingId;
	fWaitingTitle = fPlayingTitle;
	_Find(fPlayingId);
}


/*!	The overlay as the settings want it, on the video playing too: its view
	gives up the key color first.
*/
void
CinemaWindow::_ApplyOverlay()
{
	int32 decision = _OverlayDecision();
	bool allowed = decision != 0;
	fAskOverlay = decision == 2;
	if (!fAskOverlay)
		fPlayerView->SetOverlayQuestion(NULL);
	if (!allowed && fPlayer->Overlay() != NULL)
		fPlayerView->ClearOverlay();
	fPlayer->SetOverlayAllowed(allowed);
}


/*!	The graphics card, as its driver names it: "NVIDIA Tegra 2 (Tegra20 DC)".
*/
BString
CinemaWindow::_OverlayCard() const
{
	BScreen screen(const_cast<CinemaWindow*>(this));
	accelerant_device_info info;
	BString card;
	if (screen.GetDeviceInfo(&info) == B_OK) {
		card.SetTo(info.name, sizeof(info.name));
		BString chipset;
		chipset.SetTo(info.chipset, sizeof(info.chipset));
		if (!chipset.IsEmpty() && chipset != card)
			card << " (" << chipset << ")";
	}
	return card;
}


/*!	Whether to use the overlay: 1 yes, 0 no, 2 yes and ask whether the video
	shows. The screen cannot tell: the overlay is put over the picture as it
	goes out, after what app_server sees, and some drivers accept one and
	show only its key color.
*/
int32
CinemaWindow::_OverlayDecision() const
{
	// CINEMA_ASK_OVERLAY=1 asks on any card, to try the question
	const char* ask = getenv("CINEMA_ASK_OVERLAY");
	if (ask != NULL && strcmp(ask, "1") == 0)
		return 2;
	if (fSettings.overlayChoice == kOverlayAlways)
		return 1;
	if (fSettings.overlayChoice == kOverlayNever)
		return 0;
	BString card = _OverlayCard();
	for (const BString& failed : fSettings.overlayFails) {
		if (failed == card)
			return 0;
	}
	for (const BString& works : fSettings.overlayWorks) {
		if (works == card)
			return 1;
	}
	// the drivers seen to show it, and those seen not to
	static const char* kWorks[] = { "Tegra" };
	static const char* kFails[] = { NULL };
	for (const char* name : kWorks) {
		if (name != NULL && card.FindFirst(name) >= 0)
			return 1;
	}
	for (const char* name : kFails) {
		if (name != NULL && card.FindFirst(name) >= 0)
			return 0;
	}
#if defined(__arm__) || defined(__aarch64__)
	// a slow processor needs it, and the ARM boards have shown it
	return 1;
#else
	return card.IsEmpty() ? 0 : 2;
#endif
}


/*!	What was seen on this graphics card, for the next times. */
void
CinemaWindow::_RememberOverlay(bool works)
{
	BString card = _OverlayCard();
	if (card.IsEmpty())
		return;
	Settings saved = Settings::Load();
	for (std::vector<BString>* list : { &saved.overlayWorks,
			&saved.overlayFails }) {
		for (size_t i = 0; i < list->size(); i++) {
			if ((*list)[i] == card) {
				list->erase(list->begin() + i);
				break;
			}
		}
	}
	(works ? saved.overlayWorks : saved.overlayFails).push_back(card);
	saved.Save();
	fSettings.overlayWorks = saved.overlayWorks;
	fSettings.overlayFails = saved.overlayFails;
}


/*!	The head of the videos of a channel: what is known of it now; the rest
	comes with its videos.
*/
void
CinemaWindow::_SetChannelHeader(const char* name, const char* id,
	const char* provider)
{
	BString source(provider != NULL && provider[0] != '\0' ? provider
		: fProvider.String());
	// the channels of people: not TV lists, folders or the favorites
	if ((source != "youtube" && source != "peertube" && source != "archive")
		|| strcmp(id, "series") == 0) {
		fGrid->ClearChannelHeader();
		return;
	}
	ChannelHeader header;
	if (fGrid->HasChannelHeader() && fGrid->Header().id == id)
		header = fGrid->Header();
	header.provider = source;
	header.id = id;
	if (header.name.IsEmpty() || header.id != fGrid->Header().id)
		header.name = name;
	MediaItem channel;
	channel.provider = source;
	channel.channelId = id;
	header.followed = _Followed(channel);
	header.favorite = Favorites::Has(id);
	fGrid->SetChannelHeader(header);
}


/*!	1 when the channel of the item is one of the tabs, 0 when it is not, -1
	when it has none that can be one (a file of the library, TV).
*/
int32
CinemaWindow::_Followed(const MediaItem& item) const
{
	if (item.channelId.IsEmpty() || item.provider == "library"
		|| item.provider == "local" || item.kind == "live")
		return -1;
	for (const Category& category : fCategories) {
		if (category.channel == item.channelId)
			return 1;
	}
	return 0;
}


void
CinemaWindow::_HomeChanged()
{
	// the buttons of the page and of the channel shown follow the tabs
	if (fGrid->HasChannelHeader()) {
		ChannelHeader header = fGrid->Header();
		MediaItem channel;
		channel.provider = header.provider;
		channel.channelId = header.id;
		int32 followed = _Followed(channel);
		if (followed != header.followed) {
			header.followed = followed;
			fGrid->SetChannelHeader(header);
		}
	}
	if (fDetailsShown) {
		MediaItem shown = fDetail->Item();
		fDetail->SetFollowed(shown.channelId.String(), _Followed(shown));
	}
	fHomeLoaded = false;
	fHomeRequests.clear();
	fHomeQueue.clear();
	fHomeActive = 0;
	if (fCard == 2 && !fDetailsShown)
		_ShowHome();
	else if (fDetailsShown && fDetailsReturn == 2)
		fHomeStale = true;
}


void
CinemaWindow::_RemoveCategory(int32 index)
{
	if (index < 0 || index >= (int32)fCategories.size())
		return;
	fCategories.erase(fCategories.begin() + index);
	SaveCategories(fCategories);

	// what is shown stays: only the tab goes
	int32 selected = fCategoryBar->Selected();
	int32 tab = fCategoryBar->CategoryTab(index);
	fCategoryBar->SetCategories(fCategories);
	if (selected == tab)
		fCategoryBar->SetSelected(-1);
	else if (selected > tab)
		fCategoryBar->SetSelected(selected - 1);
	_HomeChanged();
}


std::vector<MediaItem>
CinemaWindow::_Videos(BMessage* message)
{
	std::map<BString, Positions::Position> positions = Positions::All();
	std::vector<MediaItem> videos;
	const char* id;
	for (int32 i = 0; message->FindString("id", i, &id) == B_OK; i++) {
		MediaItem video;
		video.id = id;
		video.provider = message->GetString("provider", i, fProvider);
		video.kind = message->GetString("kind", i, "video");
		video.title = message->GetString("title", i, "");
		video.description = message->GetString("description", i, "");
		video.duration = message->GetInt32("duration", i, 0);
		video.channel = message->GetString("channel", i, "");
		video.channelId = message->GetString("channel id", i, "");
		video.year = message->GetInt32("year", i, 0);
		video.date = message->GetString("date", i, "");
		video.views = message->GetInt64("views", i, 0);
		video.rating = message->GetDouble("rating", i, 0);
		video.poster = message->GetString("poster", i, "");
		video.thumbnail = message->GetString("thumbnail", i, "");
		fItemProviders[video.id] = video.provider;
		auto position = positions.find(video.id);
		if (position != positions.end()) {
			video.watched = position->second.watched;
			if (position->second.duration > 0)
				video.resume = (int32)(position->second.position * 1000
					/ position->second.duration);
		}
		if (!video.channelId.IsEmpty())
			fChannelProviders[video.channelId] = video.provider;
		video.downloaded = fDownloaded.count(video.id) > 0;
		video.favorite = Favorites::Has(video.id);
		auto downloading = fDownloading.find(video.id);
		if (downloading != fDownloading.end())
			video.progress = downloading->second.GetInt32("percent", 0);
		video.picture = NULL;
		videos.push_back(video);
	}
	return videos;
}


void
CinemaWindow::_Thumbnails(const std::vector<MediaItem>& videos,
	int32 generation, float largest)
{
	BMessage thumbnails(kMsgThumbnails);
	thumbnails.AddInt32("generation", generation);
	thumbnails.AddFloat("largest", largest);
	for (const MediaItem& video : videos) {
		thumbnails.AddString("id", video.id);
		thumbnails.AddString("url", video.thumbnail);
	}
	fWorker->PostMessage(&thumbnails);
}


void
CinemaWindow::_ShowResults(BMessage* message)
{
	const char* aboutName;
	if (fGrid->HasChannelHeader()
		&& message->FindString("about name", &aboutName) == B_OK) {
		ChannelHeader header = fGrid->Header();
		if (aboutName[0] != '\0')
			header.name = aboutName;
		header.handle = message->GetString("about handle", "");
		header.followers = message->GetInt64("about followers", 0);
		header.description = message->GetString("about description", "");
		header.avatarUrl = message->GetString("about avatar", "");
		fGrid->SetChannelHeader(header);
		if (!header.avatarUrl.IsEmpty()) {
			BMessage fetch(kMsgFetchPicture);
			fetch.AddString("id", BString("avatar:") << header.id);
			fetch.AddString("url", header.avatarUrl);
			fWorker->PostMessage(&fetch);
		}
	}
	std::vector<MediaItem> videos = _Videos(message);
	if (videos.empty()) {
		fGrid->SetMessage(B_TRANSLATE("No videos found"));
		return;
	}
	fGrid->SetVideos(videos);
	_Thumbnails(videos, fGeneration);

	// the first one is chosen already, or, in the episodes of a series,
	// the next one to watch
	fWanted = videos[0].id;
	if (fChannelId.StartsWith("series:")) {
		for (const MediaItem& video : videos) {
			if (!video.watched) {
				fWanted = video.id;
				fGrid->Select(video.id);
				break;
			}
		}
	}
	_PrefetchWanted();
}


void
CinemaWindow::_ShowHome()
{
	_CloseDetails();
	fCategoryBar->SetSelected(fCategoryBar->HomeIndex());
	fChannelId = "";
	if (!fHomeLoaded)
		_LoadHome();
	fCards->SetVisibleItem(2);
	fCard = 2;
	fHome->MakeFirstFocus();
}


void
CinemaWindow::_FocusCategoryBar()
{
	fCategoryBar->FocusSelected();
}


void
CinemaWindow::_FocusContent()
{
	if (fPlayerShown) {
		fPlayerView->MakeFocus(true);
		return;
	}
	if (fDetailsShown) {
		fDetail->MakeFocus(true);
		return;
	}
	if (fCard == 2) {
		fHome->MakeLastFocus();
		return;
	}
	fGrid->MakeFocus(true);
}


void
CinemaWindow::_ToggleCategoryFocus()
{
	if (fCategoryBar->IsFocus())
		_FocusContent();
	else
		_FocusCategoryBar();
}


void
CinemaWindow::_LoadHome()
{
	fHomeStale = false;
	fHomeRequests.clear();
	fHomeQueue.clear();
	fHomeActive = 0;
	std::vector<ShelfSpec> shelves;

	std::vector<MediaItem> history = History::Load(fWorker->Cache());
	if (!history.empty()) {
		ShelfSpec spec;
		spec.title = B_TRANSLATE("Continue watching");
		spec.videos = history;
		shelves.push_back(spec);
	}

	fDownloaded.clear();
	// the latest on the shelf, with their pictures: all of them would hold
	// the window on a slow machine with many files
	static const int32 kShelfDownloads = 24;
	std::vector<MediaItem> downloads = Downloads::Load(&fDownloaded,
		kShelfDownloads);
	if (downloads.size() > (size_t)kShelfDownloads)
		downloads.resize(kShelfDownloads);
	if (!downloads.empty()) {
		ShelfSpec spec;
		spec.title = B_TRANSLATE("Downloaded");
		spec.seeAll = BMessage(kMsgShowDownloads);
		spec.videos = downloads;
		shelves.push_back(spec);
	}

	for (size_t i = 0; i < fCategories.size(); i++) {
		ShelfSpec spec;
		spec.title = fCategories[i].name;
		BMessage seeAll(kMsgCategory);
		seeAll.AddInt32("index", i);
		spec.seeAll = seeAll;
		spec.message = B_TRANSLATE("Looking for the videos"
			B_UTF8_ELLIPSIS);
		int32 shelf = shelves.size();
		shelves.push_back(spec);
		int32 generation = --fHomeGeneration;
		fHomeRequests[generation] = shelf;
		fHomeQueue.push_back({generation, shelf, (int32)i});
	}

	if (shelves.empty())
		fHome->SetMessage(B_TRANSLATE("Type what to look for at the top "
			"right"));
	else
		fHome->SetShelves(shelves);
	fHomeLoaded = true;
	_StartHomeRequests();
}


/*!	The shelves are filled a few at a time: a search costs a yt-dlp, and
	they would all run together otherwise.
*/
void
CinemaWindow::_StartHomeRequests()
{
	static const int32 kAtOnce = 2;
	while (fHomeActive < kAtOnce && !fHomeQueue.empty()) {
		HomeRequest request = fHomeQueue.front();
		fHomeQueue.pop_front();
		if (fHomeRequests.find(request.generation) == fHomeRequests.end()
			|| request.category < 0
			|| request.category >= (int32)fCategories.size())
			continue;
		const Category& category = fCategories[request.category];
		if (category.IsChannel())
			fHelper->Channel(request.generation, kResultCount,
				category.channel, category.provider.String());
		else
			fHelper->Search(request.generation, kResultCount,
				category.query, category.provider.String());
		fHomeActive++;
	}
}


void
CinemaWindow::_HomeResults(BMessage* message, int32 shelf)
{
	std::vector<MediaItem> videos = _Videos(message);
	if (videos.empty())
		fHome->SetShelfMessage(shelf, B_TRANSLATE("No videos found"));
	else {
		fHome->SetShelfVideos(shelf, videos);
		_Thumbnails(videos, message->GetInt32("generation", -1),
			Pictures::kShelf);
	}
	if (fHomeActive > 0)
		fHomeActive--;
	_StartHomeRequests();
}


void
CinemaWindow::_HomeFailed(BMessage* message, int32 shelf)
{
	fHome->SetShelfMessage(shelf, B_TRANSLATE("YouTube could not be reached: "
		"is the network on?"));
	(void)message;
	if (fHomeActive > 0)
		fHomeActive--;
	_StartHomeRequests();
}


/*!	The page of one video: what the grid already knows, while the helper
	looks for the rest.
*/
void
CinemaWindow::_ShowDetails(BMessage* message)
{
	MediaItem item;
	item.provider = message->GetString("provider", fProvider);
	item.kind = message->GetString("kind", "video");
	item.id = message->GetString("id", "");
	fItemProviders[item.id] = item.provider;
	item.title = message->GetString("title", "");
	item.description = message->GetString("description", "");
	item.channel = message->GetString("channel", "");
	item.channelId = message->GetString("channel id", "");
	item.duration = message->GetInt32("duration", 0);
	item.year = message->GetInt32("year", 0);
	item.date = message->GetString("date", "");
	item.views = message->GetInt64("views", 0);
	item.rating = message->GetDouble("rating", 0);
	item.poster = message->GetString("poster", "");
	item.thumbnail = message->GetString("thumbnail", "");
	item.path = message->GetString("path", "");
	item.downloaded = message->GetBool("downloaded", false);
	item.progress = message->GetInt32("progress", -1);
	item.picture = NULL;
	item.favorite = Favorites::Has(item.id.String());
	// its file, for the Trash and to play it without the network
	auto file = fDownloaded.find(item.id);
	if (item.path.IsEmpty() && file != fDownloaded.end()
		&& BEntry(file->second.String()).Exists()) {
		item.path = file->second;
		item.downloaded = true;
	}
	if (item.id.IsEmpty())
		return;

	if (!fDetailsShown)
		fDetailsReturn = fCard;
	fDetailsId = item.id;
	fDetailsShown = true;
	fDetail->SetItem(item);
	fDetail->SetFollowed(item.channelId.String(), _Followed(item));
	fDetail->SetLoading(true);

	// the small picture at once, while the big one is on its way
	BString path(fWorker->Cache());
	path << "/" << PictureName(item.id) << ".jpg";
	if (BEntry(path.String()).Exists())
		fDetail->SetPicture(Pictures::Load(path.String(),
			Pictures::kDetail));

	fCards->SetVisibleItem(1);
	fCard = 1;
	fDetail->MakeFocus(true);
	fHelper->Details(item.provider.String(), item.id.String(),
		item.kind.String());
}


/*!	What the helper found about the video on the detail page: the rest of
	its description, when it was published, its big picture.
*/
void
CinemaWindow::_Details(BMessage* message)
{
	BString id = message->GetString("id", "");
	if (!fDetailsShown || id != fDetailsId)
		return;
	if (!message->GetBool("found", false)) {
		fDetail->SetLoading(false);
		return;
	}

	MediaItem item = fDetail->Item();
	item.title = message->GetString("title", item.title);
	item.description = message->GetString("description", item.description);
	item.channel = message->GetString("channel", item.channel);
	item.channelId = message->GetString("channel id", item.channelId);
	item.duration = message->GetInt32("duration", item.duration);
	item.year = message->GetInt32("year", item.year);
	item.date = message->GetString("date", item.date);
	item.views = message->GetInt64("views", item.views);
	item.rating = message->GetDouble("rating", item.rating);
	item.poster = message->GetString("poster", item.poster);
	item.thumbnail = message->GetString("thumbnail", item.thumbnail);
	fDetail->SetItem(item);
	fDetail->SetFollowed(item.channelId.String(), _Followed(item));
	fDetail->SetLoading(false);

	if (!item.poster.IsEmpty()) {
		BMessage fetch(kMsgFetchPicture);
		fetch.AddString("id", item.id);
		fetch.AddString("url", item.poster);
		fWorker->PostMessage(&fetch);
	}
}


void
CinemaWindow::_Picture(BMessage* message)
{
	BString id = message->GetString("id", "");
	if (id.StartsWith("avatar:")) {
		if (fGrid->HasChannelHeader()
			&& id == BString("avatar:") << fGrid->Header().id) {
			fGrid->SetChannelAvatar(Pictures::Load(
				message->GetString("path", ""), S(96)));
		}
		return;
	}
	if (!fDetailsShown || id != fDetailsId)
		return;
	BBitmap* picture = Pictures::Load(message->GetString("path", ""),
		Pictures::kDetail);
	if (picture != NULL)
		fDetail->SetPicture(picture);
}


void
CinemaWindow::_CloseDetails()
{
	if (!fDetailsShown)
		return;
	fDetailsShown = false;
	fCards->SetVisibleItem(fDetailsReturn);
	fCard = fDetailsReturn;
	fDetail->SetItem(MediaItem());
	if (fDetailsReturn == 2 && fHomeStale)
		_LoadHome();
	if (fDetailsReturn == 2) {
		if (!fHome->MoveFocus(fDetailsId.String(), 0))
			fHome->MakeFirstFocus();
	} else
		fGrid->MakeFocus(true);
	fDetailsId = "";
}


bool
CinemaWindow::_Ready(const BString& id) const
{
	auto found = fStreams.find(id);
	return found != fStreams.end()
		&& system_time() - found->second.found < kStreamLifetime;
}


void
CinemaWindow::_Find(const BString& id)
{
	if (fFinding.count(id) != 0)
		return;
	fFinding.insert(id);
	fHelper->Find(id.String(), _ProviderOf(id).String());
}


/*!	Looks for the file of the chosen video ahead, one at a time, so that it
	plays as soon as it is clicked.
*/
void
CinemaWindow::_PrefetchWanted()
{
	if (fWanted.IsEmpty() || _Ready(fWanted) || !fFinding.empty()
		|| fDownloaded.count(fWanted) > 0)
		return;
	_Find(fWanted);
}


void
CinemaWindow::_Play(const char* url, const char* title, const char* id)
{
	// a video still being looked for is not wanted any more: its address
	// would close this one when it comes
	if (!fWaiting.IsEmpty()) {
		fWaiting = "";
		fGrid->SetBusy(NULL);
		fHome->ClearBusy();
	}
	if (!fSettings.playInCinema) {
		_PlayInMediaPlayer(url, title);
		return;
	}
	// in the window, from where it was left
	_OpenPlayer(url, title, id != NULL && id[0] != '\0' ? id : url);
}


void
CinemaWindow::_PlayInMediaPlayer(const char* address, const char* title,
	bool followed)
{
	// a live stream as it is: MediaPlayer opens it by itself
	BString url = LiveAddress::Url(address);
	// YouTube's addresses redirect, which MediaPlayer does not follow: the
	// worker follows them first
	if (!followed && url.FindFirst(".googlevideo.com/") >= 0) {
		BMessage follow(kMsgFollow);
		follow.AddString("title", title);
		follow.AddString("url", url);
		fWorker->PostMessage(&follow);
		return;
	}
	const char* argv[] = { url.String() };
	status_t status = be_roster->Launch(kMediaPlayer, 1, argv);
	if (status != B_OK && status != B_ALREADY_RUNNING) {
		BString text(B_TRANSLATE("MediaPlayer could not be started: %error%"));
		text.ReplaceFirst("%error%", strerror(status));
		(new BAlert(title, text.String(), B_TRANSLATE("OK")))->Go(NULL);
	}
}


void
CinemaWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgFullScreen:
			_SetFullScreen(!fFullScreen);
			break;

		case kMsgFocusContent:
			_FocusContent();
			break;

		case kMsgHome:
			_ShowHome();
			break;
		case kMsgShelfMove:
			fHome->MoveFocus(message->GetString("id", ""),
				message->GetInt32("delta", 0));
			break;
		case kMsgShelfFocus:
			fHome->RememberFocus();
			fHome->Invalidate();
			break;
		case kMsgCategory:
			_ShowCategory(message->GetInt32("index", -1));
			break;
		case kMsgChannel:
			_ShowChannel(message->GetString("channel", ""),
				message->GetString("channel id", ""));
			break;
		case kMsgPlayerStruggling:
		{
			// of a video closed since: nothing to do with this one
			if (message->GetInt32("session", -1) != fPlayer->Session())
				break;
			_LowerQuality(message->GetInt32("height", 0));
			break;
		}
		case kMsgAddTorrent:
			_AddTorrent(message->GetString("source", ""));
			break;
		case kMsgToggleOverlay:
		{
			// the O key: the overlay used shows nothing, or the one not used
			// is wanted; kept for this graphics card, in the automatic way
			bool works = !fPlayer->IsOverlayAllowed();
			_RememberOverlay(works);
			if (fSettings.overlayChoice != kOverlayAutomatic) {
				Settings saved = Settings::Load();
				saved.overlayChoice = kOverlayAutomatic;
				saved.Save();
				fSettings.overlayChoice = kOverlayAutomatic;
			}
			_ApplyOverlay();
			break;
		}
		case kMsgAskOverlay:
			// a few seconds of the overlay: is the video there?
			if (fAskOverlay && fPlayerShown && fPlayer->Overlay() != NULL)
				fPlayerView->SetOverlayQuestion(_OverlayCard().String());
			break;
		case kMsgOverlayAnswer:
			_RememberOverlay(message->GetBool("works", true));
			fPlayerView->SetOverlayQuestion(NULL);
			_ApplyOverlay();
			break;
		case kMsgFollowChannel:
		{
			BString id = message->GetString("channel id", "");
			if (message->GetBool("follow", true)) {
				const char* provider = message->GetString("provider", "");
				if (provider[0] != '\0')
					fChannelProviders[id] = provider;
				_AddChannel(message->GetString("channel", ""), id.String());
			} else {
				for (size_t i = 0; i < fCategories.size(); i++) {
					if (fCategories[i].channel == id) {
						_RemoveCategory(i);
						break;
					}
				}
			}
			break;
		}
		case kMsgAddChannel:
			// the one given, or the one shown
			_AddChannel(message->GetString("channel", fChannelName),
				message->GetString("channel id", fChannelId));
			break;
		case kMsgRemoveCategory:
			_RemoveCategory(message->GetInt32("index", -1));
			break;
		case kMsgSearchField:
		{
			BString query(fSearchField->Text());
			query.Trim();
			if (query.IsEmpty())
				break;
			// a torrent to download rather than something to look for
			if (query.StartsWith("magnet:")
				|| (query.IStartsWith("http") && query.IEndsWith(".torrent"))) {
				// the field keeps its text: changing it now could pull it from
				// under a selection the mouse is still making
				_AddTorrent(query.String());
				break;
			}
			// "@peertube words" searches another source than the menu's
			BString provider = fSearchProvider;
			if (query.ByteAt(0) == '@') {
				int32 space = query.FindFirst(' ');
				if (space > 1) {
					query.CopyInto(provider, 1, space - 1);
					query.Remove(0, space);
					query.Trim();
				}
			}
			fCategoryBar->SetSelected(-1);
			fChannelId = "";
			_Search(query.String(), provider.String());
			fGrid->MakeFocus(true);
			break;
		}
		case kMsgSearchSource:
			fSearchProvider = message->GetString("provider", fProvider);
			fSearchField->MakeFocus(true);
			break;
		case B_SIMPLE_DATA:
		case B_REFS_RECEIVED:
			_Dropped(message);
			break;
		case kMsgProviders:
			_SetProviders(message);
			break;
		case kMsgResults:
		{
			int32 generation = message->GetInt32("generation", -1);
			auto home = fHomeRequests.find(generation);
			if (home != fHomeRequests.end())
				_HomeResults(message, home->second);
			else if (generation == fGeneration)
				_ShowResults(message);
			break;
		}
		case kMsgSearchFailed:
		{
			int32 generation = message->GetInt32("generation", -1);
			auto home = fHomeRequests.find(generation);
			if (home != fHomeRequests.end())
				_HomeFailed(message, home->second);
			else if (generation == fGeneration) {
				fGrid->SetMessage(B_TRANSLATE("YouTube could not be reached: "
					"is the network on?"));
			}
			break;
		}
		case kMsgThumbnail:
		{
			int32 generation = message->GetInt32("generation", -1);
			// read and made small by the worker already, as a bitmap of
			// its own memory: made one of app_server here
			BBitmap* read = NULL;
			message->FindPointer("picture", (void**)&read);
			BBitmap* picture = Pictures::ForScreen(read);
			auto home = fHomeRequests.find(generation);
			if (home != fHomeRequests.end())
				fHome->SetPicture(message->GetString("id", ""), picture);
			else if (generation == fGeneration)
				fGrid->SetPicture(message->GetString("id", ""), picture);
			else
				delete picture;
			break;
		}

		case kMsgShowDetails:
			_ShowDetails(message);
			break;
		case kMsgDetails:
			_Details(message);
			break;
		case kMsgPicture:
			_Picture(message);
			break;
		case kMsgCloseDetails:
			_CloseDetails();
			break;

		case kMsgSelected:
			if (_ShowingDownloads())
				break;
			// worth looking for, if it stays chosen a moment
			fWanted = message->GetString("id", "");
			delete fPrefetchRunner;
			fPrefetchRunner = new BMessageRunner(BMessenger(this),
				new BMessage(kMsgPrefetch), kPrefetchDelay, 1);
			break;
		case kMsgPrefetch:
			_PrefetchWanted();
			break;

		case kMsgShowDownloads:
			_ShowDownloads();
			break;
		case kMsgDownload:
			_Download(message);
			break;
		case kMsgDownloadProgress:
		{
			BString id = message->GetString("id", "");
			auto downloading = fDownloading.find(id);
			if (downloading == fDownloading.end())
				break;
			int32 percent = message->GetInt32("percent", 0);
			BMessage& download = downloading->second;
			download.SetInt32("percent", percent);
			for (const char* field : { "done", "total", "speed", "eta" })
				download.SetString(field, message->GetString(field, ""));
			download.SetInt32("peers", message->GetInt32("peers", -1));
			fGrid->SetProgress(id, percent);
			fGrid->SetStatus(id, _DownloadStatus(download).String());
			fHome->SetProgress(id, percent);
			fDetail->SetProgress(id.String(), percent);
			break;
		}
		case kMsgShowSettings:
			_ShowSettings();
			break;
		case kMsgShowAbout:
			_ShowAbout();
			break;
		case kMsgSettingsClosed:
			fSettingsWindow = BMessenger();
			break;
		case kMsgSettingsChanged:
		{
			int32 height = fSettings.videoHeight;
			fSettings = Settings::Load();
			// at once, on the video playing too
			fPlayer->SetNormalized(fSettings.evenVolume);
			_ApplyOverlay();
			if (!fSettings.searchSource.IsEmpty())
				fSearchProvider = fSettings.searchSource;
			_UpdateSources();
			if (fSettings.videoHeight != height) {
				// the files found before are of the other size
				fHelper->SetHeight(fSettings.videoHeight);
				fStreams.clear();
			}
			break;
		}
		case kMsgAddFolder:
			_AddFolder(message->GetString("path", ""));
			_SendFolders();
			break;
		case kMsgRemoveFolder:
		{
			BString path = message->GetString("path", "");
			for (size_t i = 0; i < fCategories.size(); i++) {
				if (fCategories[i].provider == "local"
					&& fCategories[i].channel == path) {
					_RemoveCategory(i);
					break;
				}
			}
			_SendFolders();
			break;
		}
		case kMsgPlayerFavorite:
		{
			if (fPlayingItem.id != fPlayingId || fPlayingId.IsEmpty())
				break;
			// the fields Favorites::Toggle() keeps, as the grid sends them
			const MediaItem& item = fPlayingItem;
			BMessage favorite(kMsgToggleFavorite);
			favorite.AddString("id", item.id);
			favorite.AddString("provider", item.provider);
			favorite.AddString("kind", item.kind);
			favorite.AddString("title", item.title);
			favorite.AddString("description", item.description);
			favorite.AddString("channel", item.channel);
			favorite.AddString("channel id", item.channelId);
			favorite.AddInt32("duration", item.duration);
			favorite.AddInt32("year", item.year);
			favorite.AddString("poster", item.poster);
			favorite.AddString("thumbnail", item.thumbnail);
			_ToggleFavorite(&favorite);
			break;
		}
		case kMsgToggleFavorite:
			_ToggleFavorite(message);
			break;
		case kMsgAddTvList:
			_AddTvList(message->GetString("address", ""),
				message->GetString("name", NULL));
			_SendFolders();
			break;
		case kMsgRemoveTvList:
		{
			BString address = message->GetString("address", "");
			for (size_t i = 0; i < fCategories.size(); i++) {
				if (fCategories[i].provider == "iptv"
					&& fCategories[i].channel == address) {
					_RemoveCategory(i);
					break;
				}
			}
			_SendFolders();
			break;
		}
		case kMsgCheckTmdb:
			fHelper->CheckTmdb();
			break;
		case kMsgTmdbChecked:
			fSettingsWindow.SendMessage(message);
			break;

		case kMsgPlayerOpened:
			// of a video closed since: nothing to do with this one
			if (message->GetInt32("session", -1) != fPlayer->Session())
				break;
			fPlayerView->SetOpened(true);
			fPlayerView->SetOverlay(fPlayer->Overlay());
			if (fAskOverlay && fPlayer->Overlay() != NULL) {
				// asked once the video had time to show
				delete fOverlayAsker;
				BMessage ask(kMsgAskOverlay);
				fOverlayAsker = new BMessageRunner(BMessenger(this), &ask,
					4000000, 1);
			}
			fPlayerView->RefreshSubtitles();
			if (fFullScreen)
				_DirectSync();
			break;
		case kMsgSubtitles:
		{
			BString id = message->GetString("id", "");
			if (!fPlayerShown || id != fPlayingId)
				break;
			BMessage track;
			for (int32 i = 0;
				message->FindMessage("track", i, &track) == B_OK; i++) {
				fPlayer->AddSubtitleTrack(
					track.GetString("language", ""),
					track.GetString("name", ""),
					track.GetBool("automatic", false));
			}
			_RequestSelectedSubtitle();
			fPlayerView->RefreshSubtitles();
			break;
		}
		case kMsgSubtitle:
		{
			BString id = message->GetString("id", "");
			if (!fPlayerShown || id != fPlayingId)
				break;
			if (message->GetBool("failed", false)) {
				// not got: asked again a little later, twice at most, and
				// again whenever it is chosen after that
				const char* language = message->GetString("language", "");
				bool automatic = message->GetBool("automatic", false);
				BString key(fPlayingId);
				key << "\n" << language << "\n" << (automatic ? "1" : "0");
				fSubtitlesAsked.erase(key);
				if (fSubtitleRetries[key]++ < 2) {
					BMessage retry(kMsgSubtitleRequest);
					retry.AddString("language", language);
					retry.AddBool("automatic", automatic);
					BMessageRunner::StartSending(BMessenger(this), &retry,
						3000000, 1);
				}
				break;
			}
			fPlayer->LoadSubtitleText(
				message->GetString("language", ""),
				message->GetBool("automatic", false),
				message->GetString("text", ""));
			fPlayerView->RefreshSubtitles();
			break;
		}
		case kMsgSubtitleChosen:
		{
			// the next videos start with the same language, or none
			BString choice = fPlayer->SubtitleChoice();
			fPlayer->SetSubtitlePreference(choice.String());
			fSettings = Settings::Load();
			if (fSettings.subtitleLanguage != choice) {
				fSettings.subtitleLanguage = choice;
				fSettings.Save();
			}
			break;
		}
		case kMsgSubtitleRequest:
			_RequestSubtitle(message->GetString("language", ""),
				message->GetBool("automatic", false));
			break;
		case kMsgPlayerFrame:
			// to the view at once, in this thread: PostMessage() to a handler
			// takes the lock of the list of loopers, which the application
			// holds while it closes the windows, waiting for this one
			fPlayerView->MessageReceived(message);
			if (fDirectActive && !fDirectPaused)
				_DirectDraw();
			break;
		case kMsgDirectDraw:
			_DirectDraw();
			break;
		case kMsgDirectFallback:
			if (fDirectActive) {
				fDirectActive = false;
				fDirectFailed = true;
				fPlayerView->SetDirect(false);
				delete fDirectRunner;
				fDirectRunner = NULL;
				if (BDirectWindow::IsFullScreen())
					BDirectWindow::SetFullScreen(false);
				if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
					fprintf(stderr, fDirectConnected
						? "direct: buffer unsupported, fallback\n"
						: "direct: no connection, fallback\n");
				_DirectSync();
			}
			break;
		case kMsgPlayerReopen:
		{
			// only the last of the steps asked one after the other, and
			// for the video playing
			if (message->GetInt32("session", -1) != fPlayer->Session())
				break;
			if (!fPlayerShown
				|| MessageQueue()->FindMessage(kMsgPlayerReopen, 0) != NULL)
				break;
			fPlayerView->ClearOverlay();
			// the same video, with its subtitles
			fPlayer->Reopen(message->GetInt64("time", 0));
			break;
		}
		case kMsgPlayerSave:
			_SavePosition();
			break;
		case kMsgPlayerEnded:
		{
			// of a video closed since: nothing to do with this one
			if (message->GetInt32("session", -1) != fPlayer->Session())
				break;
			// watched to the end
			if (fPlayer->Duration() > 0)
				Positions::Set(fPlayingId, fPlayer->Duration(),
					fPlayer->Duration());
			// an episode: the next one of the series, if it is in the list
			MediaItem next;
			bool episode = fPlayingKind == "episode"
				&& fGrid->NextOf(fPlayingId, next) && next.kind == "episode";
			BString finished = fPlayingId;
			_ClosePlayer(false);
			if (episode) {
				fGrid->Select(next.id);
				BMessage play(kMsgPlay);
				play.AddString("id", next.id);
				play.AddString("title", next.title);
				play.AddString("provider", next.provider);
				play.AddString("kind", next.kind);
				play.AddString("channel", next.channel);
				play.AddString("channel id", next.channelId);
				play.AddInt32("duration", next.duration);
				// a file of the library: played from where it is
				if (next.provider == "library" || next.provider == "local")
					play.AddString("path", next.id);
				PostMessage(&play);
			}
			break;
		}
		case kMsgPlayerError:
		{
			// of a video closed since: nothing to do with this one
			if (message->GetInt32("session", -1) != fPlayer->Session())
				break;
			BString address = fPlayingAddress;
			BString title = fPlayingTitle;
			BString id = fPlayingId;
			auto stream = fStreams.find(id);
			if (stream != fStreams.end() && stream->second.url == address
				&& fRetried != id) {
				// an address found before, which YouTube may not send any
				// more, on another network say: found again, once
				fStreams.erase(stream);
				fRetried = id;
				// from where it stopped, if it played
				_ClosePlayer(fPlayer->Position() > 0);
				fWaiting = id;
				fWaitingTitle = title;
				fGrid->SetBusy(title.String());
				fHome->SetBusy(id.String(), title.String());
				_Find(id);
				break;
			}
			// what the Media Kit cannot open here, MediaPlayer may
			_ClosePlayer(false);
			_PlayInMediaPlayer(address.String(), title.String());
			break;
		}
		case kMsgPlayInMediaPlayer:
		{
			BString address = fPlayingAddress;
			BString title = fPlayingTitle;
			_ClosePlayer();
			_PlayInMediaPlayer(address.String(), title.String());
			break;
		}
		case kMsgCancelDownload:
			fHelper->Cancel(message->GetString("id", ""));
			break;
		case kMsgDownloadCancelled:
			_DownloadCancelled(message);
			break;
		case kMsgDownloaded:
			_Downloaded(message);
			break;
		case kMsgDownloadFailed:
			_DownloadFailed(message);
			break;
		case kMsgTrash:
			_Trash(message);
			break;

		case kMsgPlay:
		{
			BString id = message->GetString("id", "");
			BString title = message->GetString("title", "");
			fRetried = "";
			const char* provider = message->GetString("provider", NULL);
			if (provider != NULL && provider[0] != '\0')
				fItemProviders[id] = provider;
			fNextKind = message->GetString("kind", "video");
			if (strcmp(message->GetString("kind", ""), "series") == 0) {
				// not something to play: its episodes
				fChannelProviders[id] = provider != NULL ? provider : "library";
				_ShowChannel(title.String(), id.String());
				break;
			}
			if (strcmp(message->GetString("kind", ""), "channel") == 0) {
				// a channel given a star: its latest videos
				if (provider != NULL && provider[0] != '\0')
					fChannelProviders[id] = provider;
				_ShowChannel(title.String(), id.String());
				break;
			}
			const char* path = message->GetString("path", NULL);
			auto downloaded = fDownloaded.find(id);
			if (path == NULL && downloaded != fDownloaded.end()
				&& BEntry(downloaded->second.String()).Exists())
				path = downloaded->second.String();

			MediaItem recent;
			recent.id = id;
			recent.title = title;
			recent.provider = message->GetString("provider", fProvider);
			recent.kind = message->GetString("kind", "video");
			recent.channel = message->GetString("channel", "");
			recent.channelId = message->GetString("channel id", "");
			recent.duration = message->GetInt32("duration", 0);
			recent.path = path != NULL ? path : "";
			recent.downloaded = path != NULL;
			History::Remember(recent, fWorker->Cache());
			fHomeLoaded = false;
			// for the star of the player
			fPlayingItem = recent;
			fPlayingItem.description = message->GetString("description", "");
			fPlayingItem.year = message->GetInt32("year", 0);
			fPlayingItem.poster = message->GetString("poster", "");
			fPlayingItem.thumbnail = message->GetString("thumbnail", "");

			if (path != NULL) {
				// downloaded: no need for the network
				_Play(path, title.String(), id.String());
				break;
			}
			if (_Ready(id)) {
				_Play(fStreams[id].url.String(), title.String(), id.String());
				break;
			}
			fWaiting = id;
			fWaitingTitle = title;
			fGrid->SetBusy(title.String());
			fHome->SetBusy(id.String(), title.String());
			_Find(id);
			break;
		}
		case kMsgFound:
		{
			// the player of Cinema follows the redirects of YouTube by
			// itself: only MediaPlayer and the Media Kit need them followed
			// first, which takes a second or two
			BMessage follow(*message);
			const char* mediaKit = getenv("CINEMA_MEDIA_KIT");
			if (fSettings.playInCinema
				&& (mediaKit == NULL || strcmp(mediaKit, "0") == 0))
				follow.what = kMsgStreamReady;
			else
				follow.what = kMsgFollow;
			if (follow.what == kMsgStreamReady)
				PostMessage(&follow);
			else
				fWorker->PostMessage(&follow);
			break;
		}
		case kMsgFollowed:
			_PlayInMediaPlayer(message->GetString("url", ""),
				message->GetString("title", ""), true);
			break;
		case kMsgStreamReady:
		{
			BString id = message->GetString("id", "");
			fFinding.erase(id);
			// the next videos at the size of the settings again
			if (id == fLowered)
				fHelper->SetHeight(fSettings.videoHeight);
			Stream stream = { message->GetString("url", ""), system_time() };
			fStreams[id] = stream;
			if (id == fWaiting) {
				fWaiting = "";
				fGrid->SetBusy(NULL);
				fHome->ClearBusy();
				_Play(stream.url.String(), fWaitingTitle.String(),
					id.String());
			}
			_PrefetchWanted();
			break;
		}
		case kMsgLost:
		{
			BString id = message->GetString("id", "");
			fFinding.erase(id);
			if (id == fLowered)
				fHelper->SetHeight(fSettings.videoHeight);
			if (id == fWanted)
				fWanted = "";
			if (id == fWaiting) {
				fWaiting = "";
				fGrid->SetBusy(NULL);
				fHome->ClearBusy();
				(new BAlert(fWaitingTitle.String(),
					B_TRANSLATE("This video can't be played here, try another "
						"one."), B_TRANSLATE("OK")))->Go(NULL);
			}
			_PrefetchWanted();
			break;
		}
		case kMsgWatchdogPing:
			// only to show the window takes its messages
			break;
		case kMsgHelperGone:
		{
			fHomeQueue.clear();
			fHomeActive = 0;
			fFinding.clear();
			// a helper that worked and then went, a crash say: another one,
			// unless that keeps happening
			bigtime_t now = system_time();
			if (now - fHelperRestarted > 60000000LL)
				fHelperRestarts = 0;
			if (fHelper->WasReady() && fHelperRestarts < 3
				&& fHelper->Restart() == B_OK) {
				fHelperRestarts++;
				fHelperRestarted = now;
				fHelper->SetHeight(fSettings.videoHeight);
				// the downloads go on from where they were
				BString folder = Downloads::Folder();
				for (const auto& entry : fDownloading)
					fHelper->Download(entry.first.String(), folder.String(),
						entry.second.GetString("provider",
							_ProviderOf(entry.first).String()));
				if (!fWaiting.IsEmpty())
					_Find(fWaiting);
				fHomeLoaded = false;
				if (fCard == 2)
					_LoadHome();
				break;
			}
			fWaiting = "";
			fGrid->SetBusy(NULL);
			fHome->ClearBusy();
			fDetail->SetLoading(false);
			// nothing more comes for the downloads
			std::vector<BString> downloads;
			for (const auto& entry : fDownloading)
				downloads.push_back(entry.first);
			for (const BString& id : downloads) {
				BMessage failed(kMsgDownloadFailed);
				failed.AddString("id", id);
				_DownloadFailed(&failed);
			}
			fHome->FailPending(B_TRANSLATE("Cinema needs yt-dlp and "
				"python3, and cinema-helper.py next to it"));
			fGrid->SetMessage(B_TRANSLATE("Cinema needs yt-dlp and python3, "
				"and cinema-helper.py next to it"));
			break;
		}

		default:
			BDirectWindow::MessageReceived(message);
	}
}


void
CinemaWindow::_SetFullScreen(bool fullScreen)
{
	if (fullScreen == fFullScreen)
		return;
	if (fullScreen)
		fWindowFrame = Frame();
	fFullScreen = fullScreen;
	fDirectFailed = false;
	_DirectSync();
	_UpdateTopBar();
	Activate();
}


/*!	The direct screen is used only full screen and only when no overlay is
	free: the overlay already sends the picture to the screen and leaves
	the controls to the view.
*/
void
CinemaWindow::_DirectSync()
{
	bool wanted = fFullScreen && fPlayerShown && direct_enabled()
		&& !fDirectFailed;
	bool live = fPlayingKind == "live" || LiveAddress::Is(fPlayingAddress);
	bool subtitles = fPlayer->SubtitleCount() > 0;
	if (wanted && fPlayer->Overlay() != NULL && !live && !subtitles) {
		fPlayerView->ClearOverlay();
		fPlayer->DisableOverlay();
	}
	bool direct = wanted && fPlayer->Overlay() == NULL;
	if (direct != fDirectActive) {
		fDirectActive = direct;
		fPlayerView->SetDirect(fDirectActive);
		if (fDirectActive) {
			fDirectStart = system_time();
			fDirectFallbackAsked = false;
			delete fDirectRunner;
			fDirectRunner = NULL;
			if (!fDirectPaused) {
				BMessage draw(kMsgDirectDraw);
				fDirectRunner = new BMessageRunner(BMessenger(this), &draw,
					100000);
			}
			SetLook(B_NO_BORDER_WINDOW_LOOK);
			BRect screen = BScreen(this).Frame();
			MoveTo(screen.LeftTop());
			ResizeTo(screen.Width(), screen.Height());
			SetFlags(Flags() | B_NOT_MOVABLE | B_NOT_RESIZABLE);
			if (BDirectWindow::SetFullScreen(true) != B_OK) {
				fDirectActive = false;
				fDirectFailed = true;
				fPlayerView->SetDirect(false);
				delete fDirectRunner;
				fDirectRunner = NULL;
				if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
					fprintf(stderr, "direct: unavailable\n");
			} else if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
				fprintf(stderr, "direct: on\n");
		} else {
			fDirectPaused = false;
			delete fDirectRunner;
			fDirectRunner = NULL;
			if (BDirectWindow::IsFullScreen())
				BDirectWindow::SetFullScreen(false);
			if (getenv("CINEMA_PLAYER_DEBUG") != NULL)
				fprintf(stderr, "direct: off\n");
		}
	}

	if (fFullScreen) {
		if (fDirectActive)
			return;
		BRect screen = BScreen(this).Frame();
		SetLook(B_NO_BORDER_WINDOW_LOOK);
		MoveTo(screen.LeftTop());
		ResizeTo(screen.Width(), screen.Height());
		SetFlags(Flags() | B_NOT_MOVABLE | B_NOT_RESIZABLE);
	} else {
		SetFlags(Flags() & ~(B_NOT_MOVABLE | B_NOT_RESIZABLE));
		SetLook(B_TITLED_WINDOW_LOOK);
		MoveTo(fWindowFrame.LeftTop());
		ResizeTo(fWindowFrame.Width(), fWindowFrame.Height());
	}
}


void
CinemaWindow::SetDirectPaused(bool paused)
{
	if (!fDirectActive || fDirectPaused == paused)
		return;
	fDirectPaused = paused;
	delete fDirectRunner;
	fDirectRunner = NULL;
	if (!fDirectPaused) {
		BMessage draw(kMsgDirectDraw);
		fDirectRunner = new BMessageRunner(BMessenger(this), &draw,
			100000);
		_DirectDraw();
	}
}


void
CinemaWindow::DirectConnected(direct_buffer_info* info)
{
	BAutolock locker(fDirectLock);
	direct_buffer_state state =
		(direct_buffer_state)(info->buffer_state & B_DIRECT_MODE_MASK);
	if (state == B_DIRECT_START || state == B_DIRECT_MODIFY) {
		fDirectConnected = true;
		fDirectBits = info->bits;
		fDirectRowBytes = info->bytes_per_row;
		fDirectBpp = info->bits_per_pixel;
		fDirectFormat = info->pixel_format;
		fDirectBounds = info->window_bounds;
		fDirectClips.clear();
		if (info->clip_list != NULL && info->clip_list_count > 0)
			fDirectClips.assign(info->clip_list,
				info->clip_list + info->clip_list_count);
	} else if (state == B_DIRECT_STOP) {
		fDirectConnected = false;
		fDirectBits = NULL;
		fDirectClips.clear();
	}
	bool supported = fDirectBpp == 32 && fDirectRowBytes > 0
		&& (fDirectFormat == B_RGB32 || fDirectFormat == B_RGBA32);
	if (state == B_DIRECT_START && getenv("CINEMA_PLAYER_DEBUG") != NULL) {
		fprintf(stderr, "direct: buffer %dx%d %dbpp format %d clips %d\n",
			(int)(fDirectBounds.right - fDirectBounds.left + 1),
			(int)(fDirectBounds.bottom - fDirectBounds.top + 1),
			(int)fDirectBpp, (int)fDirectFormat,
			(int)fDirectClips.size());
	}
	if (state != B_DIRECT_STOP && !supported)
		PostMessage(kMsgDirectFallback);
	BDirectWindow::DirectConnected(info);
	PostMessage(kMsgDirectDraw);
}


void
CinemaWindow::_DirectCreateLayerLocked()
{
	int32 width = fDirectBounds.right - fDirectBounds.left + 1;
	int32 height = fDirectBounds.bottom - fDirectBounds.top + 1;
	if (width <= 0 || height <= 0)
		return;
	if (fDirectUi != NULL && fDirectUiView != NULL
		&& fDirectUi->Bounds().IntegerWidth() + 1 == width
		&& fDirectUi->Bounds().IntegerHeight() + 1 == height)
		return;
	if (fDirectUiView != NULL) {
		fDirectUi->RemoveChild(fDirectUiView);
		delete fDirectUiView;
		fDirectUiView = NULL;
	}
	delete fDirectUi;
	fDirectUi = NULL;

	BRect bounds(0, 0, width - 1, height - 1);
	fDirectUi = new BBitmap(bounds, B_RGBA32, true);
	if (fDirectUi->InitCheck() != B_OK) {
		delete fDirectUi;
		fDirectUi = NULL;
		return;
	}
	fDirectUiView = new BView(bounds, "direct ui", B_FOLLOW_NONE, 0);
	fDirectUi->AddChild(fDirectUiView);
	fDirectUiView->SetViewColor(B_TRANSPARENT_COLOR);
}


void
CinemaWindow::_DirectFillLocked(uint32 pixel)
{
	if (fDirectBits == NULL || fDirectBpp != 32)
		return;
	for (const clipping_rect& clip : fDirectClips) {
		int32 left = std::max((int32)clip.left, (int32)fDirectBounds.left);
		int32 top = std::max((int32)clip.top, (int32)fDirectBounds.top);
		int32 right = std::min((int32)clip.right,
			(int32)fDirectBounds.right);
		int32 bottom = std::min((int32)clip.bottom,
			(int32)fDirectBounds.bottom);
		for (int32 y = top; y <= bottom; y++) {
			uint32* row = (uint32*)((uint8*)fDirectBits
				+ (y - fDirectBounds.top) * fDirectRowBytes);
			for (int32 x = left; x <= right; x++)
				row[x - fDirectBounds.left] = pixel;
		}
	}
}


void
CinemaWindow::_DirectBlitLocked(BBitmap* picture)
{
	if (picture == NULL || fDirectBits == NULL || fDirectBpp != 32)
		return;
	color_space sourceFormat = picture->ColorSpace();
	if (sourceFormat != B_RGB32 && sourceFormat != B_RGBA32)
		return;

	int32 windowWidth = fDirectBounds.right - fDirectBounds.left + 1;
	int32 windowHeight = fDirectBounds.bottom - fDirectBounds.top + 1;
	BRect source = picture->Bounds();
	int32 sourceWidth = source.IntegerWidth() + 1;
	int32 sourceHeight = source.IntegerHeight() + 1;
	if (sourceWidth <= 0 || sourceHeight <= 0)
		return;
	float scale = std::min((float)windowWidth / sourceWidth,
		(float)windowHeight / sourceHeight);
	int32 width = std::max((int32)1, (int32)(sourceWidth * scale + 0.5f));
	int32 height = std::max((int32)1, (int32)(sourceHeight * scale + 0.5f));
	int32 left = (windowWidth - width) / 2;
	int32 top = (windowHeight - height) / 2;

	const uint32* bits = (const uint32*)picture->Bits();
	bits += (int32)source.top * (picture->BytesPerRow() / 4)
		+ (int32)source.left;
	int32 rowBytes = picture->BytesPerRow();
	for (const clipping_rect& clip : fDirectClips) {
		int32 x0 = std::max((int32)clip.left,
			fDirectBounds.left + left);
		int32 y0 = std::max((int32)clip.top,
			fDirectBounds.top + top);
		int32 x1 = std::min((int32)clip.right,
			fDirectBounds.left + left + width - 1);
		int32 y1 = std::min((int32)clip.bottom,
			fDirectBounds.top + top + height - 1);
		for (int32 y = y0; y <= y1; y++) {
			int32 sourceY = (int32)((int64)(y - fDirectBounds.top - top)
				* sourceHeight / height);
			const uint32* sourceRow = (const uint32*)((const uint8*)bits
				+ sourceY * rowBytes);
			uint32* row = (uint32*)((uint8*)fDirectBits
				+ (y - fDirectBounds.top) * fDirectRowBytes);
			if (sourceWidth == width && sourceHeight == height) {
				memcpy(row + (x0 - fDirectBounds.left),
					sourceRow + (x0 - fDirectBounds.left - left),
					(x1 - x0 + 1) * sizeof(uint32));
				continue;
			}
			for (int32 x = x0; x <= x1; x++) {
				int32 sourceX = (int32)((int64)(x - fDirectBounds.left
					- left) * sourceWidth / width);
				row[x - fDirectBounds.left] = sourceRow[sourceX];
			}
		}
	}
}


void
CinemaWindow::_DirectCompositeLocked()
{
	if (fDirectUi == NULL || fDirectUiView == NULL || fDirectBits == NULL
		|| fDirectBpp != 32)
		return;
	int32 width = fDirectUi->Bounds().IntegerWidth() + 1;
	int32 height = fDirectUi->Bounds().IntegerHeight() + 1;
	int32 windowWidth = fDirectBounds.right - fDirectBounds.left + 1;
	int32 windowHeight = fDirectBounds.bottom - fDirectBounds.top + 1;
	if (width != windowWidth || height != windowHeight)
		return;

	const uint32* bits = (const uint32*)fDirectUi->Bits();
	int32 rowBytes = fDirectUi->BytesPerRow();
	for (const clipping_rect& clip : fDirectClips) {
		int32 x0 = std::max((int32)clip.left, (int32)fDirectBounds.left);
		int32 y0 = std::max((int32)clip.top, (int32)fDirectBounds.top);
		int32 x1 = std::min((int32)clip.right,
			(int32)fDirectBounds.right);
		int32 y1 = std::min((int32)clip.bottom,
			(int32)fDirectBounds.bottom);
		for (int32 y = y0; y <= y1; y++) {
			const uint32* sourceRow = (const uint32*)((const uint8*)bits
				+ (y - fDirectBounds.top) * rowBytes);
			uint32* row = (uint32*)((uint8*)fDirectBits
				+ (y - fDirectBounds.top) * fDirectRowBytes);
			for (int32 x = x0; x <= x1; x++) {
				uint32 sourcePixel = sourceRow[x - fDirectBounds.left];
				uint32 alpha = sourcePixel >> 24;
				if (alpha == 0)
					continue;
				int32 at = x - fDirectBounds.left;
				if (alpha == 255) {
					row[at] = sourcePixel | 0xFF000000;
					continue;
				}
				uint32 destination = row[at];
				uint32 rb = ((sourcePixel & 0x00FF00FF) * alpha
					+ (destination & 0x00FF00FF) * (255 - alpha)
					+ 0x00800080) >> 8;
				uint32 g = ((sourcePixel & 0x0000FF00) * alpha
					+ (destination & 0x0000FF00) * (255 - alpha))
					>> 8;
				row[at] = (rb & 0x00FF00FF) | (g & 0x0000FF00)
					| 0xFF000000;
			}
		}
	}
}


void
CinemaWindow::_DirectDraw()
{
	BAutolock locker(fDirectLock);
	_DirectDrawLocked();
}


void
CinemaWindow::_DirectDrawLocked()
{
	bool supported = fDirectBpp == 32 && fDirectRowBytes > 0
		&& (fDirectFormat == B_RGB32 || fDirectFormat == B_RGBA32);
	if (fDirectActive && (!fDirectConnected || !supported)
		&& !fDirectFallbackAsked && fDirectStart > 0
		&& system_time() - fDirectStart > 3000000) {
		fDirectFallbackAsked = true;
		PostMessage(kMsgDirectFallback);
	}
	if (!fDirectActive || fDirectPaused || !fDirectConnected || !supported
		|| fDirectBits == NULL)
		return;
	_DirectFillLocked(0xFF000000);
	// locked with a picture or without: unlocked either way
	BBitmap* picture = fPlayer->LockFrame();
	if (picture != NULL)
		_DirectBlitLocked(picture);
	fPlayer->UnlockFrame();
	if (fPlayerView->HasOverlayContent()) {
		_DirectCreateLayerLocked();
	}
	if (fDirectUi != NULL && fDirectUiView != NULL
		&& fPlayerView->HasOverlayContent() && fDirectUi->Lock()) {
		uint32 state;
		if (fDirectUi->LockBits(&state) == B_OK) {
			memset(fDirectUi->Bits(), 0, fDirectUi->BitsLength());
			fDirectUi->UnlockBits();
		}
		fPlayerView->DrawOverlay(fDirectUiView);
		fDirectUiView->Sync();
		fDirectUi->Unlock();
		if (fDirectUi->LockBits(&state) == B_OK) {
			_DirectCompositeLocked();
			fDirectUi->UnlockBits();
		}
	}
}


/*!	A video on the whole screen has the whole screen: the tabs and the
	search go away, and come back with the window or without the player.
*/
void
CinemaWindow::_UpdateTopBar()
{
	bool hide = fFullScreen && fPlayerShown;
	if (hide && !fTopBar->IsHidden())
		fTopBar->Hide();
	else if (!hide && fTopBar->IsHidden())
		fTopBar->Show();
}


bool
CinemaWindow::_ShowingDownloads() const
{
	return fCategoryBar->Selected() == fCategoryBar->DownloadsIndex();
}


void
CinemaWindow::_ShowDownloads()
{
	_CloseDetails();
	fCards->SetVisibleItem((int32)0);
	fCard = 0;
	fCategoryBar->SetSelected(fCategoryBar->DownloadsIndex());
	fChannelId = "";
	fGrid->ClearChannelHeader();
	// what is still on its way for a search is not wanted any more
	fGeneration++;
	fDownloaded.clear();
	std::vector<MediaItem> videos;
	// the ones on their way first, with how far they are
	for (const auto& entry : fDownloading) {
		const BMessage& download = entry.second;
		MediaItem item;
		item.id = entry.first;
		item.provider = download.GetString("provider", fProvider);
		item.kind = download.GetString("kind", "video");
		item.title = download.GetString("title", "");
		item.channel = download.GetString("channel", "");
		item.duration = download.GetInt32("duration", 0);
		item.progress = download.GetInt32("percent", 0);
		item.status = _DownloadStatus(download);
		BString picture(fWorker->Cache());
		picture << "/" << PictureName(entry.first) << ".jpg";
		item.picture = Pictures::Load(picture.String());
		videos.push_back(item);
	}
	std::vector<MediaItem> saved = Downloads::Load(&fDownloaded);
	for (MediaItem& item : saved) {
		// a torrent has no picture of its own: the one the library found
		// for the same file, if it showed it already
		if (item.picture != NULL || item.path.IsEmpty())
			continue;
		BString picture(fWorker->Cache());
		picture << "/" << PictureName(item.path) << ".jpg";
		item.picture = Pictures::Load(picture.String());
	}
	videos.insert(videos.end(), saved.begin(), saved.end());
	if (videos.empty()) {
		fGrid->SetMessage(B_TRANSLATE("No videos downloaded yet: right click "
			"a video, or press D"));
	} else
		fGrid->SetVideos(videos);
	fHomeLoaded = false;
	fGrid->MakeFocus(true);
}


void
CinemaWindow::_Download(BMessage* message)
{
	BString id = message->GetString("id", "");
	if (id.IsEmpty() || fDownloaded.count(id) > 0
		|| fDownloading.count(id) > 0)
		return;
	BString folder = Downloads::Folder();
	if (folder.IsEmpty())
		return;

	// the picture now, while it is in the cache
	Downloads::KeepPicture(fWorker->Cache(), id.String());
	BMessage download(*message);
	download.SetInt32("percent", 0);
	fDownloading[id] = download;
	fGrid->SetProgress(id, 0);
	fHome->SetProgress(id, 0);
	fDetail->SetProgress(id.String(), 0);
	fHelper->Download(id.String(), folder.String(),
		message->GetString("provider", _ProviderOf(id).String()));
	_SavePending();
	if (_ShowingDownloads())
		_ShowDownloads();
}


/*!	The torrents on their way, for when Cinema starts again: the other
	downloads start from the beginning anyway.
*/
void
CinemaWindow::_SavePending()
{
	std::vector<BMessage> torrents;
	for (const auto& entry : fDownloading) {
		if (strcmp(entry.second.GetString("provider", ""), "torrent") != 0)
			continue;
		BMessage download(entry.second);
		download.what = kMsgDownload;
		download.RemoveName("percent");
		torrents.push_back(download);
	}
	Downloads::SavePending(torrents);
}


void
CinemaWindow::_Downloaded(BMessage* message)
{
	BString id = message->GetString("id", "");
	auto downloading = fDownloading.find(id);
	if (downloading == fDownloading.end())
		return;
	BMessage video = downloading->second;
	fDownloading.erase(downloading);
	_SavePending();

	Downloads::Describe(message->GetString("path", ""), video);
	if (strcmp(video.GetString("provider", ""), "torrent") == 0) {
		size_t before = fCategories.size();
		_AddLibraryTabs();
		if (fCategories.size() != before) {
			SaveCategories(fCategories);
			fCategoryBar->SetCategories(fCategories);
			fHomeLoaded = false;
		}
	}
	fDownloaded[id] = message->GetString("path", "");
	fGrid->SetDownloaded(id, true);
	fHome->SetDownloaded(id.String(), true);
	fDetail->SetDownloaded(id.String(), true);
	fHomeLoaded = false;
	if (_ShowingDownloads())
		_ShowDownloads();

	BNotification notification(B_INFORMATION_NOTIFICATION);
	notification.SetGroup(B_TRANSLATE_SYSTEM_NAME("Cinema"));
	notification.SetTitle(B_TRANSLATE("Video downloaded"));
	// in place of the progress of a torrent
	notification.SetMessageID(id);
	notification.SetContent(video.GetString("title", ""));
	notification.Send();
}


void
CinemaWindow::_DownloadFailed(BMessage* message)
{
	BString id = message->GetString("id", "");
	auto downloading = fDownloading.find(id);
	if (downloading == fDownloading.end())
		return;
	BString title = downloading->second.GetString("title", "");
	fDownloading.erase(downloading);
	_SavePending();
	fGrid->SetProgress(id, -1);
	fHome->SetProgress(id, -1);
	fDetail->SetProgress(id.String(), -1);

	BNotification notification(B_ERROR_NOTIFICATION);
	notification.SetGroup(B_TRANSLATE_SYSTEM_NAME("Cinema"));
	notification.SetTitle(B_TRANSLATE("The video could not be downloaded"));
	notification.SetMessageID(id);
	notification.SetContent(title);
	notification.Send();
}


void
CinemaWindow::_Trash(BMessage* message)
{
	BString id = message->GetString("id", "");
	BString path = message->GetString("path", "");
	auto file = fDownloaded.find(id);
	if (path.IsEmpty() && file != fDownloaded.end())
		path = file->second;

	// asked first: the answer comes back as this message, with "which"
	if (!message->GetBool("asked", false)) {
		BString title = message->GetString("title", "");
		if (title.IsEmpty())
			title = BPath(path.String()).Leaf();
		BString text(B_TRANSLATE("Move \"%title%\" to the Trash?"));
		text.ReplaceFirst("%title%", title);
		BAlert* alert = new BAlert("Cinema", text.String(),
			B_TRANSLATE("Cancel"), B_TRANSLATE("Move to Trash"), NULL,
			B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		BMessage* answer = new BMessage(*message);
		answer->AddBool("asked", true);
		if (!path.IsEmpty() && !answer->HasString("path"))
			answer->AddString("path", path);
		alert->Go(new BInvoker(answer, this));
		return;
	}
	if (message->GetInt32("which", 0) != 1)
		return;

	status_t status = Downloads::MoveToTrash(path.String());
	if (status != B_OK) {
		BString text(B_TRANSLATE("The video could not be moved to the Trash: "
			"%error%"));
		text.ReplaceFirst("%error%", strerror(status));
		(new BAlert("Cinema", text.String(), B_TRANSLATE("OK")))->Go(NULL);
		return;
	}
	fHomeLoaded = false;
	fDetail->SetDownloaded(id.String(), false);
	fGrid->SetDownloaded(id.String(), false);
	fHome->SetDownloaded(id.String(), false);
	fDownloaded.erase(id);
	if (_ShowingDownloads())
		_ShowDownloads();
	else if ((fDetailsShown ? fDetailsReturn : fCard) == 2) {
		// the home, under the page of the video too: its shelf of the
		// downloads without the one gone
		_LoadHome();
	}
}


/*!	A magnet link, or a .torrent file (a path or an address): downloaded
	with BitTorrent like a video of the other providers, with its progress
	in a notification since it is in no list yet.
*/
void
CinemaWindow::_AddTorrent(const char* source)
{
	// until the helper tells its providers, let it try
	bool supported = fProviders.empty();
	for (const Provider& provider : fProviders)
		if (provider.id == "torrent")
			supported = true;
	if (!supported) {
		(new BAlert("Cinema", B_TRANSLATE("BitTorrent downloads need aria2c, "
			"which is not installed."), B_TRANSLATE("OK")))->Go(NULL);
		return;
	}

	// the name the link gives, or the one of the file
	BString title;
	BString text(source);
	if (text.StartsWith("magnet:")) {
		int32 start = text.FindFirst("dn=");
		if (start >= 0) {
			int32 end = text.FindFirst('&', start);
			if (end < 0)
				end = text.Length();
			text.CopyInto(title, start + 3, end - start - 3);
			title.ReplaceAll('+', ' ');
			title = BUrl::UrlDecode(title);
		}
	} else {
		int32 slash = text.FindLast('/');
		text.CopyInto(title, slash + 1, text.Length() - slash - 1);
		title.RemoveLast(".torrent");
	}
	if (title.IsEmpty())
		title = "BitTorrent";

	BMessage download(kMsgDownload);
	download.AddString("id", source);
	download.AddString("provider", "torrent");
	download.AddString("kind", "video");
	download.AddString("title", title);
	download.AddString("channel", "BitTorrent");
	download.AddString("channel id", "");
	download.AddInt32("duration", 0);
	_Download(&download);
	// where it can be followed, and stopped
	_ShowDownloads();
	// and watched meanwhile: the helper plays it as its pieces come, in
	// order, from the start
	BMessage play(download);
	play.what = kMsgPlay;
	PostMessage(&play);
}


/*!	How far a download is, as its tile says it: "12.3MiB of 700.1MiB ·
	1.2MiB/s · 8 sources · 9m31s left", with what is known of it.
*/
BString
CinemaWindow::_DownloadStatus(const BMessage& download) const
{
	BString status;
	BString done = download.GetString("done", "");
	BString total = download.GetString("total", "");
	if (!done.IsEmpty() && !total.IsEmpty() && total != "0B") {
		status = B_TRANSLATE("%done% of %total%");
		status.ReplaceFirst("%done%", done);
		status.ReplaceFirst("%total%", total);
	} else if (!done.IsEmpty())
		status = done;
	else
		status.SetToFormat("%" B_PRId32 "%%", download.GetInt32("percent", 0));

	BString speed = download.GetString("speed", "");
	if (!speed.IsEmpty())
		status << " · " << speed << "/s";
	int32 peers = download.GetInt32("peers", -1);
	if (peers >= 0) {
		BString text(B_TRANSLATE("%peers% sources"));
		BString number;
		number << peers;
		text.ReplaceFirst("%peers%", number);
		status << " · " << text;
	}
	BString eta = download.GetString("eta", "");
	if (!eta.IsEmpty()) {
		BString text(B_TRANSLATE("%eta% left"));
		text.ReplaceFirst("%eta%", eta);
		status << " · " << text;
	}
	return status;
}


void
CinemaWindow::_DownloadCancelled(BMessage* message)
{
	BString id = message->GetString("id", "");
	auto downloading = fDownloading.find(id);
	if (downloading == fDownloading.end())
		return;
	fDownloading.erase(downloading);
	_SavePending();
	fGrid->SetProgress(id, -1);
	fGrid->SetStatus(id, "");
	fHome->SetProgress(id, -1);
	fDetail->SetProgress(id.String(), -1);
	if (_ShowingDownloads())
		_ShowDownloads();
}


/*!	A folder becomes a tab of the videos in it, a .torrent file is
	downloaded, a video is played.
*/
void
CinemaWindow::_AddFolder(const char* path)
{
	for (size_t i = 0; i < fCategories.size(); i++) {
		if (fCategories[i].provider == "local"
			&& fCategories[i].channel == path) {
			_ShowCategory(i);
			return;
		}
	}
	Category category;
	category.name = BPath(path).Leaf();
	category.provider = "local";
	category.channel = path;
	fCategories.push_back(category);

	_AddLibraryTabs();
	SaveCategories(fCategories);
	fCategoryBar->SetCategories(fCategories);
	fHomeLoaded = false;
	_ShowCategory(fCategories.size() - 1);
}


/*!	A star given or taken away; the favorites have a tab of their own,
	first, from the first one on.
*/
void
CinemaWindow::_ToggleFavorite(BMessage* message)
{
	BString id = message->GetString("id", "");
	bool favorite = Favorites::Toggle(*message);
	fGrid->SetFavorite(id.String(), favorite);
	fHome->SetFavorite(id.String(), favorite);
	fDetail->SetFavorite(id.String(), favorite);
	if (fPlayerShown && id == fPlayingId)
		fPlayerView->SetFavorite(favorite ? 1 : 0);
	fHomeLoaded = false;

	int32 index = -1;
	for (size_t i = 0; i < fCategories.size(); i++) {
		if (fCategories[i].provider == "favorites")
			index = i;
	}
	if (index < 0 && favorite) {
		Category category;
		category.name = B_TRANSLATE("Favorites");
		category.provider = "favorites";
		category.channel = "all";
		fCategories.insert(fCategories.begin(), category);
		int32 selected = fCategoryBar->Selected();
		SaveCategories(fCategories);
		fCategoryBar->SetCategories(fCategories);
		// the tab chosen moved one on
		if (selected >= fCategoryBar->CategoryTab(0))
			fCategoryBar->SetSelected(selected + 1);
		// so did the shelves of the home, which find them by their place
		_HomeChanged();
		return;
	}
	// the favorites shown: without the one taken away
	if (index >= 0 && fCard == 0
		&& fCategoryBar->Selected() == fCategoryBar->CategoryTab(index))
		_ShowCategory(index);
}


/*!	Once, a tab with the free channels of the country of the system: if
	it is taken away, it does not come back.
*/
void
CinemaWindow::_OfferTv()
{
	if (fSettings.tvOffered)
		return;
	fSettings.tvOffered = true;
	fSettings.Save();
	BString country = TvLists::SystemCountry();
	if (country.IsEmpty())
		return;
	BString address = TvLists::Address(country);
	for (const Category& existing : fCategories) {
		if (existing.provider == "iptv" && existing.channel == address)
			return;
	}
	Category category;
	category.name = B_TRANSLATE("TV");
	category.provider = "iptv";
	category.channel = address;
	fCategories.push_back(category);
	SaveCategories(fCategories);
	fCategoryBar->SetCategories(fCategories);
}


/*!	A tab with the channels of a list M3U, an address or a file. */
void
CinemaWindow::_AddTvList(const char* address, const char* name)
{
	BString list(address);
	list.Trim();
	if (list.IsEmpty())
		return;
	int32 lists = 0;
	for (size_t i = 0; i < fCategories.size(); i++) {
		if (fCategories[i].provider != "iptv")
			continue;
		if (fCategories[i].channel == list) {
			_ShowCategory(i);
			return;
		}
		lists++;
	}
	Category category;
	if (name != NULL && name[0] != '\0')
		category.name = name;
	else {
		category.name = B_TRANSLATE("TV");
		if (lists > 0)
			category.name << " " << lists + 1;
	}
	category.provider = "iptv";
	category.channel = list;
	fCategories.push_back(category);

	SaveCategories(fCategories);
	fCategoryBar->SetCategories(fCategories);
	fHomeLoaded = false;
	_ShowCategory(fCategories.size() - 1);
}


void
CinemaWindow::_Dropped(BMessage* message)
{
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BEntry entry(&ref, true);
		BPath path;
		if (entry.GetPath(&path) != B_OK)
			continue;
		BString name(path.Leaf());
		if (entry.IsDirectory())
			_AddFolder(path.Path());
		else if (name.IEndsWith(".torrent"))
			_AddTorrent(path.Path());
		else if (name.IEndsWith(".m3u") || name.IEndsWith(".m3u8")) {
			_AddTvList(path.Path());
			_SendFolders();
		} else
			_Play(path.Path(), path.Leaf());
	}
}


void
CinemaWindow::_OpenPlayer(const char* address, const char* title,
	const char* id)
{
	if (fPlayerShown)
		_ClosePlayer();
	fPlayingId = id;
	fPlayingAddress = address;
	fPlayingTitle = title;
	fPlayingKind = fNextKind;
	fNextKind = "";
	fPlayerReturn = fCard;
	fCards->SetVisibleItem(3);
	fCard = 3;
	fPlayerShown = true;
	_UpdateTopBar();
	fPlayerView->Start(title);
	// a star to give to what has a page of its own: not to live TV, nor
	// to an address played as it is
	if (fPlayingItem.id == fPlayingId && !fPlayingId.IsEmpty()
		&& fPlayingKind != "live" && !LiveAddress::Is(address))
		fPlayerView->SetFavorite(Favorites::Has(id) ? 1 : 0);
	else
		fPlayerView->SetFavorite(-1);
	fPlayerView->MakeFocus(true);
	// the overlay of the video before goes before its bitmap
	fPlayerView->ClearOverlay();
	// a video of YouTube made smaller goes on from where it was
	bigtime_t at = fSettings.resume ? Positions::ResumeAt(id) : 0;
	if (fOpenAt >= 0 && fLowered == id)
		at = fOpenAt;
	fOpenAt = -1;
	fPlayer->Open(address, at);
	fPlayerView->RefreshSubtitles();

	fSubtitlesAsked.clear();
	fSubtitleRetries.clear();
	if (!fPlayingId.IsEmpty() && address[0] != '/'
		&& fPlayingKind != "live" && !LiveAddress::Is(address)) {
		fHelper->Subtitles(fPlayingId.String(),
			_ProviderOf(fPlayingId).String(), fPlayingKind.String());
	}

	delete fPlayerSaver;
	BMessage save(kMsgPlayerSave);
	fPlayerSaver = new BMessageRunner(BMessenger(this), &save, 5000000);
}


void
CinemaWindow::_SavePosition()
{
	// live, there is nowhere to go back to
	if (fPlayingKind == "live")
		return;
	if (fPlayerShown && fPlayer->IsOpen() && !fPlayingId.IsEmpty())
		Positions::Set(fPlayingId, fPlayer->Position(), fPlayer->Duration());
}


void
CinemaWindow::_ClosePlayer(bool save)
{
	if (!fPlayerShown)
		return;
	if (save)
		_SavePosition();
	delete fPlayerSaver;
	fPlayerSaver = NULL;
	fPlayerView->ClearOverlay();
	fPlayer->Close();
	// the volume, for the next videos and the next time Cinema starts
	if (fabsf(fPlayer->Volume() - fSettings.volume) > 0.01f) {
		fSettings = Settings::Load();
		fSettings.volume = fPlayer->Volume();
		fSettings.Save();
	}
	fSubtitlesAsked.clear();
	fSubtitleRetries.clear();
	fPlayerShown = false;
	if (fFullScreen)
		_DirectSync();
	_UpdateTopBar();
	fCards->SetVisibleItem(fPlayerReturn);
	fCard = fPlayerReturn;
	Positions::Position position = Positions::Get(fPlayingId);
	fGrid->SetWatched(fPlayingId, position.watched, position.duration > 0
		? (int32)(position.position * 1000 / position.duration) : 0);
	_FocusContent();
}


void
CinemaWindow::_RequestSubtitle(const char* language, bool automatic)
{
	if (!fPlayerShown || fPlayingId.IsEmpty() || language == NULL
		|| language[0] == '\0')
		return;
	BString key(fPlayingId);
	key << "\n" << language << "\n" << (automatic ? "1" : "0");
	if (!fSubtitlesAsked.insert(key).second)
		return;
	fHelper->Subtitle(fPlayingId.String(), language, automatic,
		_ProviderOf(fPlayingId).String(), fPlayingKind.String());
}


void
CinemaWindow::_RequestSelectedSubtitle()
{
	if (!fPlayerShown)
		return;
	int32 index = fPlayer->SelectedSubtitle();
	SubtitleTrackInfo info;
	if (index < 0 || !fPlayer->SubtitleTrack(index, &info) || info.loaded)
		return;
	_RequestSubtitle(info.language.String(), info.automatic);
}


void
CinemaWindow::_ShowSettings()
{
	if (fSettingsWindow.IsValid()) {
		// open already: to the front
		BLooper* looper = NULL;
		fSettingsWindow.Target(&looper);
		BWindow* window = dynamic_cast<BWindow*>(looper);
		if (window != NULL && window->Lock()) {
			window->Activate();
			window->Unlock();
		}
		return;
	}
	std::vector<BString> folders;
	for (const Category& category : fCategories) {
		if (category.provider == "local")
			folders.push_back(category.channel);
	}
	SettingsWindow* window = new SettingsWindow(BMessenger(this),
		fProviders, folders);
	fSettingsWindow = BMessenger(window);
	window->Show();
	// the lists of TV channels too
	_SendFolders();
}


void
CinemaWindow::_SendFolders()
{
	if (!fSettingsWindow.IsValid())
		return;
	BMessage folders(kMsgSettingsFolders);
	for (const Category& category : fCategories) {
		if (category.provider == "local")
			folders.AddString("path", category.channel);
		else if (category.provider == "iptv")
			folders.AddString("tv", category.channel);
	}
	fSettingsWindow.SendMessage(&folders);
}


void
CinemaWindow::_ShowAbout()
{
	BString text(B_TRANSLATE("Cinema: videos for Haiku and RenkuOS, from "
		"YouTube, the Internet Archive, PeerTube, BitTorrent and your own "
		"folders."));
	text << "\n\n© 2026 Andrea Bernardi\n"
		"https://github.com/atomozero/Cinema\n\n"
		"This product uses the TMDB API but is not endorsed or certified by "
		"TMDB.";
	BAlert* alert = new BAlert(B_TRANSLATE("About Cinema"), text.String(),
		B_TRANSLATE("OK"));
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(NULL);
}


/*!	The Movies and TV series tabs, once: when a folder is added, or a
	torrent is downloaded.
*/
void
CinemaWindow::_AddLibraryTabs()
{
	for (const Category& existing : fCategories)
		if (existing.provider == "library")
			return;
	Category movies;
	movies.name = B_TRANSLATE("Movies");
	movies.provider = "library";
	movies.channel = "movies";
	fCategories.push_back(movies);
	Category series;
	series.name = B_TRANSLATE("TV series");
	series.provider = "library";
	series.channel = "series";
	fCategories.push_back(series);
}
