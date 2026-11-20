/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "SettingsWindow.h"

#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Country.h>
#include <LocaleRoster.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <SpaceLayoutItem.h>
#include <GroupLayout.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <Url.h>

#include "Messages.h"
#include "NewsChannels.h"
#include "Style.h"
#include "TvLists.h"
#include "VideoLimits.h"

#include <algorithm>
#include <string.h>
#include <utility>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Settings"


static const uint32 kMsgKeyChanged = 'skey';
static const uint32 kMsgGetKey = 'sget';
static const uint32 kMsgCheck = 'schk';
static const uint32 kMsgAdd = 'sadd';
static const uint32 kMsgRemove = 'srem';
static const uint32 kMsgFolderSelected = 'ssel';
static const uint32 kMsgPlayInCinema = 'splc';
static const uint32 kMsgResume = 'sres';
static const uint32 kMsgEvenVolume = 'sevn';
static const uint32 kMsgOverlay = 'sovl';
static const uint32 kMsgSource = 'sssr';
static const uint32 kMsgQuality = 'squa';
static const uint32 kMsgTvSelected = 'stvs';
static const uint32 kMsgTvAddress = 'stva';
static const uint32 kMsgTvAdd = 'stv+';
static const uint32 kMsgTvRemove = 'stv-';
static const uint32 kMsgTvCountry = 'stvc';
static const uint32 kMsgActivate = kMsgSettingsActivate;
static const uint32 kMsgNetworkChanged = 'snet';
static const uint32 kMsgOrganize = 'sorg';
static const uint32 kMsgNews = 'snws';
static const uint32 kMsgOrganizeNow = 'sorn';
static const uint32 kMsgPasswordChanged = 'snpw';
static const uint32 kMsgNetworkTest = 'sntt';


static const char* kKeyPage = "https://www.themoviedb.org/settings/api";


