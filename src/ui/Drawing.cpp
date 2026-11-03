#include "Drawing.h"

#include <Shape.h>

#include <cmath>

namespace Drawing {

std::string
Fit(BView* view, const std::string& text, float width)
{
	if (width <= 4)
		return "";
	if (view->StringWidth(text.c_str()) <= width)
		return text;
	BString string(text.c_str());
	view->TruncateString(&string, B_TRUNCATE_END, width);
	return string.String();
}


float
TextHeight(const BFont& font)
{
	font_height height;
	font.GetHeight(&height);
	return ceilf(height.ascent + height.descent);
}


float
Ascent(const BFont& font)
{
	font_height height;
	font.GetHeight(&height);
	return ceilf(height.ascent);
}


float
Baseline(const BFont& font, BRect rect)
{
	font_height height;
	font.GetHeight(&height);
	return floorf(rect.top + (rect.Height() + 1 - height.ascent - height.descent)
		/ 2 + height.ascent);
}


void
FillRound(BView* view, BRect rect, float radius, rgb_color color)
{
	view->SetHighColor(color);
	if (radius <= 0)
		view->FillRect(rect);
	else
		view->FillRoundRect(rect, radius, radius);
}


void
StrokeRound(BView* view, BRect rect, float radius, rgb_color color, float width)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(width);
	view->StrokeRoundRect(rect, radius, radius);
	view->SetPenSize(old);
}


void
FillCircle(BView* view, BPoint center, float radius, rgb_color color)
{
	view->SetHighColor(color);
	view->FillEllipse(center, radius, radius);
}


void
StrokeCircle(BView* view, BPoint center, float radius, rgb_color color,
	float width)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(width);
	view->StrokeEllipse(center, radius, radius);
	view->SetPenSize(old);
}


void
DrawCheck(BView* view, BPoint center, float radius, rgb_color color,
	bool checked)
{
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	if (checked) {
		FillCircle(view, center, radius, color);
		view->SetHighColor(255, 255, 255);
		float old = view->PenSize();
		view->SetPenSize(std::max(1.5f, radius / 4.5f));
		view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
		BPoint a(center.x - radius * 0.45f, center.y + radius * 0.02f);
		BPoint b(center.x - radius * 0.1f, center.y + radius * 0.38f);
		BPoint c(center.x + radius * 0.5f, center.y - radius * 0.35f);
		view->StrokeLine(a, b);
		view->StrokeLine(b, c);
		view->SetPenSize(old);
	} else
		StrokeCircle(view, center, radius, color, std::max(1.2f, radius / 6.0f));
	view->SetDrawingMode(mode);
}


