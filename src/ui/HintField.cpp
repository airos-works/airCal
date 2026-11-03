#include "HintField.h"

#include "Drawing.h"
#include "Theme.h"

#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>

namespace {

class HintTextView : public BTextView {
public:
	HintTextView(HintField* owner)
		:
		BTextView("text", B_WILL_DRAW | B_NAVIGABLE),
		fOwner(owner)
	{
		SetWordWrap(false);
		DisallowChar('\n');
		SetStylable(false);
	}

	void Draw(BRect update) override
	{
		BTextView::Draw(update);
		if (TextLength() == 0 && !fOwner->Hint().empty()) {
			// BTextView leaves a clipping region behind; draw unclipped
			// at the text's own origin.
			PushState();
			ConstrainClippingRegion(nullptr);
			SetHighColor(Theme::TertiaryText());
			SetLowColor(ViewColor());
			SetDrawingMode(B_OP_OVER);
			font_height height;
			GetFontHeight(&height);
			DrawString(fOwner->Hint().c_str(), BPoint(4,
				2 + ceilf(height.ascent)));
			PopState();
		}
	}

	void KeyDown(const char* bytes, int32 count) override
	{
		if (bytes[0] == B_ENTER) {
			fOwner->Invoke();
			return;
		}
		if (bytes[0] == B_ESCAPE) {
			fOwner->Cancel();
			return;
		}
		if (bytes[0] == B_TAB) {
			BView::KeyDown(bytes, count);
			return;
		}
		BTextView::KeyDown(bytes, count);
	}

	void MakeFocus(bool focus) override
	{
		BTextView::MakeFocus(focus);
		fOwner->FocusChanged();
	}

protected:
	void InsertText(const char* text, int32 length, int32 offset,
		const text_run_array* runs) override
	{
		BTextView::InsertText(text, length, offset, runs);
		fOwner->TextChanged();
	}

	void DeleteText(int32 start, int32 finish) override
	{
		BTextView::DeleteText(start, finish);
		fOwner->TextChanged();
	}

private:
	HintField* fOwner;
};

}	// namespace


HintField::HintField(const char* name, const char* hint, BMessage* invoke,
	BMessage* modified, Glyph glyph)
	:
	BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
	fHint(hint != nullptr ? hint : ""),
	fInvoke(invoke),
	fModified(modified),
	fGlyph(glyph)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	fTextView = new HintTextView(this);
	AddChild(fTextView);
}


HintField::~HintField()
{
	delete fInvoke;
	delete fModified;
}


const char*
HintField::Text() const
{
	return fTextView->Text();
}


void
HintField::SetText(const char* text)
{
	fTextView->SetText(text);
	fTextView->Invalidate();
}


void
HintField::SetHint(const char* hint)
{
	fHint = hint != nullptr ? hint : "";
	fTextView->Invalidate();
}


void
HintField::SetTarget(BMessenger target)
{
	fTarget = target;
}


void
HintField::MakeFocus(bool focus)
{
	fTextView->MakeFocus(focus);
}


void
HintField::SelectAll()
{
	fTextView->SelectAll();
}


BSize
HintField::MinSize()
{
	BFont font;
	GetFont(&font);
	return BSize(80, ceilf(Drawing::TextHeight(font) + 12));
}


BSize
HintField::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, MinSize().height);
}


BSize
HintField::PreferredSize()
{
	return BSize(200, MinSize().height);
}


void
HintField::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (!fTarget.IsValid())
		fTarget = BMessenger(Parent() != nullptr ? (BHandler*)Parent()
			: (BHandler*)Window());
	LayoutText();
	FocusChanged();
}


void
HintField::FrameResized(float width, float height)
{
	LayoutText();
	Invalidate();
}


void
HintField::LayoutText()
{
	BRect bounds = Bounds();
	float left = fGlyph == kNone ? 8 : 26;
	BFont font;
	GetFont(&font);
	float height = Drawing::TextHeight(font) + 4;
	float top = floorf((bounds.Height() - height) / 2);
	fTextView->MoveTo(left, top);
	fTextView->ResizeTo(std::max(10.0f, bounds.Width() - left - 6), height);
	BRect text(0, 0, std::max(10.0f, bounds.Width() - left - 6), height);
	text.InsetBy(3, 2);
	fTextView->SetTextRect(text);
}


void
HintField::FocusChanged()
{
	rgb_color background = Theme::IsDark()
		? Theme::Mix(Theme::Sidebar(), Theme::Text(), 0.08f) : Theme::Background();
	fTextView->SetViewColor(background);
	fTextView->SetLowColor(background);
	rgb_color text = Theme::Text();
	fTextView->SetFontAndColor(nullptr, 0, &text);
	fTextView->Invalidate();
	Invalidate();
}


void
HintField::Draw(BRect update)
{
	// Follow palette changes (light/dark switch) without a focus change.
	rgb_color wanted = Theme::IsDark()
		? Theme::Mix(Theme::Sidebar(), Theme::Text(), 0.08f) : Theme::Background();
	if (fTextView->ViewColor() != wanted)
		FocusChanged();
	BRect bounds = Bounds();
	SetHighColor(Parent() != nullptr ? Parent()->ViewColor() : Theme::Sidebar());
	if (Parent() != nullptr && Parent()->ViewColor() == B_TRANSPARENT_COLOR)
		SetHighColor(Theme::Sidebar());
	FillRect(bounds);
	SetDrawingMode(B_OP_ALPHA);
	BRect frame = bounds.InsetByCopy(1, 1);
	rgb_color background = Theme::IsDark()
		? Theme::Mix(Theme::Sidebar(), Theme::Text(), 0.08f) : Theme::Background();
	Drawing::FillRound(this, frame, 7, background);
	bool focused = fTextView->IsFocus() && Window() != nullptr && Window()->IsActive();
	Drawing::StrokeRound(this, frame, 7, focused ? Theme::Highlight()
		: Theme::Separator(), focused ? 1.6f : 1.0f);
	if (fGlyph == kSearch) {
		Drawing::DrawSearch(this, BRect(8, bounds.Height() / 2 - 8, 24,
			bounds.Height() / 2 + 8), Theme::TertiaryText());
	} else if (fGlyph == kPlus) {
		Drawing::DrawPlus(this, BRect(7, bounds.Height() / 2 - 8, 23,
			bounds.Height() / 2 + 8), Theme::Accent(), 1.8f);
	}
	SetDrawingMode(B_OP_COPY);
}


void
HintField::MouseDown(BPoint where)
{
	fTextView->MakeFocus(true);
}


void
HintField::TextChanged()
{
	if (fModified != nullptr && fTarget.IsValid())
		fTarget.SendMessage(fModified);
}


void
HintField::Invoke()
{
	if (fInvoke != nullptr && fTarget.IsValid())
		fTarget.SendMessage(fInvoke);
}


void
HintField::Cancel()
{
	if (fTextView->TextLength() > 0)
		SetText("");
	TextChanged();
}
