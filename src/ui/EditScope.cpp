// Asks which occurrences of a recurring series a change applies to: a small
// modal window with one button per scope (BAlert has room for three buttons
// only, and this needs a Cancel too).
#include "Dialogs.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <Window.h>

#include <OS.h>

using aircal::EditScope;

namespace {

enum {
	kMsgThis = 'scth',
	kMsgFuture = 'scfu',
	kMsgAll = 'scal',
	kMsgCancelScope = 'sccn'
};


class ScopeWindow : public BWindow {
public:
	ScopeWindow(BWindow* parent, bool deleting);
	~ScopeWindow() override;

	// Shows the window and waits; -1 when cancelled, else the scope.
	int32 Go();

	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;

private:
	void Done(int32 result);

	sem_id fDone;
	int32 fResult = -1;
	bool fFinished = false;
};


ScopeWindow::ScopeWindow(BWindow* parent, bool deleting)
	:
	BWindow(BRect(0, 0, 300, 160), deleting ? "Delete Event" : "Save Event",
		B_MODAL_WINDOW_LOOK, parent != NULL ? B_MODAL_SUBSET_WINDOW_FEEL
			: B_MODAL_APP_WINDOW_FEEL,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_CLOSABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE
			| B_ASYNCHRONOUS_CONTROLS)
{
	fDone = create_sem(0, "edit scope");
	if (parent != NULL)
		AddToSubset(parent);

	BStringView* title = new BStringView("title", "This is a repeating event.");
	title->SetFont(be_bold_font);
	BStringView* question = new BStringView("question", deleting
		? "Delete only this occurrence, or the series?"
		: "Save the changes for this occurrence only, or for the series?");

	BButton* thisOne = new BButton("this", deleting ? "Delete This Event"
		: "Save for This Event", new BMessage(kMsgThis));
	BButton* future = new BButton("future", deleting
		? "Delete All Future Events" : "Save for Future Events",
		new BMessage(kMsgFuture));
	BButton* all = new BButton("all", deleting ? "Delete All Events"
		: "Save for All Events", new BMessage(kMsgAll));
	BButton* cancel = new BButton("cancel", "Cancel",
		new BMessage(kMsgCancelScope));
	for (BButton* button : {thisOne, future, all})
		button->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(title)
		.Add(question)
		.AddStrut(B_USE_SMALL_SPACING)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.Add(thisOne)
			.Add(future)
			.Add(all)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
		.End();

	SetDefaultButton(thisOne);
	ResizeToPreferred();
	if (parent != NULL)
		CenterIn(parent->Frame());
	else
		CenterOnScreen();
	MoveOnScreen(B_DO_NOT_RESIZE_TO_FIT);
}


ScopeWindow::~ScopeWindow()
{
	delete_sem(fDone);
}


int32
ScopeWindow::Go()
{
	Show();

	// Like BAlert::Go(): when called from a window's thread, keep that
	// window drawing while waiting.
	BWindow* caller = dynamic_cast<BWindow*>(
		BLooper::LooperForThread(find_thread(NULL)));
	sem_id done = fDone;
	if (caller != NULL) {
		status_t status;
		do {
			caller->UpdateIfNeeded();
			status = acquire_sem_etc(done, 1, B_RELATIVE_TIMEOUT, 50000);
		} while (status == B_TIMED_OUT || status == B_INTERRUPTED);
	} else {
		while (acquire_sem(done) == B_INTERRUPTED)
			;
	}

	int32 result = -1;
	if (Lock()) {
		result = fResult;
		Quit();
	}
	return result;
}


void
ScopeWindow::Done(int32 result)
{
	if (fFinished)
		return;
	fFinished = true;
	fResult = result;
	release_sem(fDone);
}


void
ScopeWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgThis:
			Done((int32)EditScope::This);
			break;
		case kMsgFuture:
			Done((int32)EditScope::Future);
			break;
		case kMsgAll:
			Done((int32)EditScope::All);
			break;
		case kMsgCancelScope:
			Done(-1);
			break;
		default:
			BWindow::MessageReceived(message);
	}
}


bool
ScopeWindow::QuitRequested()
{
	// Escape: cancel; Go() closes the window.
	Done(-1);
	return false;
}

}	// namespace


bool
AskEditScope(BWindow* parent, bool deleting, EditScope& scope)
{
	ScopeWindow* window = new ScopeWindow(parent, deleting);
	int32 result = window->Go();
	if (result < 0)
		return false;
	scope = (EditScope)result;
	return true;
}
