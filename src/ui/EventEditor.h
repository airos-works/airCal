// Pieces shared by airCal's dialog windows (event and task editors, the
// accounts and settings windows): colour-dot menu items, the calendar /
// alert / repeat popups, popover placement and a registry that keeps one
// editor per item.
#pragma once

#include <MenuItem.h>
#include <View.h>

#include "core/Model.h"

#include <string>

class BMenu;
class BMessage;
class BTextControl;
class BWindow;

namespace DialogKit {

// A filled calendar colour dot with a thin separator-coloured ring.
void DrawColorDot(BView* view, BPoint center, float radius, uint32 color,
	bool enabled = true);

// A menu item with a calendar's colour dot before its label.
class ColorDotItem : public BMenuItem {
public:
	ColorDotItem(const char* label, uint32 color, BMessage* message);

protected:
	void GetContentSize(float* _width, float* _height) override;
	void DrawContent() override;

private:
	float DotSpace() const;

	uint32 fColor;
};

// A small view showing one colour dot (account window rows).
class ColorDotView : public BView {
public:
	explicit ColorDotView(uint32 color);

	void Draw(BRect updateRect) override;
	void MessageReceived(BMessage* message) override;
	BSize MinSize() override;
	BSize MaxSize() override;
	BSize PreferredSize() override;

private:
	uint32 fColor;
};

// The application's main window (as a BWindow), or NULL.
BWindow* MainBWindow();
// Posts a copy of the message to the main window.
void PostToMain(BMessage* message);

// Sizes the window to its layout and places it next to a screen point like
// a popover (to its right, or left when there is no room), keeping it on
// screen. A negative x centres it on the main window.
void PlaceWindow(BWindow* window, float anchorX, float anchorY);

// One window per key ("event:<id>@<occurrence>", "task:<id>", ...).
// Activate brings an open one forward and returns true.
bool ActivateRegistered(const std::string& key);
void Register(const std::string& key, BWindow* window);
void Unregister(const std::string& key, BWindow* window);
// Sends a copy of the message to the window registered under key.
bool PostToRegistered(const std::string& key, BMessage* message);

// Fills a calendar popup with the writable event calendars (tasks false) or
// task lists (tasks true), grouped under their account's name. Each item's
// message is `what` with "calendar" (string). `selected` is marked and is
// listed even when read-only. Returns the number of selectable items.
int AddCalendarItems(BMenu* menu, bool tasks, const std::string& selected,
	uint32 what);

// Alert choices: None (-1), at time of event, 5 minutes ... 1 week before,
// plus `current` when it is none of those. Messages carry "minutes" (int32).
void AddAlarmItems(BMenu* menu, int current, uint32 what);

// Repeat presets for a rule, plus "Custom: <description>" when the rule is
// not one of them. Messages carry "preset" (int32, aircal::RepeatPreset).
void AddRepeatItems(BMenu* menu, const std::string& rrule, uint32 what);

// Field text that ParseDate/ParseTime read back exactly.
std::string DateFieldText(const aircal::Date& date);
std::string TimeFieldText(int hour, int minute);

// Marks a text field's text as invalid (red tint) or valid.
void SetFieldValid(BTextControl* control, bool valid);

// Whether a URL is a video call link (Meet, Zoom, Teams, Webex, ...).
bool IsVideoCallUrl(const std::string& url);

// Writable-calendar lookups under the store lock.
aircal::AccountType AccountTypeOfCalendar(const std::string& calendarId);

std::string Trim(const std::string& text);

}	// namespace DialogKit
