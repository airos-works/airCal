#pragma once

#include <GroupView.h>

#include "core/Store.h"

class DateNavigator;
class MainWindow;
namespace BMobileKit {
	class MetalBar;
	class MetalButton;
	class SegmentedControl;
	class TableView;
}

class MobilePlanner : public BGroupView {
public:
	explicit MobilePlanner(MainWindow* owner);
	void ShowTasks(bool tasks);
	void Reload(bool resetScroll = false);
	aircal::Date Step(int direction) const;
	void FrameResized(float width, float height) override;
	void AttachedToWindow() override;
	void MessageReceived(BMessage* message) override;

private:
	void LoadAgenda();
	void LoadTasks();
	void LoadListSidebar();

	MainWindow* fOwner;
	BGroupView* fBody;
	DateNavigator* fDates;
	BMobileKit::MetalBar* fTaskHeader;
	BMobileKit::SegmentedControl* fFilter;
	BMobileKit::SegmentedControl* fTabs;
	BMobileKit::MetalButton* fAdd;
	BMobileKit::MetalButton* fMore;
	BMobileKit::TableView* fList;
	BMobileKit::TableView* fLists;
	BView* fListSidebar;
	std::string fSelectedList;
	bool fTasks = false;
	bool fTablet = false;
};
