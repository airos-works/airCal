// The editor and settings windows. Each opens (or brings forward) its window;
// they talk to the store through TheApp() and post kMsgItemSaved to the main
// window after saving.
#pragma once

#include "core/Editing.h"

#include <string>

class BWindow;

// Edits an existing event / occurrence (ref.eventId set) or creates one from
// `draft` (ref.eventId empty; draft has calendarId, times, maybe title).
// `anchor` is the screen point the editor appears next to (popover style);
// pass a negative x to center it on the main window.
void OpenEventEditor(const aircal::OccurrenceRef& ref, const aircal::Event& draft,
	float anchorX = -1, float anchorY = -1);

// Edits an existing task (taskId) or creates one from `draft`.
void OpenTaskEditor(const std::string& taskId, const aircal::Task& draft,
	float anchorX = -1, float anchorY = -1);

void OpenAccountsWindow();
// Starts adding an account right away: "google", "caldav", "todoist".
void OpenAddAccount(const std::string& type);
void OpenSettingsWindow();

// Asks which occurrences a change applies to. Returns false when cancelled.
// `deleting` changes the wording ("Delete" vs "Save").
bool AskEditScope(BWindow* parent, bool deleting, aircal::EditScope& scope);