SettingsWindow::SettingsWindow(const BMessenger& target,
	const std::vector<Provider>& providers,
	const std::vector<BString>& folders)
	:
	BWindow(BRect(0, 0, Style::S(480), Style::S(300)),
		B_TRANSLATE("Cinema settings"),
		B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fTarget(target),
	fSettings(Settings::Load()),
	fShown(fSettings),
	fPanel(NULL)
{
	// the library: the key of TMDB
	fKey = new BTextControl("key", B_TRANSLATE("TMDB key:"),
		Settings::TmdbKey().String(), new BMessage(kMsgKeyChanged));
	fKey->SetModificationMessage(new BMessage(kMsgKeyChanged));
	fCheck = new BButton("check", B_TRANSLATE("Check"),
		new BMessage(kMsgCheck));
	BButton* getKey = new BButton("get", B_TRANSLATE("Get a key" B_UTF8_ELLIPSIS),
		new BMessage(kMsgGetKey));
	fKeyStatus = new BStringView("status", B_TRANSLATE("With a key, the "
		"library gets posters, plots and casts. It is free."));
	fKeyStatus->SetExplicitMinSize(BSize(Style::S(300), B_SIZE_UNSET));

	// the folders of the library
	fFolders = new BListView("folders");
	fFolders->SetSelectionMessage(new BMessage(kMsgFolderSelected));
	BScrollView* scroll = new BScrollView("folders scroll", fFolders, 0,
		false, true);
	scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, Style::S(90)));
	BButton* add = new BButton("add", B_TRANSLATE("Add" B_UTF8_ELLIPSIS),
		new BMessage(kMsgAdd));
	fRemove = new BButton("remove", B_TRANSLATE("Remove"),
		new BMessage(kMsgRemove));
	fRemove->SetEnabled(false);

	BBox* foldersBox = new BBox("folders box");
	foldersBox->SetLabel(B_TRANSLATE("Folders of the library"));
	BLayoutBuilder::Group<>(foldersBox, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_BIG_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(scroll, 10)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fRemove)
			.Add(add)
		.End();

	// the lists of TV channels
	fTvLists = new BListView("tv lists");
	fTvLists->SetSelectionMessage(new BMessage(kMsgTvSelected));
	BScrollView* tvScroll = new BScrollView("tv scroll", fTvLists, 0,
		false, true);
	tvScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, Style::S(60)));
	fTvAddress = new BTextControl("tv address", B_TRANSLATE("M3U list:"), "",
		new BMessage(kMsgTvAdd));
	fTvAddress->SetModificationMessage(new BMessage(kMsgTvAddress));
	fTvAdd = new BButton("tv add", B_TRANSLATE("Add"),
		new BMessage(kMsgTvAdd));
	fTvAdd->SetEnabled(false);
	fTvRemove = new BButton("tv remove", B_TRANSLATE("Remove"),
		new BMessage(kMsgTvRemove));
	fTvRemove->SetEnabled(false);
	// the free channels of a country, from the public lists of iptv-org
	std::vector<std::pair<BString, BString>> countries;
	BMessage available;
	BLocaleRoster::Default()->GetAvailableCountries(&available);
	const char* code;
	for (int32 i = 0; available.FindString("country", i, &code) == B_OK;
			i++) {
		BString name;
		if (strlen(code) != 2 || BCountry(code).GetName(name) != B_OK)
			continue;
		countries.push_back(std::make_pair(name, BString(code)));
	}
	std::sort(countries.begin(), countries.end(),
		[](const std::pair<BString, BString>& a,
			const std::pair<BString, BString>& b) {
			return a.first.ICompare(b.first) < 0;
		});
	BPopUpMenu* countryMenu = new BPopUpMenu(B_TRANSLATE("Choose"
		B_UTF8_ELLIPSIS), true, false);
	for (const auto& country : countries) {
		BMessage* message = new BMessage(kMsgTvCountry);
		message->AddString("code", country.second);
		message->AddString("name", country.first);
		countryMenu->AddItem(new BMenuItem(country.first.String(), message));
	}
	Style::ScaleMenu(countryMenu);
	fTvCountry = new BMenuField("tv country",
		B_TRANSLATE("Free channels of:"), countryMenu);

	BStringView* tvNote = new BStringView("tv note",
		B_TRANSLATE("The address of a list of channels, or drop an .m3u file "
			"on Cinema."));

	// how the videos play
	fPlayInCinema = new BCheckBox("cinema", B_TRANSLATE("Play the videos in "
		"Cinema, not in MediaPlayer"), new BMessage(kMsgPlayInCinema));
	fPlayInCinema->SetValue(fSettings.playInCinema);
	fResume = new BCheckBox("resume", B_TRANSLATE("Go on from where a video "
		"was left"), new BMessage(kMsgResume));
	fResume->SetValue(fSettings.resume);
	fResume->SetEnabled(fSettings.playInCinema);
	fEvenVolume = new BCheckBox("even volume", B_TRANSLATE("Even volume: "
		"films and TV as loud as music"), new BMessage(kMsgEvenVolume));
	fEvenVolume->SetValue(fSettings.evenVolume);
	fEvenVolume->SetEnabled(fSettings.playInCinema);
	BPopUpMenu* overlays = new BPopUpMenu("overlays");
	const char* overlayLabels[] = { B_TRANSLATE("Automatic"),
		B_TRANSLATE("Always"), B_TRANSLATE("Never") };
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kMsgOverlay);
		message->AddInt32("choice", i);
		BMenuItem* item = new BMenuItem(overlayLabels[i], message);
		item->SetMarked(i == fSettings.overlayChoice);
		overlays->AddItem(item);
	}
	if (overlays->FindMarked() == NULL)
		overlays->ItemAt(0)->SetMarked(true);
	Style::ScaleMenu(overlays);
	fOverlay = new BMenuField("overlay", B_TRANSLATE("Video overlay:"),
		overlays);
	fOverlay->SetEnabled(fSettings.playInCinema);
	BStringView* overlayNote = new BStringView("overlay note",
		B_TRANSLATE("Automatic asks once, on a graphics card it does not "
			"know, whether you see the video. O switches it while one "
			"plays."));

	// how big the pictures of YouTube and PeerTube are
	BPopUpMenu* qualities = new BPopUpMenu("qualities");
	static const int32 kHeights[] = { 0, 360, 480, 720, 1080, 1440, 2160 };
	int32 limit = VideoHeightLimit();
	for (int32 height : kHeights) {
		if (height > limit)
			continue;
		BString label;
		if (height == 0)
			label = B_TRANSLATE("Automatic");
		else
			label.SetToFormat("%" B_PRId32 "p", height);
		BMessage* message = new BMessage(kMsgQuality);
		message->AddInt32("height", height);
		BMenuItem* item = new BMenuItem(label.String(), message);
		item->SetMarked(height == fSettings.videoHeight);
		qualities->AddItem(item);
	}
	if (qualities->FindMarked() == NULL)
		qualities->ItemAt(0)->SetMarked(true);
	Style::ScaleMenu(qualities);
	fQuality = new BMenuField("quality", B_TRANSLATE("Video quality:"),
		qualities);
	BStringView* qualityNote = new BStringView("quality note",
		B_TRANSLATE("Above 360p, YouTube takes longer to start."));

	BBox* playback = new BBox("playback");
	playback->SetBorder(B_NO_BORDER);
	BLayoutBuilder::Group<>(playback, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(0)
		.Add(fPlayInCinema)
		.Add(fResume)
		.Add(fEvenVolume)
		.Add(fOverlay)
		.Add(overlayNote)
		.Add(fQuality)
		.Add(qualityNote);

	// where a search goes first
	BPopUpMenu* sources = new BPopUpMenu("sources");
	for (const Provider& provider : providers) {
		if (!provider.Supports("search"))
			continue;
		BMessage* message = new BMessage(kMsgSource);
		message->AddString("provider", provider.id);
		BMenuItem* item = new BMenuItem(provider.name.String(), message);
		item->SetMarked(provider.id == fSettings.searchSource);
		sources->AddItem(item);
	}
	if (sources->FindMarked() == NULL && sources->CountItems() > 0)
		sources->ItemAt(0)->SetMarked(true);
	Style::ScaleMenu(sources);
	fSource = new BMenuField("source", B_TRANSLATE("Search first in:"),
		sources);

	BBox* search = new BBox("search");
	search->SetLabel(B_TRANSLATE("Search"));
	BLayoutBuilder::Group<>(search, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_BIG_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fSource);

	// the network folder: where downloads are copied or moved, a disk of
	// a FRITZ!Box say, by FTP; the password in the keyring of Haiku
	fNetworkFolder = new BTextControl("network folder",
		B_TRANSLATE("Address:"), fSettings.networkFolder.String(),
		new BMessage(kMsgNetworkChanged));
	fNetworkFolder->SetModificationMessage(new BMessage(kMsgNetworkChanged));
	fNetworkUser = new BTextControl("network user", B_TRANSLATE("User:"),
		fSettings.networkUser.String(), new BMessage(kMsgNetworkChanged));
	fNetworkUser->SetModificationMessage(new BMessage(kMsgNetworkChanged));
	// read from the keyring only when there is a folder: it may ask
	fNetworkPassword = new BTextControl("network password",
		B_TRANSLATE("Password:"), fSettings.networkFolder.IsEmpty() ? ""
			: Settings::NetworkPassword().String(),
		new BMessage(kMsgPasswordChanged));
	fNetworkPassword->SetModificationMessage(
		new BMessage(kMsgPasswordChanged));
	fNetworkPassword->TextView()->HideTyping(true);
	fNetworkTest = new BButton("network test", B_TRANSLATE("Test"),
		new BMessage(kMsgNetworkTest));
	fNetworkStatus = new BStringView("network status", "");
	fNetworkStatus->SetExplicitMinSize(BSize(Style::S(300), B_SIZE_UNSET));
	BStringView* networkNote = new BStringView("network note",
		B_TRANSLATE("Downloaded videos can be copied or moved there, with a "
			"right click."));
	BStringView* networkExample = new BStringView("network example",
		B_TRANSLATE("For a FRITZ!Box: ftp://192.168.178.1/ and the name of "
			"its disk."));

	// the downloads put in order once done
	fOrganize = new BCheckBox("organize", B_TRANSLATE("Put new downloads in "
		"Films, Series and Videos, with a picture"), new BMessage(kMsgOrganize));
	fOrganize->SetValue(fSettings.organizeDownloads);
	BButton* organizeNow = new BButton("organize now",
		B_TRANSLATE("Put them all in order now"),
		new BMessage(kMsgOrganizeNow));

	// the account of YouTube: signed in with WebPositive, whose cookies of
	// YouTube Cinema reads for the pages of the account
	fYouTubeStatus = new BStringView("youtube status",
		fSettings.youtubeAccount ? B_TRANSLATE("Signed in: the tabs of the "
			"account are there.") : B_TRANSLATE("Not signed in."));
	fYouTubeStatus->SetExplicitMinSize(BSize(Style::S(300), B_SIZE_UNSET));
	BButton* youtubeSignIn = new BButton("youtube sign in",
		B_TRANSLATE("Sign in with WebPositive" B_UTF8_ELLIPSIS),
		new BMessage(kMsgYouTubeSignIn));
	fYouTubeCheck = new BButton("youtube check",
		B_TRANSLATE("I signed in"), new BMessage(kMsgCheckYouTube));
	fYouTubeSignOut = new BButton("youtube sign out",
		B_TRANSLATE("Sign out"), new BMessage(kMsgYouTubeSignOut));
	fYouTubeSignOut->SetEnabled(fSettings.youtubeAccount);
	BStringView* youtubeSteps = new BStringView("youtube steps",
		B_TRANSLATE("1. Sign in to YouTube in WebPositive.  2. Come back and "
			"press \"I signed in\"."));
	BStringView* youtubeTabs = new BStringView("youtube tabs",
		B_TRANSLATE("Subscriptions, Watch later, Liked videos and History "
			"become tabs."));
	BStringView* youtubeNote = new BStringView("youtube note",
		B_TRANSLATE("Only these pages use the account; Cinema keeps no "
			"password."));

	// three tabs, so that the window fits a small screen such as the
	// AC100's 1024x600: the library, the TV channels, how the videos play
	BView* libraryTab = new BView(B_TRANSLATE("Library"), 0);
	BLayoutBuilder::Group<>(libraryTab, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(fKey)
			.Add(fCheck)
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(fKeyStatus, 10)
			.Add(getKey)
		.End()
		.Add(foldersBox, 10)
		.AddGroup(B_HORIZONTAL)
			.Add(fOrganize, 10)
			.Add(organizeNow)
		.End();

	BView* tvTab = new BView(B_TRANSLATE("TV channels"), 0);
	BLayoutBuilder::Group<>(tvTab, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(tvScroll, 10)
		.Add(fTvCountry)
		.AddGroup(B_HORIZONTAL)
			.Add(fTvAddress, 10)
			.Add(fTvAdd)
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(tvNote, 10)
			.Add(fTvRemove)
		.End();

	BView* playbackTab = new BView(B_TRANSLATE("Playback"), 0);
	BLayoutBuilder::Group<>(playbackTab, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(playback)
		.Add(search)
		.AddGlue();

	// the news: the channels the News tab is made of
	std::vector<BString> chosen = fSettings.newsChannelsSet
		? fSettings.newsChannels : DefaultNewsChannels();
	BView* newsTab = new BView(B_TRANSLATE("News"), 0);
	BGroupLayout* newsLayout = BLayoutBuilder::Group<>(newsTab, B_VERTICAL,
		B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(new BStringView("news note", B_TRANSLATE("The News tab shows the "
			"latest videos of these channels, the newest first:")))
		.Layout();
	for (const NewsChannel& channel : kNewsChannels) {
		BMessage* message = new BMessage(kMsgNews);
		BCheckBox* box = new BCheckBox(channel.id, channel.name, message);
		box->SetValue(std::find(chosen.begin(), chosen.end(),
			BString(channel.id)) != chosen.end());
		fNews.push_back(box);
		newsLayout->AddView(box);
	}
	newsLayout->AddView(new BStringView("news none", B_TRANSLATE("None "
		"chosen: the tab searches YouTube instead.")));
	newsLayout->AddItem(BSpaceLayoutItem::CreateGlue());

	BView* youtubeTab = new BView(B_TRANSLATE("YouTube account"), 0);
	BLayoutBuilder::Group<>(youtubeTab, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(youtubeSteps)
		.AddGroup(B_HORIZONTAL)
			.Add(youtubeSignIn)
			.Add(fYouTubeCheck)
			.AddGlue()
			.Add(fYouTubeSignOut)
		.End()
		.Add(fYouTubeStatus)
		.Add(youtubeTabs)
		.Add(youtubeNote)
		.AddGlue();

	BView* networkTab = new BView(B_TRANSLATE("Network folder"), 0);
	BLayoutBuilder::Group<>(networkTab, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fNetworkFolder, 0, 0)
			.AddTextControl(fNetworkUser, 0, 1)
			.AddTextControl(fNetworkPassword, 0, 2)
		.End()
		.Add(networkExample)
		.AddGroup(B_HORIZONTAL)
			.Add(fNetworkStatus, 10)
			.Add(fNetworkTest)
		.End()
		.Add(networkNote)
		.AddGlue();

	BTabView* tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
	tabs->SetBorder(B_NO_BORDER);
	// the tabs not shown are out of the window: scaled one by one
	BView* pages[] = { libraryTab, tvTab, playbackTab, newsTab, youtubeTab,
		networkTab };
	for (BView* page : pages) {
		page->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		Style::ScaleView(page);
		tabs->AddTab(page);
	}

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.SetInsets(0, B_USE_SMALL_SPACING, 0, 0)
		.Add(tabs);

	for (int32 i = 0; i < CountChildren(); i++)
		Style::ScaleView(ChildAt(i));

	BMessage list;
	for (const BString& folder : folders)
		list.AddString("path", folder);
	_SetFolders(&list);
	CenterOnScreen();
}


SettingsWindow::~SettingsWindow()
{
	delete fPanel;
}


void
SettingsWindow::Show()
{
	BWindow::Show();
	// opened from a menu, which gives the focus back to the main window
	// as it closes: take it again a moment later
	BMessageRunner::StartSending(BMessenger(this),
		new BMessage(kMsgActivate), 200000, 1);
}


bool
SettingsWindow::QuitRequested()
{
	// the key as it is, even without Enter
	Settings::SetTmdbKey(fKey->Text());
	_SaveNetwork();
	BMessage closed(kMsgSettingsClosed);
	closed.AddMessenger("window", BMessenger(this));
	fTarget.SendMessage(&closed);
	return true;
}


void
SettingsWindow::_SetFolders(const BMessage* message)
{
	while (BListItem* item = fFolders->RemoveItem((int32)0))
		delete item;
	const char* path;
	for (int32 i = 0; message->FindString("path", i, &path) == B_OK; i++)
		fFolders->AddItem(new BStringItem(path));
	fRemove->SetEnabled(false);

	while (BListItem* item = fTvLists->RemoveItem((int32)0))
		delete item;
	const char* address;
	for (int32 i = 0; message->FindString("tv", i, &address) == B_OK; i++)
		fTvLists->AddItem(new BStringItem(address));
	fTvRemove->SetEnabled(false);
}


void
SettingsWindow::_Changed()
{
	// what this window shows over what is saved now: the volume and the
	// language of the subtitles change meanwhile, in the player
	// only what was changed here since: the main window may have changed
	// the others meanwhile (O sets the overlay to automatic)
	Settings saved = Settings::Load();
	if (fSettings.playInCinema != fShown.playInCinema)
		saved.playInCinema = fSettings.playInCinema;
	if (fSettings.resume != fShown.resume)
		saved.resume = fSettings.resume;
	if (fSettings.searchSource != fShown.searchSource)
		saved.searchSource = fSettings.searchSource;
	if (fSettings.videoHeight != fShown.videoHeight)
		saved.videoHeight = fSettings.videoHeight;
	if (fSettings.evenVolume != fShown.evenVolume)
		saved.evenVolume = fSettings.evenVolume;
	if (fSettings.overlayChoice != fShown.overlayChoice)
		saved.overlayChoice = fSettings.overlayChoice;
	fSettings = saved;
	fShown = saved;
	fSettings.Save();
	fTarget.SendMessage(kMsgSettingsChanged);
}


void
SettingsWindow::_SaveNetwork()
{
	Settings saved = Settings::Load();
	BString folder(fNetworkFolder->Text());
	folder.Trim();
	saved.networkFolder = folder;
	saved.networkUser = fNetworkUser->Text();
	saved.Save();
	fSettings.networkFolder = saved.networkFolder;
	fSettings.networkUser = saved.networkUser;
	if (fPasswordChanged) {
		fPasswordChanged = false;
		if (Settings::SetNetworkPassword(fNetworkPassword->Text()) != B_OK)
			fNetworkStatus->SetText(B_TRANSLATE("The password could not be "
				"kept in the keyring."));
	}
}


void
SettingsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgYouTubeSignIn:
			fTarget.SendMessage(kMsgYouTubeSignIn);
			fYouTubeStatus->SetText(B_TRANSLATE("Sign in in WebPositive, "
				"then press \"I signed in\"."));
			break;
		case kMsgCheckYouTube:
			fYouTubeStatus->SetText(B_TRANSLATE("Checking" B_UTF8_ELLIPSIS));
			fYouTubeCheck->SetEnabled(false);
			fTarget.SendMessage(kMsgCheckYouTube);
			break;
		case kMsgYouTubeChecked:
		{
			fYouTubeCheck->SetEnabled(true);
			bool ok = message->GetBool("ok", false);
			fYouTubeSignOut->SetEnabled(ok);
			if (ok) {
				fYouTubeStatus->SetText(B_TRANSLATE("Signed in: the tabs of "
					"the account are there."));
			} else {
				BString text(B_TRANSLATE("Not signed in: %error%"));
				text.ReplaceFirst("%error%", message->GetString("error", ""));
				fYouTubeStatus->SetText(text.String());
			}
			break;
		}
		case kMsgYouTubeSignOut:
			fTarget.SendMessage(kMsgYouTubeSignOut);
			fYouTubeSignOut->SetEnabled(false);
			fYouTubeStatus->SetText(B_TRANSLATE("Not signed in."));
			break;
		case kMsgHelperGone:
			// no answer will come to a check asked of it
			fCheck->SetEnabled(true);
			fYouTubeCheck->SetEnabled(true);
			fNetworkTest->SetEnabled(true);
			for (BStringView* status : { fKeyStatus, fYouTubeStatus,
					fNetworkStatus }) {
				if (strcmp(status->Text(),
						B_TRANSLATE("Checking" B_UTF8_ELLIPSIS)) == 0)
					status->SetText("");
			}
			break;
		case kMsgNews:
		{
			Settings saved = Settings::Load();
			saved.newsChannels.clear();
			for (BCheckBox* box : fNews) {
				if (box->Value() == B_CONTROL_ON)
					saved.newsChannels.push_back(box->Name());
			}
			saved.newsChannelsSet = true;
			saved.Save();
			fSettings.newsChannels = saved.newsChannels;
			fSettings.newsChannelsSet = true;
			fTarget.SendMessage(kMsgSettingsChanged);
			break;
		}
		case kMsgOrganize:
		{
			Settings saved = Settings::Load();
			saved.organizeDownloads = fOrganize->Value() == B_CONTROL_ON;
			saved.Save();
			fSettings.organizeDownloads = saved.organizeDownloads;
			break;
		}
		case kMsgOrganizeNow:
			fTarget.SendMessage(kMsgOrganizeAll);
			break;
		case kMsgNetworkChanged:
			_SaveNetwork();
			fNetworkStatus->SetText("");
			break;
		case kMsgPasswordChanged:
			fPasswordChanged = true;
			fNetworkStatus->SetText("");
			break;
		case kMsgNetworkTest:
		{
			_SaveNetwork();
			fNetworkStatus->SetText(B_TRANSLATE("Checking" B_UTF8_ELLIPSIS));
			fNetworkTest->SetEnabled(false);
			BMessage check(kMsgCheckUpload);
			check.AddString("target", fNetworkFolder->Text());
			check.AddString("user", fNetworkUser->Text());
			check.AddString("password", fNetworkPassword->Text());
			fTarget.SendMessage(&check);
			break;
		}
		case kMsgUploadChecked:
		{
			fNetworkTest->SetEnabled(true);
			if (message->GetBool("ok", false)) {
				fNetworkStatus->SetText(B_TRANSLATE("The folder can be "
					"written to."));
			} else {
				BString text(B_TRANSLATE("It does not work: %error%"));
				text.ReplaceFirst("%error%", message->GetString("error", ""));
				fNetworkStatus->SetText(text.String());
			}
			break;
		}
		case kMsgActivate:
			Activate(true);
			if (fKey->Text()[0] == '\0')
				fKey->MakeFocus(true);
			break;
		case kMsgKeyChanged:
			Settings::SetTmdbKey(fKey->Text());
			fKeyStatus->SetText("");
			break;
		case kMsgGetKey:
			BUrl(kKeyPage, true).OpenWithPreferredApplication();
			break;
		case kMsgCheck:
			Settings::SetTmdbKey(fKey->Text());
			fKeyStatus->SetText(B_TRANSLATE("Checking" B_UTF8_ELLIPSIS));
			fCheck->SetEnabled(false);
			fTarget.SendMessage(kMsgCheckTmdb);
			break;
		case kMsgTmdbChecked:
			fCheck->SetEnabled(true);
			fKeyStatus->SetText(message->GetBool("ok", false)
				? B_TRANSLATE("The key works.")
				: B_TRANSLATE("TMDB does not accept this key."));
			break;

		case kMsgFolderSelected:
			fRemove->SetEnabled(fFolders->CurrentSelection() >= 0);
			break;
		case kMsgAdd:
			if (fPanel == NULL) {
				fPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
					NULL, B_DIRECTORY_NODE, false, NULL, NULL, true);
			}
			fPanel->Show();
			break;
		case B_REFS_RECEIVED:
		{
			entry_ref ref;
			for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
				BPath path(&ref);
				BMessage add(kMsgAddFolder);
				add.AddString("path", path.Path());
				fTarget.SendMessage(&add);
			}
			break;
		}
		case kMsgRemove:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fFolders->ItemAt(fFolders->CurrentSelection()));
			if (item == NULL)
				break;
			BMessage remove(kMsgRemoveFolder);
			remove.AddString("path", item->Text());
			fTarget.SendMessage(&remove);
			break;
		}
		case kMsgSettingsFolders:
			_SetFolders(message);
			break;

		case kMsgTvSelected:
			fTvRemove->SetEnabled(fTvLists->CurrentSelection() >= 0);
			break;
		case kMsgTvAddress:
		{
			BString text(fTvAddress->Text());
			text.Trim();
			fTvAdd->SetEnabled(!text.IsEmpty());
			break;
		}
		case kMsgTvAdd:
		{
			BString text(fTvAddress->Text());
			text.Trim();
			if (text.IsEmpty())
				break;
			BMessage add(kMsgAddTvList);
			add.AddString("address", text);
			fTarget.SendMessage(&add);
			fTvAddress->SetText("");
			fTvAdd->SetEnabled(false);
			break;
		}
		case kMsgTvCountry:
		{
			BString name(B_TRANSLATE("TV %country%"));
			name.ReplaceFirst("%country%", message->GetString("name", ""));
			BMessage add(kMsgAddTvList);
			add.AddString("address",
				TvLists::Address(message->GetString("code", "")));
			add.AddString("name", name);
			fTarget.SendMessage(&add);
			break;
		}
		case kMsgTvRemove:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fTvLists->ItemAt(fTvLists->CurrentSelection()));
			if (item == NULL)
				break;
			BMessage remove(kMsgRemoveTvList);
			remove.AddString("address", item->Text());
			fTarget.SendMessage(&remove);
			break;
		}

		case kMsgPlayInCinema:
			fSettings.playInCinema = fPlayInCinema->Value() == B_CONTROL_ON;
			fResume->SetEnabled(fSettings.playInCinema);
			fEvenVolume->SetEnabled(fSettings.playInCinema);
			fOverlay->SetEnabled(fSettings.playInCinema);
			_Changed();
			break;
		case kMsgOverlay:
			fSettings.overlayChoice = message->GetInt32("choice",
				kOverlayAutomatic);
			_Changed();
			break;
		case kMsgEvenVolume:
			fSettings.evenVolume = fEvenVolume->Value() == B_CONTROL_ON;
			_Changed();
			break;
		case kMsgResume:
			fSettings.resume = fResume->Value() == B_CONTROL_ON;
			_Changed();
			break;
		case kMsgSource:
			fSettings.searchSource = message->GetString("provider", "");
			_Changed();
			break;
		case kMsgQuality:
			fSettings.videoHeight = LimitVideoHeight(
				message->GetInt32("height", 0));
			_Changed();
			break;

		default:
			BWindow::MessageReceived(message);
	}
}
