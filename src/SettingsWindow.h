/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef SETTINGS_WINDOW_H
#define SETTINGS_WINDOW_H


#include <Messenger.h>
#include <Window.h>

#include <vector>

#include "Provider.h"
#include "Settings.h"


class BButton;
class BCheckBox;
class BFilePanel;
class BListView;
class BMenuField;
class BStringView;
class BTextControl;


/*!	The settings of Cinema: the key of TMDB, the folders of the library,
	the lists of TV channels, how the videos play, where a search goes
	first. What is changed is kept at once, and told to the main window
	(the target): kMsgSettingsChanged, kMsgAddFolder and kMsgRemoveFolder
	("path"), kMsgAddTvList and kMsgRemoveTvList ("address"),
	kMsgCheckTmdb, whose answer, kMsgTmdbChecked, it sends back here. It
	sends kMsgSettingsFolders when the folders or the lists change, and
	kMsgSettingsClosed when this goes away.
*/
class SettingsWindow : public BWindow {
public:
								SettingsWindow(const BMessenger& target,
									const std::vector<Provider>& providers,
									const std::vector<BString>& folders);
	virtual						~SettingsWindow();

	virtual	void				Show();
	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_SetFolders(const BMessage* message);
			void				_Changed();

			BMessenger			fTarget;
			Settings			fSettings;

			BTextControl*		fKey;
			BButton*			fCheck;
			BStringView*		fKeyStatus;
			BListView*			fFolders;
			BButton*			fRemove;
			BListView*			fTvLists;
			BTextControl*		fTvAddress;
			BButton*			fTvAdd;
			BButton*			fTvRemove;
			BMenuField*			fTvCountry;
			BCheckBox*			fPlayInCinema;
			BCheckBox*			fResume;
			BCheckBox*			fEvenVolume;
			BMenuField*			fQuality;
			BMenuField*			fSource;
			BFilePanel*			fPanel;
};


#endif	// SETTINGS_WINDOW_H
