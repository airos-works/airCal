// Small drawing helpers shared by airCal's custom views.
#pragma once

#include <Font.h>
#include <Rect.h>
#include <View.h>

#include <string>

namespace Drawing {

// Truncates with an ellipsis to fit width (B_TRUNCATE_END).
std::string Fit(BView* view, const std::string& text, float width);
float TextHeight(const BFont& font);	// ascent + descent
float Ascent(const BFont& font);

void FillRound(BView* view, BRect rect, float radius, rgb_color color);
void StrokeRound(BView* view, BRect rect, float radius, rgb_color color,
	float width = 1);
void FillCircle(BView* view, BPoint center, float radius, rgb_color color);
void StrokeCircle(BView* view, BPoint center, float radius, rgb_color color,
	float width = 1);

// A task checkbox: circle, filled with a check when done.
void DrawCheck(BView* view, BPoint center, float radius, rgb_color color,
	bool checked);
// Chevrons and glyphs drawn with lines (no icon fonts).
void DrawChevron(BView* view, BRect rect, bool left, rgb_color color);
void DrawPlus(BView* view, BRect rect, rgb_color color, float width = 1.6f);
void DrawRepeat(BView* view, BRect rect, rgb_color color);	// recurring mark
void DrawBell(BView* view, BRect rect, rgb_color color);
void DrawSync(BView* view, BRect rect, rgb_color color, float rotation = 0);
void DrawGear(BView* view, BRect rect, rgb_color color);
void DrawFlag(BView* view, BRect rect, rgb_color color);
void DrawSidebarIcon(BView* view, BRect rect, rgb_color color);
void DrawSearch(BView* view, BRect rect, rgb_color color);

// A text baseline that centres the font vertically in rect.
float Baseline(const BFont& font, BRect rect);

}	// namespace Drawing
