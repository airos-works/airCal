// Message codes shared by airCal's windows and views.
#pragma once

#include <SupportDefs.h>

enum {
	// Store / sync -> windows
	kMsgStoreChanged = 'stch',		// the store changed (coalesced)
	kMsgSyncStatus = 'syst',		// "account", "running", "message", "error", "auth"

	// Navigation / views
	kMsgSetView = 'svew',			// "view": "day" | "week" | "month" | "year" | "tasks"
	kMsgGoToday = 'gtdy',
	kMsgGoNext = 'gnxt',
	kMsgGoPrevious = 'gprv',
	kMsgGoToDate = 'gdat',			// "date": "YYYY-MM-DD", optional "view"
	kMsgSelectDate = 'sdat',		// "date": selection changed (mini month)
	kMsgToggleSidebar = 'tsbr',
	kMsgSearchChanged = 'srch',
	kMsgShowCompleted = 'shcp',
	kMsgTaskFilter = 'tflt',		// "filter": "today" | "upcoming" | "all" | "completed" | "list:<id>"

	// Items
	kMsgNewEvent = 'nevt',			// optional "start" (int64), "end", "allDay" (bool), "date"
	kMsgNewTask = 'ntsk',			// optional "date", "list"
	kMsgEditEvent = 'eevt',			// "event" (master id), optional "override",
									// "original" (int64), "originalDate" (string)
	kMsgEditTask = 'etsk',			// "task"
	kMsgDeleteSelection = 'dsel',
	kMsgToggleTask = 'ttsk',		// "task", "completed" (bool)
	kMsgQuickEntry = 'qent',		// "text"
	kMsgQuickEntryModified = 'qemd',
	kMsgMoveEvent = 'mvev',			// "event", "override", "original", "originalDate",
									// "start", "end" (int64), "allDay" (bool), "date"
	kMsgItemSaved = 'isvd',			// an editor saved; "kind", "id"
	kMsgDuplicateEvent = 'dupe',
	kMsgEventToCalendar = 'evcl',	// "event", "calendar"

	// Calendars / accounts
	kMsgToggleCalendar = 'tcal',	// "calendar", "visible" (bool)
	kMsgShowAccounts = 'sacc',
	kMsgShowSettings = 'sset',
	kMsgSyncNow = 'sync',
	kMsgAccountAdded = 'aadd',		// "account"
	kMsgImport = 'impt',
	kMsgExport = 'expt',
	kMsgPrefsChanged = 'pchg',

	// Appearance
	kMsgThemeChanged = 'thch'
};
