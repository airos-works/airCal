// A single-line text field with placeholder (hint) text and an optional
// leading glyph, drawn to match airCal's look in light and dark mode.
#pragma once

#include <Messenger.h>
#include <View.h>

#include <string>

class BTextView;

class HintField : public BView {
public:
	enum Glyph { kNone, kSearch, kPlus };

	HintField(const char* name, const char* hint, BMessage* invoke,
		BMessage* modified = nullptr, Glyph glyph = kNone);
	~HintField() override;

	const char* Text() const;
	void SetText(const char* text);
	void SetHint(const char* hint);
	void SetTarget(BMessenger target);
	BTextView* TextView() const { return fTextView; }
	void MakeFocus(bool focus = true) override;
	void SelectAll();

	void AttachedToWindow() override;
	void Draw(BRect update) override;
	void FrameResized(float width, float height) override;
	void MouseDown(BPoint where) override;
	BSize MinSize() override;
	BSize MaxSize() override;
	BSize PreferredSize() override;

	// Called by the text view.
	void TextChanged();
	void Invoke();
	void FocusChanged();
	void Cancel();
	const std::string& Hint() const { return fHint; }

private:
	void LayoutText();

	BTextView* fTextView;
	std::string fHint;
	BMessage* fInvoke;
	BMessage* fModified;
	BMessenger fTarget;
	Glyph fGlyph;
};
