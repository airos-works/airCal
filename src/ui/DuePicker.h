#pragma once

#include "core/DateTime.h"
#include <SupportDefs.h>

class BWindow;

// A choice is sent to the editor only when accepted.
void PickDueDate(BWindow* editor, const aircal::Date& date, int weekStart, uint32 reply);
void PickDueTime(BWindow* editor, int hour, int minute, uint32 reply);