void
DrawChevron(BView* view, BRect rect, bool left, rgb_color color)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(1.8f);
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	BPoint center((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	float h = std::min(rect.Width(), rect.Height()) * 0.28f;
	float w = h * 0.55f;
	if (left) {
		view->StrokeLine(BPoint(center.x + w, center.y - h), BPoint(center.x - w, center.y));
		view->StrokeLine(BPoint(center.x - w, center.y), BPoint(center.x + w, center.y + h));
	} else {
		view->StrokeLine(BPoint(center.x - w, center.y - h), BPoint(center.x + w, center.y));
		view->StrokeLine(BPoint(center.x + w, center.y), BPoint(center.x - w, center.y + h));
	}
	view->SetDrawingMode(mode);
	view->SetPenSize(old);
}


void
DrawPlus(BView* view, BRect rect, rgb_color color, float width)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(width);
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	BPoint center((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	float h = std::min(rect.Width(), rect.Height()) * 0.32f;
	view->StrokeLine(BPoint(center.x - h, center.y), BPoint(center.x + h, center.y));
	view->StrokeLine(BPoint(center.x, center.y - h), BPoint(center.x, center.y + h));
	view->SetPenSize(old);
}


void
DrawRepeat(BView* view, BRect rect, rgb_color color)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(1.2f);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	BPoint center((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	float r = std::min(rect.Width(), rect.Height()) * 0.36f;
	view->StrokeArc(center, r, r, 30, 300);
	// Arrow head at the arc's start (30 degrees).
	float angle = 30 * M_PI / 180;
	BPoint tip(center.x + r * cosf(angle), center.y - r * sinf(angle));
	view->StrokeLine(tip, BPoint(tip.x + r * 0.55f, tip.y - r * 0.05f));
	view->StrokeLine(tip, BPoint(tip.x + r * 0.05f, tip.y - r * 0.6f));
	view->SetDrawingMode(mode);
	view->SetPenSize(old);
}


void
DrawBell(BView* view, BRect rect, rgb_color color)
{
	view->SetHighColor(color);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	float w = rect.Width(), h = rect.Height();
	BShape shape;
	shape.MoveTo(BPoint(rect.left + w * 0.2f, rect.top + h * 0.75f));
	shape.LineTo(BPoint(rect.left + w * 0.28f, rect.top + h * 0.35f));
	BPoint control[3] = {BPoint(rect.left + w * 0.32f, rect.top + h * 0.1f),
		BPoint(rect.left + w * 0.68f, rect.top + h * 0.1f),
		BPoint(rect.left + w * 0.72f, rect.top + h * 0.35f)};
	shape.BezierTo(control);
	shape.LineTo(BPoint(rect.left + w * 0.8f, rect.top + h * 0.75f));
	shape.Close();
	view->FillShape(&shape);
	view->FillEllipse(BPoint(rect.left + w * 0.5f, rect.top + h * 0.84f),
		w * 0.1f, h * 0.08f);
	view->SetDrawingMode(mode);
}


void
DrawSync(BView* view, BRect rect, rgb_color color, float rotation)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(1.6f);
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	BPoint center((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	float r = std::min(rect.Width(), rect.Height()) * 0.34f;
	for (int half = 0; half < 2; half++) {
		float start = rotation + half * 180 + 20;
		view->StrokeArc(center, r, r, start, 140);
		float end = (start + 140) * M_PI / 180;
		BPoint tip(center.x + r * cosf(end), center.y - r * sinf(end));
		float a1 = end - M_PI / 2 + 0.6f, a2 = end - M_PI / 2 - 0.9f;
		view->StrokeLine(tip, BPoint(tip.x + r * 0.5f * cosf(a1 + M_PI),
			tip.y - r * 0.5f * sinf(a1 + M_PI)));
		view->StrokeLine(tip, BPoint(tip.x + r * 0.5f * cosf(a2),
			tip.y - r * 0.5f * sinf(a2)));
	}
	view->SetDrawingMode(mode);
	view->SetPenSize(old);
}


void
DrawGear(BView* view, BRect rect, rgb_color color)
{
	view->SetHighColor(color);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	BPoint center((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	float r = std::min(rect.Width(), rect.Height()) * 0.42f;
	float old = view->PenSize();
	view->SetPenSize(r * 0.28f);
	for (int i = 0; i < 8; i++) {
		float a = i * M_PI / 4;
		view->StrokeLine(BPoint(center.x + r * 0.55f * cosf(a), center.y + r * 0.55f * sinf(a)),
			BPoint(center.x + r * cosf(a), center.y + r * sinf(a)));
	}
	view->SetPenSize(r * 0.25f);
	view->StrokeEllipse(center, r * 0.55f, r * 0.55f);
	view->SetPenSize(old);
	view->SetDrawingMode(mode);
}


void
DrawFlag(BView* view, BRect rect, rgb_color color)
{
	view->SetHighColor(color);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	float w = rect.Width(), h = rect.Height();
	view->FillRect(BRect(rect.left + w * 0.2f, rect.top + h * 0.1f,
		rect.left + w * 0.28f, rect.bottom - h * 0.05f));
	BShape shape;
	shape.MoveTo(BPoint(rect.left + w * 0.28f, rect.top + h * 0.1f));
	shape.LineTo(BPoint(rect.right - w * 0.1f, rect.top + h * 0.32f));
	shape.LineTo(BPoint(rect.left + w * 0.28f, rect.top + h * 0.56f));
	shape.Close();
	view->FillShape(&shape);
	view->SetDrawingMode(mode);
}


void
DrawSidebarIcon(BView* view, BRect rect, rgb_color color)
{
	view->SetHighColor(color);
	BRect frame = rect.InsetByCopy(rect.Width() * 0.12f, rect.Height() * 0.2f);
	view->StrokeRoundRect(frame, 2, 2);
	view->StrokeLine(BPoint(frame.left + frame.Width() * 0.36f, frame.top),
		BPoint(frame.left + frame.Width() * 0.36f, frame.bottom));
}


void
DrawSearch(BView* view, BRect rect, rgb_color color)
{
	view->SetHighColor(color);
	float old = view->PenSize();
	view->SetPenSize(1.5f);
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	float r = std::min(rect.Width(), rect.Height()) * 0.28f;
	BPoint center(rect.left + rect.Width() * 0.42f, rect.top + rect.Height() * 0.42f);
	view->StrokeEllipse(center, r, r);
	view->StrokeLine(BPoint(center.x + r * 0.7f, center.y + r * 0.7f),
		BPoint(center.x + r * 1.7f, center.y + r * 1.7f));
	view->SetDrawingMode(mode);
	view->SetPenSize(old);
}

}	// namespace Drawing
