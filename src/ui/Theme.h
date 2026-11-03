// airCal's colors, derived from the system's UI colors so the app follows
// air/OS's light/dark switch (and any custom palette). Views call
// Theme::Refresh() on B_COLORS_UPDATED and redraw.
#pragma once

#include <GraphicsDefs.h>
#include <InterfaceDefs.h>

#include <cstdint>

namespace Theme {

void Refresh();
bool IsDark();

// Surfaces
rgb_color Background();		// calendar canvas (document background)
rgb_color Sidebar();		// sidebar background
rgb_color Header();			// header/toolbar background
rgb_color Text();			// primary text on Background
rgb_color SecondaryText();	// dimmer text
rgb_color TertiaryText();	// hour labels, weekday names
rgb_color GridLine();		// hour/day lines
rgb_color GridLineStrong();	// month separators
rgb_color HalfHourLine();
rgb_color Weekend();		// weekend column/cell tint
rgb_color OtherMonth();		// month view days outside the month
rgb_color Selection();		// selected day/cell
rgb_color Hover();
rgb_color Accent();			// Fantastical red: today, now line
rgb_color AccentText();		// text on Accent
rgb_color Control();		// segmented control background
rgb_color ControlSelected();
rgb_color Separator();
rgb_color Highlight();		// keyboard focus / selection (system)

// Item colors for a calendar color (0xRRGGBB).
rgb_color FromHex(uint32_t color);
rgb_color EventFill(uint32_t color, bool selected = false);
rgb_color EventBar(uint32_t color);
rgb_color EventText(uint32_t color, bool selected = false);
rgb_color AllDayFill(uint32_t color, bool selected = false);
rgb_color AllDayText(uint32_t color, bool selected = false);

// Priority flag colors (1 low .. 4 urgent).
rgb_color Priority(int priority);

rgb_color Mix(rgb_color a, rgb_color b, float amount);	// amount of b
rgb_color WithAlpha(rgb_color color, uint8 alpha);
float Luminance(rgb_color color);

}	// namespace Theme
