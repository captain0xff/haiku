/*
 * Copyright 2003-2014, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Sikosis, Jérôme Duval
 *		yourpalal, Alex Wilson
 */
#ifndef MEDIA_WINDOW_H
#define MEDIA_WINDOW_H


#include <Alert.h>
#include <ListView.h>
#include <MediaAddOn.h>
#include <ParameterWeb.h>
#include <StringView.h>
#include <Window.h>

#include <ObjectList.h>

#include "MediaIcons.h"
#include "MediaListItem.h"
#include "MediaViews.h"

#define SETTINGS_FILE "MediaPrefs Settings"


class BCardLayout;
class BSeparatorView;
class MidiSettingsView;

class MediaWindow : public BWindow {
public:
								MediaWindow(BRect frame);
								~MediaWindow();

			status_t			InitCheck();

	// methods to be called by MediaListItems...
			void				SelectNode(const dormant_node_info* node);
			void				SelectAudioSettings(const char* title);
			void				SelectVideoSettings(const char* title);
			void				SelectAudioMixer(const char* title);
			void				SelectMidiSettings(const char* title);

	// methods to be called by SettingsViews...
			void				UpdateInputListItem(
									MediaListItem::media_type type,
									const dormant_node_info* node);
			void				UpdateOutputListItem(
									MediaListItem::media_type type,
									const dormant_node_info* node);

	virtual	bool 				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

private:
	typedef BObjectList<dormant_node_info, true> NodeList;

			void				_InitWindow();
			status_t			_InitMedia(bool first);

			void				_FindNodes();
			void				_FindNodes(media_type type, uint64 kind,
									NodeList& into);
			void				_AddNodeItems(NodeList &from,
									MediaListItem::media_type type);
			void				_EmptyNodeLists();
			void				_UpdateListViewMinWidth();

			NodeListItem*		_FindNodeListItem(dormant_node_info* info);

	static	status_t			_RestartMediaServices(void* data);

			void				_ClearParamView();
			void				_MakeParamView();
			void				_MakeEmptyParamView();

	struct SmartNode {
								SmartNode(const BMessenger& notifyHandler);
								~SmartNode();

			void				SetTo(const dormant_node_info* node);
			void				SetTo(const media_node& node);
			bool				IsSet();
								operator media_node();

	private:
			void				_FreeNode();
			media_node*			fNode;
			BMessenger			fMessenger;
	};

			BListView*			fListView;
			BSeparatorView*		fTitleView;
			BCardLayout*		fContentLayout;
			AudioSettingsView*	fAudioView;
			VideoSettingsView*	fVideoView;
			MidiSettingsView*	fMidiView;

			SmartNode			fCurrentNode;
			BParameterWeb*		fParamWeb;


			NodeList			fAudioInputs;
			NodeList			fAudioOutputs;
			NodeList			fVideoInputs;
			NodeList			fVideoOutputs;

			status_t			fInitCheck;
			thread_id			fRestartThread;
			BAlert*				fRestartAlert;
};


#endif	// MEDIA_WINDOW_H
