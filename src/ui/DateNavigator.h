// A week strip or month page, shared by the touch calendar and due-date picker.
#pragma once

#include "core/DateTime.h"

#include <GroupView.h>
#include <Messenger.h>

#include <map>

namespace BMobileKit { class TitleView; }
class DateTiles;

class DateNavigator : public BGroupView {
public:
	DateNavigator(const aircal::Date& date, bool month, int weekStart,
		BHandler* target, uint32 choose, bool browseSelects = false);
	void SetDate(const aircal::Date& date);
	void SetMonthLayout(bool month);
	void SetWeekStart(int first);
	void SetMarkers(std::map<int64_t, uint32> markers);
	aircal::Date FirstDate() const;
	int DayCount() const { return fMonth ? 42 : 7; }
	void MessageReceived(BMessage* message) override;
	void AttachedToWindow() override;

private:
	void Choose(const aircal::Date& date);
	friend class DateTiles;
	BMessenger fTarget;
	uint32 fChoose;
	bool fBrowseSelects;
	aircal::Date fDate;
	bool fMonth;
	int fWeekStart;
	BMobileKit::TitleView* fTitle;
	DateTiles* fTiles;
	std::map<int64_t, uint32> fMarkers;
};
