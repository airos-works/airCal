#include "Theme.h"

#include <dlfcn.h>

namespace Theme {

namespace {

bool sDark = false;
rgb_color sDocument;
rgb_color sDocumentText;
rgb_color sPanel;
rgb_color sPanelText;
rgb_color sControl;
rgb_color sHighlight;


rgb_color
Make(int r, int g, int b, int a = 255)
{
	rgb_color color;
	color.red = (uint8)r;
	color.green = (uint8)g;
	color.blue = (uint8)b;
	color.alpha = (uint8)a;
	return color;
}

}	// namespace


float
Luminance(rgb_color c)
{
	return (0.2126f * c.red + 0.7152f * c.green + 0.0722f * c.blue) / 255.0f;
}


rgb_color
Mix(rgb_color a, rgb_color b, float amount)
{
	if (amount < 0)
		amount = 0;
	if (amount > 1)
		amount = 1;
	return Make((int)(a.red + (b.red - a.red) * amount + 0.5f),
		(int)(a.green + (b.green - a.green) * amount + 0.5f),
		(int)(a.blue + (b.blue - a.blue) * amount + 0.5f), 255);
}


rgb_color
WithAlpha(rgb_color color, uint8 alpha)
{
	color.alpha = alpha;
	return color;
}


void
Refresh()
{
	sDocument = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	sDocumentText = ui_color(B_DOCUMENT_TEXT_COLOR);
	sPanel = ui_color(B_PANEL_BACKGROUND_COLOR);
	sPanelText = ui_color(B_PANEL_TEXT_COLOR);
	sControl = ui_color(B_CONTROL_BACKGROUND_COLOR);
	sHighlight = ui_color(B_CONTROL_HIGHLIGHT_COLOR);

	// air/OS's switch: ui_dark_mode() where the system has it (looked up at
	// run time so the binary also runs on systems without it), otherwise
	// the palette tells.
	typedef bool (*DarkModeFunction)();
	static DarkModeFunction darkMode = (DarkModeFunction)dlsym(RTLD_DEFAULT,
		"_Z12ui_dark_modev");
	bool paletteDark = Luminance(sDocument) < 0.45f;
	sDark = darkMode != nullptr ? (darkMode() || paletteDark) : paletteDark;
	// A dark switch with a light custom document color still reads as the
	// palette says; trust the actual colors for contrast.
	if (darkMode != nullptr && darkMode() && !paletteDark)
		sDark = Luminance(sPanel) < 0.45f;
}


bool
IsDark()
{
	return sDark;
}


rgb_color
Background()
{
	if (sDark)
		return Mix(sDocument, Make(0, 0, 0), 0.0f);
	return Make(255, 255, 255);
}


rgb_color
Sidebar()
{
	return sDark ? Mix(sPanel, Make(0, 0, 0), 0.18f)
		: Mix(sPanel, Make(255, 255, 255), 0.45f);
}


rgb_color
Header()
{
	return sDark ? Mix(sPanel, Make(0, 0, 0), 0.05f)
		: Mix(sPanel, Make(255, 255, 255), 0.7f);
}


rgb_color
Text()
{
	return sDark ? Mix(sDocumentText, Make(255, 255, 255), 0.2f)
		: Make(28, 28, 30);
}


rgb_color
SecondaryText()
{
	return sDark ? Make(170, 170, 176) : Make(110, 110, 115);
}


rgb_color
TertiaryText()
{
	return sDark ? Make(130, 130, 136) : Make(150, 150, 155);
}


rgb_color
GridLine()
{
	return sDark ? Mix(Background(), Make(255, 255, 255), 0.10f)
		: Make(229, 229, 234);
}


rgb_color
GridLineStrong()
{
	return sDark ? Mix(Background(), Make(255, 255, 255), 0.18f)
		: Make(205, 205, 212);
}


rgb_color
HalfHourLine()
{
	return sDark ? Mix(Background(), Make(255, 255, 255), 0.05f)
		: Make(242, 242, 245);
}


rgb_color
Weekend()
{
	return sDark ? Mix(Background(), Make(255, 255, 255), 0.03f)
		: Make(249, 249, 251);
}


rgb_color
OtherMonth()
{
	return sDark ? Mix(Background(), Make(0, 0, 0), 0.25f) : Make(247, 247, 249);
}


rgb_color
Selection()
{
	return sDark ? Mix(Background(), Accent(), 0.16f)
		: Mix(Background(), Accent(), 0.08f);
}


rgb_color
Hover()
{
	return sDark ? Mix(Background(), Make(255, 255, 255), 0.06f)
		: Make(244, 244, 247);
}


rgb_color
Accent()
{
	return sDark ? Make(255, 85, 75) : Make(232, 62, 52);
}


rgb_color
AccentText()
{
	return Make(255, 255, 255);
}


rgb_color
Control()
{
	return sDark ? Mix(sPanel, Make(255, 255, 255), 0.08f)
		: Mix(sPanel, Make(0, 0, 0), 0.06f);
}


rgb_color
ControlSelected()
{
	return sDark ? Mix(sPanel, Make(255, 255, 255), 0.28f) : Make(255, 255, 255);
}


rgb_color
Separator()
{
	return sDark ? Mix(sPanel, Make(0, 0, 0), 0.35f)
		: Mix(sPanel, Make(0, 0, 0), 0.14f);
}


rgb_color
Highlight()
{
	return sHighlight;
}


rgb_color
FromHex(uint32_t color)
{
	return Make((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}


rgb_color
EventFill(uint32_t hex, bool selected)
{
	rgb_color color = FromHex(hex);
	if (selected)
		return sDark ? Mix(color, Make(0, 0, 0), 0.1f) : color;
	return sDark ? Mix(Background(), color, 0.32f) : Mix(Background(), color, 0.17f);
}


rgb_color
EventBar(uint32_t hex)
{
	rgb_color color = FromHex(hex);
	return sDark ? Mix(color, Make(255, 255, 255), 0.1f) : color;
}


rgb_color
EventText(uint32_t hex, bool selected)
{
	rgb_color color = FromHex(hex);
	if (selected)
		return Luminance(color) > 0.7f ? Make(20, 20, 20) : Make(255, 255, 255);
	if (sDark)
		return Mix(color, Make(255, 255, 255), 0.62f);
	// Darken until readable on the light fill.
	rgb_color text = Mix(color, Make(0, 0, 0), 0.45f);
	return text;
}


rgb_color
AllDayFill(uint32_t hex, bool selected)
{
	rgb_color color = FromHex(hex);
	if (selected)
		return sDark ? Mix(color, Make(255, 255, 255), 0.1f)
			: Mix(color, Make(0, 0, 0), 0.12f);
	return sDark ? Mix(Background(), color, 0.55f) : Mix(Background(), color, 0.28f);
}


rgb_color
AllDayText(uint32_t hex, bool selected)
{
	if (selected)
		return Make(255, 255, 255);
	return EventText(hex, false);
}


rgb_color
Priority(int priority)
{
	switch (priority) {
		case 4: return sDark ? Make(255, 99, 90) : Make(209, 52, 56);
		case 3: return sDark ? Make(255, 170, 70) : Make(235, 137, 0);
		case 2: return sDark ? Make(110, 160, 255) : Make(36, 111, 224);
		case 1: return SecondaryText();
		default: return TertiaryText();
	}
}

}	// namespace Theme
