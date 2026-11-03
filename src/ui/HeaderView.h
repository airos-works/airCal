// The calendar area's header: big month title, navigation, the view
// switcher, search and the add button (Fantastical's toolbar).
#pragma once

#include <View.h>

#include <string>

class HintField;

class HeaderView : public BView {
public:
	HeaderView();

	void SetTitle(const std::string& bold, const std::string& light);
	void SetMode(const std::string& mode);
	void SetSync(bool running, bool error, const std::string& tip);
	void FocusSearch();
	void ClearSearch();

	void AttachedToWindow() override;
	void Draw(BRect update) override;
	void FrameResized(float width, float height) override;
	void MouseDown(BPoint where) override;
	void MouseUp(BPoint where) override;
	void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
	void MessageReceived(BMessage* message) override;
	void Pulse() override;
	BSize MinSize() override;
	BSize PreferredSize() override;
	BSize MaxSize() override;

private:
	enum Part { kNone = -1, kSidebar = 0, kPrevious, kToday, kNext, kDay,
		kWeek, kMonth, kYear, kTasks, kAdd, kSync, kPartCount };
	void Layout();
	int PartAt(BPoint where) const;

	HintField* fSearch = nullptr;
	std::string fBold;
	std::string fLight;
	std::string fMode = "week";
	BRect fParts[kPartCount];
	int fPressed = kNone;
	int fHover = kNone;
	bool fSyncRunning = false;
	bool fSyncError = false;
	float fSyncAngle = 0;
	std::string fTip;
};
