// Switches the system UI palette between air/OS's light and dark defaults
// (the dark one from the air/OS dark-mode branch), so airCal's live theme
// switching can be tested on systems without the dark switch:
//   theme-test dark | light
// Running applications get B_COLORS_UPDATED, as with the real switch.
#include <Application.h>
#include <InterfaceDefs.h>
#include <Message.h>

#include <cstdio>
#include <cstring>

struct Entry {
	color_which which;
	rgb_color dark;
	rgb_color light;
};

static const Entry kEntries[] = {
	{B_PANEL_BACKGROUND_COLOR, {53, 53, 53, 255}, {216, 216, 216, 255}},
	{B_MENU_BACKGROUND_COLOR, {53, 53, 53, 255}, {216, 216, 216, 255}},
	{B_WINDOW_TAB_COLOR, {237, 199, 70, 255}, {255, 203, 0, 255}},
	{B_KEYBOARD_NAVIGATION_COLOR, {96, 150, 255, 255}, {0, 0, 229, 255}},
	{B_MENU_SELECTED_BACKGROUND_COLOR, {90, 90, 90, 255}, {115, 120, 184, 255}},
	{B_MENU_ITEM_TEXT_COLOR, {216, 216, 216, 255}, {0, 0, 0, 255}},
	{B_MENU_SELECTED_ITEM_TEXT_COLOR, {240, 240, 240, 255}, {255, 255, 255, 255}},
	{B_PANEL_TEXT_COLOR, {216, 216, 216, 255}, {0, 0, 0, 255}},
	{B_DOCUMENT_BACKGROUND_COLOR, {36, 36, 36, 255}, {255, 255, 255, 255}},
	{B_DOCUMENT_TEXT_COLOR, {222, 222, 222, 255}, {0, 0, 0, 255}},
	{B_CONTROL_BACKGROUND_COLOR, {70, 70, 70, 255}, {245, 245, 245, 255}},
	{B_CONTROL_TEXT_COLOR, {222, 222, 222, 255}, {0, 0, 0, 255}},
	{B_CONTROL_BORDER_COLOR, {110, 110, 110, 255}, {0, 0, 0, 255}},
	{B_CONTROL_HIGHLIGHT_COLOR, {75, 124, 168, 255}, {115, 120, 184, 255}},
	{B_TOOL_TIP_BACKGROUND_COLOR, {70, 67, 52, 255}, {255, 255, 222, 255}},
	{B_TOOL_TIP_TEXT_COLOR, {230, 230, 230, 255}, {0, 0, 0, 255}},
	{B_CONTROL_MARK_COLOR, {120, 170, 230, 255}, {27, 82, 140, 255}},
	{B_LIST_BACKGROUND_COLOR, {36, 36, 36, 255}, {255, 255, 255, 255}},
	{B_LIST_SELECTED_BACKGROUND_COLOR, {80, 80, 80, 255}, {190, 190, 190, 255}},
	{B_LIST_ITEM_TEXT_COLOR, {222, 222, 222, 255}, {0, 0, 0, 255}},
	{B_LIST_SELECTED_ITEM_TEXT_COLOR, {240, 240, 240, 255}, {0, 0, 0, 255}},
	{B_SCROLL_BAR_THUMB_COLOR, {70, 70, 70, 255}, {216, 216, 216, 255}},
};


int
main(int argc, char** argv)
{
	if (argc != 2 || (strcmp(argv[1], "dark") != 0 && strcmp(argv[1], "light") != 0)) {
		fprintf(stderr, "usage: theme-test dark|light\n");
		return 1;
	}
	bool dark = strcmp(argv[1], "dark") == 0;
	BApplication app("application/x-vnd.airos-airCal-theme-test");
	BMessage colors;
	for (const Entry& entry : kEntries) {
		colors.AddColor(ui_color_name(entry.which),
			dark ? entry.dark : entry.light);
	}
	set_ui_colors(&colors);
	return 0;
}
