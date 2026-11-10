// The accounts window (accounts on the left, the selected one's calendars
// and actions on the right) and the sign-in windows that add Google, CalDAV
// and Todoist accounts. Sign-in work runs on a worker thread that reports
// back with a message; it never touches views and is joined (after being
// cancelled) before its window goes away.
#include "EventEditor.h"

#include "App.h"
#include "Dialogs.h"
#include "Format.h"
#include "Messages.h"
#include "Theme.h"
#include "sync/OAuth.h"

#include <Alert.h>
#include <Beep.h>
#include <Button.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace aircal;
using namespace DialogKit;

namespace {

enum {
	kMsgAccountSelected = 'asel',
	kMsgTick = 'atik',
	kMsgEnabled = 'aena',
	kMsgCalendarToggled = 'acal',
	kMsgSyncAccount = 'asyn',
	kMsgSignInAgain = 'arsi',
	kMsgRemoveAccount = 'arem',
	kMsgAddGoogle = 'adgo',
	kMsgAddCalDav = 'adcd',
	kMsgAddTodoist = 'adtd',

	kMsgWorkerDone = 'wdon',
	kMsgSignIn = 'sgin',
	kMsgCancelSignIn = 'scnl',
	kMsgSaveClient = 'sclt',
	kMsgPreset = 'cpre',
	kMsgConnectToken = 'ctok'
};

const char* kAccountsKey = "accounts";


std::string
Lower(const std::string& text)
{
	std::string result;
	for (char c : text)
		result += (char)tolower((unsigned char)c);
	return result;
}


const char*
TypeLabel(AccountType type)
{
	switch (type) {
		case AccountType::Local:
			return "On My Computer";
		case AccountType::Google:
			return "Google";
		case AccountType::CalDAV:
			return "CalDAV";
		case AccountType::Todoist:
			return "Todoist";
	}
	return "";
}


std::string
SyncText(const Account& account)
{
	if (account.type == AccountType::Local)
		return "Stored on this computer";
	if (!account.enabled)
		return "Turned off";
	if (!account.lastError.empty())
		return account.lastError;
	if (account.lastSync == 0)
		return "Not synced yet";
	Instant age = Now() - account.lastSync;
	if (age < 60)
		return "Synced just now";
	if (age < 3600)
		return "Synced " + std::to_string(age / 60) + " min ago";
	if (age < 86400)
		return "Synced " + std::to_string(age / 3600) + " hr ago";
	return "Synced " + Format::DateShort(LocalDate(account.lastSync));
}


bool
NeedsSignIn(const Account& account)
{
	if (account.type == AccountType::Local || account.lastError.empty())
		return false;
	std::string error = Lower(account.lastError);
	for (const char* word : {"sign in", "sign-in", "signin", "auth", "token",
			"password", "401", "credential"}) {
		if (error.find(word) != std::string::npos)
			return true;
	}
	return false;
}


std::string
HostOf(const std::string& url)
{
	std::string host = url;
	size_t scheme = host.find("://");
	if (scheme != std::string::npos)
		host = host.substr(scheme + 3);
	host = host.substr(0, host.find('/'));
	size_t at = host.rfind('@');
	if (at != std::string::npos)
		host = host.substr(at + 1);
	if (!host.empty() && host[0] != '[')
		host = host.substr(0, host.find(':'));
	return host;
}


rgb_color
ErrorColor()
{
	rgb_color red = {230, 45, 40, 255};
	return Theme::Mix(ui_color(B_PANEL_TEXT_COLOR), red, 0.8f);
}


// Wrapped, read-only text in the panel's colours (explanations, errors).
class InfoText : public BTextView {
public:
	InfoText(const char* name, const std::string& text, bool secondary = false)
		:
		BTextView(name),
		fSecondary(secondary)
	{
		MakeEditable(false);
		MakeSelectable(true);
		SetWordWrap(true);
		SetStylable(false);
		SetInsets(0, 0, 0, 0);
		SetText(text.c_str());
		SetExplicitMinSize(BSize(340, B_SIZE_UNSET));
		UpdateColors();
	}

	void AttachedToWindow() override
	{
		BTextView::AttachedToWindow();
		UpdateColors();
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what == B_COLORS_UPDATED) {
			BTextView::MessageReceived(message);
			UpdateColors();
			return;
		}
		BTextView::MessageReceived(message);
	}

	void SetMessage(const std::string& text, bool error)
	{
		fError = error;
		SetText(text.c_str());
		UpdateColors();
		InvalidateLayout();
	}

private:
	void UpdateColors()
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
		rgb_color color = ui_color(B_PANEL_TEXT_COLOR);
		BFont font(be_plain_font);
		if (fError)
			color = ErrorColor();
		else if (fSecondary) {
			color = Theme::Mix(color, ui_color(B_PANEL_BACKGROUND_COLOR), 0.35f);
			font.SetSize(ceilf(font.Size() * 0.92f));
		}
		SetFontAndColor(0, TextLength(), &font, B_FONT_ALL, &color);
		SetFontAndColor(&font, B_FONT_ALL, &color);
		Invalidate();
	}

	bool fSecondary;
	bool fError = false;
};


void
SetVisible(BView* view, bool visible)
{
	if (view == NULL || visible == !view->IsHidden(view))
		return;
	if (visible)
		view->Show();
	else
		view->Hide();
}


// #pragma mark - Committing a sign-in


// Stores the credentials and adds the account (or updates the one being
// signed in again / the same login added before). Runs on a worker thread.
std::string
CommitAccount(AccountType type, const SignInResult& result,
	const std::string& server, const std::string& reauthId)
{
	App* app = TheApp();
	Store& store = app->Store();
	std::string email = result.email;
	std::string display = !email.empty() ? email
		: !result.name.empty() ? result.name : TypeLabel(type);

	std::string id;
	bool existing = false;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Account* account = reauthId.empty() ? NULL : store.FindAccount(reauthId);
		if (account == NULL && !email.empty()) {
			for (auto& candidate : store.Accounts()) {
				if (candidate->type == type
					&& Lower(candidate->user) == Lower(email)) {
					account = candidate.get();
					break;
				}
			}
		}
		if (account != NULL) {
			id = account->id;
			existing = true;
		} else
			id = NewId();
	}

	if (!result.credentials.Save(app->Secrets(), id))
		throw SyncError("airCal could not store the sign-in credentials.");

	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Account* account = existing ? store.FindAccount(id) : NULL;
		if (account != NULL) {
			account->enabled = true;
			account->lastError.clear();
			if (account->name.empty())
				account->name = display;
			if (account->user.empty())
				account->user = email;
		} else {
			Account fresh;
			fresh.id = id;
			fresh.type = type;
			fresh.name = display;
			fresh.user = email;
			fresh.server = server;
			store.AddAccount(fresh);
		}
	}
	store.Changed();
	app->Sync().Request(id, true);
	return id;
}


// #pragma mark - SignInWindow


// The base of the add-account windows: runs one job at a time on a worker
// thread and hands its result to JobFinished() on the window thread.
class SignInWindow : public BWindow {
public:
	typedef std::function<std::string(const std::atomic<bool>& cancelled,
		std::string& accountId)> Job;

	SignInWindow(const char* title, const std::string& key,
		const std::string& reauthId);
	~SignInWindow() override;

	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;

	void PlaceNear(BWindow* parent);

protected:
	// Resizes the (non-resizable) window to its content's height at the
	// current width, after a status line or note is shown, hidden or
	// rewrapped. The size limits alone do not grow it: they come from the
	// views' minimum sizes, which leave out wrapped text.
	void FitToContent();
	void SetNoteVisible(BView* view, bool visible);
	void Start(Job job, const std::string& status);
	void CancelJob();
	bool Busy() const { return fBusy; }
	void ShowStatus(const std::string& text, bool error);
	// Enable/disable the inputs while a job runs.
	virtual void SetBusy(bool busy) = 0;
	virtual void JobFinished(const std::string& error,
		const std::string& accountId);
	void Succeeded(const std::string& accountId);

	std::string fKey;
	std::string fReauthId;
	InfoText* fStatus = NULL;
	BButton* fCancel = NULL;

private:
	void Join();

	std::thread fThread;
	std::shared_ptr<std::atomic<bool>> fCancelFlag;
	bool fBusy = false;
};


SignInWindow::SignInWindow(const char* title, const std::string& key,
	const std::string& reauthId)
	:
	BWindow(BRect(0, 0, 400, 240), title, B_TITLED_WINDOW_LOOK,
		B_NORMAL_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE
			| B_ASYNCHRONOUS_CONTROLS),
	fKey(key),
	fReauthId(reauthId)
{
	fStatus = new InfoText("status", "");
	fCancel = new BButton("cancel", "Cancel", new BMessage(kMsgCancelSignIn));
}


SignInWindow::~SignInWindow()
{
	Join();
	Unregister(fKey, this);
}


void
SignInWindow::PlaceNear(BWindow* parent)
{
	ResizeToPreferred();
	if (parent != NULL)
		CenterIn(parent->Frame());
	else
		PlaceWindow(this, -1, -1);
	MoveOnScreen(B_DO_NOT_RESIZE_TO_FIT);
}


void
SignInWindow::FitToContent()
{
	BLayout* layout = GetLayout();
	if (layout == NULL)
		return;
	float width = Bounds().Width();
	float height = layout->PreferredSize().height;
	if (layout->HasHeightForWidth())
		layout->GetHeightForWidth(width, NULL, NULL, &height);
	if (height != Bounds().Height())
		ResizeTo(width, height);
}


void
SignInWindow::SetNoteVisible(BView* view, bool visible)
{
	SetVisible(view, visible);
	FitToContent();
}


void
SignInWindow::Join()
{
	if (fCancelFlag)
		*fCancelFlag = true;
	if (fThread.joinable())
		fThread.join();
}


void
SignInWindow::Start(Job job, const std::string& status)
{
	if (fBusy)
		return;
	Join();
	fCancelFlag = std::make_shared<std::atomic<bool>>(false);
	fBusy = true;
	SetBusy(true);
	ShowStatus(status, false);

	std::shared_ptr<std::atomic<bool>> cancelled = fCancelFlag;
	BMessenger messenger(this);
	fThread = std::thread([job, cancelled, messenger]() {
		std::string error;
		std::string accountId;
		try {
			error = job(*cancelled, accountId);
		} catch (const std::exception& exception) {
			error = exception.what();
		} catch (...) {
			error = "Something went wrong.";
		}
		if (error.empty() && *cancelled)
			error = "Cancelled";
		BMessage done(kMsgWorkerDone);
		done.AddString("error", error.c_str());
		done.AddString("account", accountId.c_str());
		// The window may be gone (or going): never wait for it.
		BMessenger(messenger).SendMessage(&done, (BHandler*)NULL, 500000);
	});
}


void
SignInWindow::CancelJob()
{
	if (fCancelFlag)
		*fCancelFlag = true;
	ShowStatus("Cancelling" B_UTF8_ELLIPSIS, false);
}


void
SignInWindow::ShowStatus(const std::string& text, bool error)
{
	fStatus->SetMessage(text, error);
	SetNoteVisible(fStatus, !text.empty());
}


void
SignInWindow::JobFinished(const std::string& error, const std::string& accountId)
{
	if (error.empty()) {
		Succeeded(accountId);
		return;
	}
	ShowStatus(error == "Cancelled" ? "" : error, true);
}


void
SignInWindow::Succeeded(const std::string& accountId)
{
	TheApp()->ScheduleSave();
	BMessage added(kMsgAccountAdded);
	added.AddString("account", accountId.c_str());
	PostToMain(&added);
	PostToRegistered(kAccountsKey, &added);
	Quit();
}


void
SignInWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgWorkerDone:
		{
			if (fThread.joinable())
				fThread.join();
			fBusy = false;
			SetBusy(false);
			JobFinished(message->GetString("error", ""),
				message->GetString("account", ""));
			break;
		}
		case kMsgCancelSignIn:
			if (fBusy)
				CancelJob();
			else
				Quit();
			break;
		default:
			BWindow::MessageReceived(message);
	}
}


bool
SignInWindow::QuitRequested()
{
	// Join() in the destructor waits for the cancelled job.
	if (fCancelFlag)
		*fCancelFlag = true;
	return true;
}


// #pragma mark - GoogleSignInWindow


class GoogleSignInWindow : public SignInWindow {
public:
	GoogleSignInWindow(const std::string& key, const std::string& reauthId);

	void MessageReceived(BMessage* message) override;

protected:
	void SetBusy(bool busy) override;

private:
	void UpdateClient();
	void SaveClient();
	void SignIn();

	std::string fClientId;
	std::string fClientSecret;
	bool fHasClient = false;

	BView* fSetup = NULL;
	BTextControl* fClientIdField = NULL;
	BTextControl* fClientSecretField = NULL;
	BButton* fSaveClient = NULL;
	InfoText* fClientNote = NULL;
	BButton* fSignIn = NULL;
};


GoogleSignInWindow::GoogleSignInWindow(const std::string& key,
	const std::string& reauthId)
	:
	SignInWindow(reauthId.empty() ? "Add Google Account" : "Sign In to Google",
		key, reauthId)
{
	InfoText* intro = new InfoText("intro", "airCal opens Google's sign-in "
		"page in your web browser. Allow airCal to see and edit your calendars "
		"and tasks there; airCal continues on its own afterwards.");

	InfoText* help = new InfoText("help", "Create a Desktop app OAuth client "
		"in Google Cloud Console with the Google Calendar API and Google Tasks "
		"API enabled (see the README).", true);
	fClientIdField = new BTextControl("clientId", "Client ID:", "", NULL);
	fClientSecretField = new BTextControl("clientSecret", "Client secret:", "",
		NULL);
	fClientSecretField->TextView()->HideTyping(true);
	fSaveClient = new BButton("saveClient", "Save Client",
		new BMessage(kMsgSaveClient));

	fSetup = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Group<>(static_cast<BGroupView*>(fSetup))
		.Add(help)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fClientIdField, 0, 0)
			.AddTextControl(fClientSecretField, 0, 1)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fSaveClient)
		.End();

	fClientNote = new InfoText("clientNote", "", true);
	fSignIn = new BButton("signIn", "Sign in with Google",
		new BMessage(kMsgSignIn));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(intro)
		.Add(fSetup)
		.Add(fClientNote)
		.Add(fStatus)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCancel)
			.Add(fSignIn)
		.End();

	ShowStatus("", false);
	UpdateClient();
}


void
GoogleSignInWindow::UpdateClient()
{
	std::string source;
	fHasClient = FindGoogleClient(TheApp()->SettingsRoot(), fClientId,
		fClientSecret, &source);
	if (fHasClient)
		fClientNote->SetMessage("Using the OAuth client from " + source + ".",
			false);
	SetVisible(fSetup, !fHasClient);
	SetNoteVisible(fClientNote, fHasClient);
	fSignIn->SetEnabled(fHasClient);
	SetDefaultButton(fHasClient ? fSignIn : fSaveClient);
	if (!fHasClient)
		fClientIdField->MakeFocus(true);
}


void
GoogleSignInWindow::SaveClient()
{
	std::string id = Trim(fClientIdField->Text());
	std::string secret = Trim(fClientSecretField->Text());
	if (id.empty()) {
		ShowStatus("Enter the client ID of your Desktop app OAuth client.",
			true);
		fClientIdField->MakeFocus(true);
		return;
	}

	std::string directory = TheApp()->SettingsRoot() + "/airCal";
	mkdir(directory.c_str(), 0700);
	std::string path = directory + "/google-client.json";
	nlohmann::json root = {{"installed", {{"client_id", id},
		{"client_secret", secret}}}};
	std::string text = root.dump(1, '\t') + "\n";

	std::string temporary = path + ".new";
	int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	bool ok = fd >= 0;
	if (ok) {
		fchmod(fd, 0600);
		ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
		close(fd);
		ok = ok && rename(temporary.c_str(), path.c_str()) == 0;
		if (!ok)
			unlink(temporary.c_str());
	}
	if (!ok) {
		ShowStatus("Could not write " + path + ": " + strerror(errno), true);
		return;
	}
	ShowStatus("", false);
	UpdateClient();
}


void
GoogleSignInWindow::SignIn()
{
	if (!fHasClient)
		return;
	std::string id = fClientId;
	std::string secret = fClientSecret;
	std::string base = TheApp()->CloudBase();
	std::string reauthId = fReauthId;
	Start([id, secret, base, reauthId](const std::atomic<bool>& cancelled,
			std::string& accountId) {
		App* app = TheApp();
		SignInResult result = GoogleSignIn(app->Http(), Endpoints::For(base),
			id, secret, [](const std::string& url) { OpenUrl(url); },
			cancelled);
		if (cancelled)
			return std::string("Cancelled");
		accountId = CommitAccount(AccountType::Google, result, base, reauthId);
		return std::string();
	}, "Waiting for you to sign in in your web browser" B_UTF8_ELLIPSIS);
}


void
GoogleSignInWindow::SetBusy(bool busy)
{
	fSignIn->SetEnabled(!busy && fHasClient);
	fSaveClient->SetEnabled(!busy);
	fClientIdField->SetEnabled(!busy);
	fClientSecretField->SetEnabled(!busy);
}


void
GoogleSignInWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSaveClient:
			SaveClient();
			break;
		case kMsgSignIn:
			SignIn();
			break;
		default:
			SignInWindow::MessageReceived(message);
	}
}


// #pragma mark - CalDavSignInWindow


struct ServerPreset {
	const char* label;
	const char* server;
	const char* note;
};

const ServerPreset kServerPresets[] = {
	{"Custom", "", ""},
	{"iCloud", "https://caldav.icloud.com",
		"iCloud needs an app-specific password: create one at appleid.apple.com "
		"under Sign-In and Security " B_UTF8_ELLIPSIS " App-Specific Passwords."},
	{"Fastmail", "https://caldav.fastmail.com",
		"Fastmail needs an app password with CalDAV access (Settings "
		"\xE2\x86\x92 Privacy & Security \xE2\x86\x92 App passwords)."},
	{"Nextcloud", "https://<host>/remote.php/dav",
		"Replace <host> with the address of your Nextcloud server. With "
		"two-factor sign-in, use an app password."},
	{"Yahoo", "https://caldav.calendar.yahoo.com",
		"Yahoo needs an app password (Account Security \xE2\x86\x92 Generate "
		"app password)."}
};


class CalDavSignInWindow : public SignInWindow {
public:
	CalDavSignInWindow(const std::string& key, const std::string& reauthId);

	void MessageReceived(BMessage* message) override;

protected:
	void SetBusy(bool busy) override;

private:
	void PresetChosen(int index);
	void SignIn();

	BMenuField* fPresetField = NULL;
	BTextControl* fServer = NULL;
	BTextControl* fUser = NULL;
	BTextControl* fPassword = NULL;
	InfoText* fPresetNote = NULL;
	BButton* fSignIn = NULL;
};


CalDavSignInWindow::CalDavSignInWindow(const std::string& key,
	const std::string& reauthId)
	:
	SignInWindow(reauthId.empty() ? "Add CalDAV Account"
		: "Sign In to CalDAV", key, reauthId)
{
	std::string server;
	std::string user;
	if (!reauthId.empty()) {
		Store& store = TheApp()->Store();
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Account* account = store.FindAccount(reauthId);
		if (account != NULL) {
			server = account->server;
			user = account->user;
		}
	}

	BPopUpMenu* presets = new BPopUpMenu("preset");
	for (size_t i = 0; i < B_COUNT_OF(kServerPresets); i++) {
		BMessage* message = new BMessage(kMsgPreset);
		message->AddInt32("index", (int32)i);
		BMenuItem* item = new BMenuItem(kServerPresets[i].label, message);
		presets->AddItem(item);
		if (i == 0)
			item->SetMarked(true);
	}
	fPresetField = new BMenuField("preset", "Service:", presets);
	fServer = new BTextControl("server", "Server:", server.c_str(), NULL);
	fServer->SetExplicitMinSize(BSize(280, B_SIZE_UNSET));
	InfoText* serverHint = new InfoText("serverHint",
		"https://caldav.example.com or iCloud: https://caldav.icloud.com", true);
	fUser = new BTextControl("user", "User name:", user.c_str(), NULL);
	fPassword = new BTextControl("password", "Password:", "", NULL);
	fPassword->TextView()->HideTyping(true);
	fPresetNote = new InfoText("presetNote", "", true);
	fSignIn = new BButton("signIn", "Sign In", new BMessage(kMsgSignIn));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddMenuField(fPresetField, 0, 0)
			.AddTextControl(fServer, 0, 1)
			.Add(serverHint, 1, 2)
			.AddTextControl(fUser, 0, 3)
			.AddTextControl(fPassword, 0, 4)
		.End()
		.Add(fPresetNote)
		.Add(fStatus)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCancel)
			.Add(fSignIn)
		.End();

	if (!reauthId.empty()) {
		fPresetField->SetEnabled(false);
		fServer->SetEnabled(false);
		fUser->SetEnabled(false);
		fPassword->MakeFocus(true);
	} else
		fServer->MakeFocus(true);
	ShowStatus("", false);
	SetNoteVisible(fPresetNote, false);
	SetDefaultButton(fSignIn);
}


void
CalDavSignInWindow::PresetChosen(int index)
{
	if (index < 0 || index >= (int)B_COUNT_OF(kServerPresets))
		return;
	const ServerPreset& preset = kServerPresets[index];
	if (preset.server[0] != '\0')
		fServer->SetText(preset.server);
	fPresetNote->SetMessage(preset.note, false);
	SetNoteVisible(fPresetNote, preset.note[0] != '\0');
	std::string text = fServer->Text();
	size_t host = text.find("<host>");
	if (host != std::string::npos) {
		fServer->MakeFocus(true);
		fServer->TextView()->Select(host, host + 6);
	} else
		fUser->MakeFocus(true);
}


void
CalDavSignInWindow::SignIn()
{
	std::string server = Trim(fServer->Text());
	std::string user = Trim(fUser->Text());
	std::string password = fPassword->Text();
	if (server.empty() || server.find('<') != std::string::npos) {
		ShowStatus("Enter the address of your CalDAV server.", true);
		fServer->MakeFocus(true);
		return;
	}
	if (server.find("://") == std::string::npos)
		server = "https://" + server;
	std::string host = HostOf(server);
	if (host.empty()) {
		ShowStatus("That server address is not valid.", true);
		fServer->MakeFocus(true);
		return;
	}

	std::string reauthId = fReauthId;
	Start([server, user, password, host, reauthId](
			const std::atomic<bool>& cancelled, std::string& accountId) {
		App* app = TheApp();
		Store& store = app->Store();
		Secrets& secrets = app->Secrets();

		std::string id = reauthId;
		Credentials previous;
		bool hadPrevious = false;
		if (!id.empty())
			hadPrevious = Credentials::Load(secrets, id, previous);
		else {
			id = NewId();
			Account account;
			account.id = id;
			account.type = AccountType::CalDAV;
			account.name = user.empty() ? host : user + "@" + host;
			account.user = user;
			account.server = server;
			// Off until the test sync succeeded, so the sync engine leaves
			// it alone meanwhile.
			account.enabled = false;
			store.AddAccount(account);
		}
		Credentials credentials;
		credentials.password = password;

		// Undoes everything when the test fails.
		auto rollBack = [&]() {
			if (reauthId.empty()) {
				secrets.Remove("account:" + id);
				store.RemoveAccount(id);
			} else if (hadPrevious)
				previous.Save(secrets, id);
			store.Changed();
		};

		std::string error;
		try {
			if (!credentials.Save(secrets, id))
				throw SyncError("airCal could not store the password.");
			std::unique_ptr<Provider> provider
				= CreateProvider(AccountType::CalDAV);
			SyncContext context(store, app->Http(), secrets, cancelled);
			context.full = true;
			provider->Sync(id, context);
			if (cancelled)
				error = "Cancelled";
		} catch (const std::exception& exception) {
			error = exception.what();
		}
		if (!error.empty()) {
			rollBack();
			return error;
		}

		{
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			Account* account = store.FindAccount(id);
			if (account == NULL)
				return std::string("The account was removed meanwhile.");
			account->enabled = true;
			account->lastSync = Now();
			account->lastError.clear();
		}
		store.Changed();
		accountId = id;
		return std::string();
	}, "Connecting to " + host + B_UTF8_ELLIPSIS);
}


void
CalDavSignInWindow::SetBusy(bool busy)
{
	bool editable = !busy && fReauthId.empty();
	fPresetField->SetEnabled(editable);
	fServer->SetEnabled(editable);
	fUser->SetEnabled(editable);
	fPassword->SetEnabled(!busy);
	fSignIn->SetEnabled(!busy);
}


void
CalDavSignInWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPreset:
			PresetChosen(message->GetInt32("index", 0));
			break;
		case kMsgSignIn:
			SignIn();
			break;
		default:
			SignInWindow::MessageReceived(message);
	}
}


// #pragma mark - TodoistSignInWindow


class TodoistSignInWindow : public SignInWindow {
public:
	TodoistSignInWindow(const std::string& key, const std::string& reauthId);

	void MessageReceived(BMessage* message) override;

protected:
	void SetBusy(bool busy) override;

private:
	void SignIn();
	void Connect();

	BButton* fSignIn = NULL;
	BTextControl* fToken = NULL;
	BButton* fConnect = NULL;
};


TodoistSignInWindow::TodoistSignInWindow(const std::string& key,
	const std::string& reauthId)
	:
	SignInWindow(reauthId.empty() ? "Add Todoist Account"
		: "Sign In to Todoist", key, reauthId)
{
	InfoText* intro = new InfoText("intro", "Sign in to Todoist in your web "
		"browser, or connect with your personal API token.");
	fSignIn = new BButton("signIn", "Sign in with Todoist",
		new BMessage(kMsgSignIn));

	fToken = new BTextControl("token", "API token:", "",
		new BMessage(kMsgConnectToken));
	fToken->TextView()->HideTyping(true);
	fConnect = new BButton("connect", "Connect", new BMessage(kMsgConnectToken));
	InfoText* help = new InfoText("help", "Find it in Todoist: Settings "
		"\xE2\x86\x92 Integrations \xE2\x86\x92 Developer \xE2\x86\x92 API "
		"token", true);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(intro)
		.AddGroup(B_HORIZONTAL)
			.Add(fSignIn)
			.AddGlue()
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(fToken)
			.Add(fConnect)
		.End()
		.Add(help)
		.Add(fStatus)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCancel)
		.End();

	ShowStatus("", false);
	SetDefaultButton(fSignIn);
}


void
TodoistSignInWindow::SignIn()
{
	std::string base = TheApp()->CloudBase();
	std::string reauthId = fReauthId;
	Start([base, reauthId](const std::atomic<bool>& cancelled,
			std::string& accountId) {
		App* app = TheApp();
		SignInResult result = TodoistSignIn(app->Http(), Endpoints::For(base),
			[](const std::string& url) { OpenUrl(url); }, cancelled);
		if (cancelled)
			return std::string("Cancelled");
		accountId = CommitAccount(AccountType::Todoist, result, base, reauthId);
		return std::string();
	}, "Waiting for you to sign in in your web browser" B_UTF8_ELLIPSIS);
}


void
TodoistSignInWindow::Connect()
{
	std::string token = Trim(fToken->Text());
	if (token.empty()) {
		ShowStatus("Paste your Todoist API token first.", true);
		fToken->MakeFocus(true);
		return;
	}
	std::string base = TheApp()->CloudBase();
	std::string reauthId = fReauthId;
	Start([base, reauthId, token](const std::atomic<bool>& cancelled,
			std::string& accountId) {
		App* app = TheApp();
		SignInResult result = TodoistTokenSignIn(app->Http(),
			Endpoints::For(base), token, cancelled);
		if (cancelled)
			return std::string("Cancelled");
		accountId = CommitAccount(AccountType::Todoist, result, base, reauthId);
		return std::string();
	}, "Checking the token" B_UTF8_ELLIPSIS);
}


void
TodoistSignInWindow::SetBusy(bool busy)
{
	fSignIn->SetEnabled(!busy);
	fToken->SetEnabled(!busy);
	fConnect->SetEnabled(!busy);
}


void
TodoistSignInWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSignIn:
			SignIn();
			break;
		case kMsgConnectToken:
			Connect();
			break;
		default:
			SignInWindow::MessageReceived(message);
	}
}


void
OpenSignInWindow(AccountType type, const std::string& reauthId,
	BWindow* parent)
{
	std::string key = (reauthId.empty() ? std::string("add:")
		: "reauth:" + reauthId + ":") + AccountTypeName(type);
	if (ActivateRegistered(key))
		return;
	SignInWindow* window;
	switch (type) {
		case AccountType::Google:
			window = new GoogleSignInWindow(key, reauthId);
			break;
		case AccountType::CalDAV:
			window = new CalDavSignInWindow(key, reauthId);
			break;
		case AccountType::Todoist:
			window = new TodoistSignInWindow(key, reauthId);
			break;
		default:
			return;
	}
	Register(key, window);
	window->PlaceNear(parent);
	window->Show();
}


// #pragma mark - List items


class AccountItem : public BListItem {
public:
	explicit AccountItem(const Account& account)
	{
		Set(account);
	}

	// Returns whether anything shown changed.
	bool Set(const Account& account)
	{
		std::string detail = std::string(TypeLabel(account.type));
		if (account.type != AccountType::Local)
			detail += " \xC2\xB7 " + SyncText(account);
		bool error = account.type != AccountType::Local && account.enabled
			&& !account.lastError.empty();
		bool changed = fId != account.id || fName != account.name
			|| fDetail != detail || fError != error
			|| fEnabled != account.enabled;
		fId = account.id;
		fName = account.name;
		fDetail = detail;
		fError = error;
		fEnabled = account.enabled;
		return changed;
	}

	const std::string& Id() const { return fId; }

	void Update(BView* owner, const BFont* font) override
	{
		BListItem::Update(owner, font);
		font_height height;
		font->GetHeight(&height);
		float line = ceilf(height.ascent + height.descent + height.leading);
		SetHeight(line * 2 + 12);
	}

	void DrawItem(BView* owner, BRect frame, bool complete) override
	{
		rgb_color background = ui_color(IsSelected()
			? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
		rgb_color text = ui_color(IsSelected()
			? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
		if (!fEnabled)
			text = Theme::Mix(text, background, 0.4f);
		rgb_color secondary = Theme::Mix(text, background, 0.35f);
		if (fError && !IsSelected())
			secondary = ErrorColor();

		owner->SetLowColor(background);
		owner->FillRect(frame, B_SOLID_LOW);

		BFont saved;
		owner->GetFont(&saved);
		font_height height;
		saved.GetHeight(&height);
		float line = ceilf(height.ascent + height.descent + height.leading);
		float x = frame.left + 8;
		float width = frame.Width() - 16;

		BFont bold(saved);
		bold.SetFace(B_BOLD_FACE);
		owner->SetFont(&bold);
		owner->SetHighColor(text);
		BString name(fName.c_str());
		owner->TruncateString(&name, B_TRUNCATE_END, width);
		owner->DrawString(name.String(),
			BPoint(x, frame.top + 6 + ceilf(height.ascent)));

		owner->SetFont(&saved);
		owner->SetHighColor(secondary);
		BString detail(fDetail.c_str());
		owner->TruncateString(&detail, B_TRUNCATE_END, width);
		owner->DrawString(detail.String(),
			BPoint(x, frame.top + 6 + line + ceilf(height.ascent)));
	}

private:
	std::string fId;
	std::string fName;
	std::string fDetail;
	bool fError = false;
	bool fEnabled = true;
};


class CalendarItem : public BListItem {
public:
	explicit CalendarItem(const Calendar& calendar)
		:
		fId(calendar.id),
		fName(calendar.name),
		fColor(calendar.color),
		fVisible(calendar.visible)
	{
		if (calendar.events && calendar.tasks)
			fKind = "events and tasks";
		else if (calendar.tasks)
			fKind = "tasks";
		if (calendar.readOnly)
			fKind += fKind.empty() ? "read-only" : ", read-only";
	}

	const std::string& Id() const { return fId; }
	void SetVisible(bool visible) { fVisible = visible; }

	void Update(BView* owner, const BFont* font) override
	{
		BListItem::Update(owner, font);
		font_height height;
		font->GetHeight(&height);
		SetHeight(ceilf(height.ascent + height.descent + height.leading) + 10);
	}

	void DrawItem(BView* owner, BRect frame, bool complete) override
	{
		rgb_color background = ui_color(B_LIST_BACKGROUND_COLOR);
		rgb_color text = ui_color(B_LIST_ITEM_TEXT_COLOR);
		owner->SetLowColor(background);
		owner->FillRect(frame, B_SOLID_LOW);

		BFont font;
		owner->GetFont(&font);
		font_height height;
		font.GetHeight(&height);
		float baseline = floorf((frame.top + frame.bottom
			+ height.ascent - height.descent) / 2);

		float box = ceilf(height.ascent) + 2;
		BRect check(frame.left + 8, 0, frame.left + 8 + box, box);
		check.OffsetTo(check.left, floorf((frame.top + frame.bottom - box) / 2));
		be_control_look->DrawCheckBox(owner, check, frame,
			ui_color(B_CONTROL_BACKGROUND_COLOR),
			fVisible ? BControlLook::B_ACTIVATED : 0);

		float radius = floorf((height.ascent + height.descent) * 0.3f);
		float x = frame.left + 8 + box + 8;
		DrawColorDot(owner, BPoint(x + radius, (frame.top + frame.bottom) / 2),
			radius, fColor);
		x += radius * 2 + 6;

		owner->SetHighColor(text);
		owner->DrawString(fName.c_str(), BPoint(x, baseline));
		if (!fKind.empty()) {
			owner->SetHighColor(Theme::Mix(text, background, 0.45f));
			owner->DrawString(("   " + fKind).c_str());
		}
	}

private:
	std::string fId;
	std::string fName;
	std::string fKind;
	uint32 fColor;
	bool fVisible;
};


// Clicking a row toggles the calendar's visibility.
class CalendarListView : public BListView {
public:
	CalendarListView()
		:
		BListView("calendars", B_SINGLE_SELECTION_LIST)
	{
	}

	void MouseDown(BPoint where) override
	{
		int32 index = IndexOf(where);
		CalendarItem* item = dynamic_cast<CalendarItem*>(ItemAt(index));
		if (item == NULL)
			return;
		BMessage toggle(kMsgCalendarToggled);
		toggle.AddString("calendar", item->Id().c_str());
		Window()->PostMessage(&toggle);
	}
};


// #pragma mark - AccountsWindow


class AccountsWindow : public BWindow {
public:
	AccountsWindow();
	~AccountsWindow() override;

	void MessageReceived(BMessage* message) override;

	void Select(const std::string& accountId);

private:
	std::string SelectedId() const;
	void RefreshList();
	void RefreshDetails(bool force);
	std::string DetailsSignature(const std::string& accountId) const;
	BView* BuildDetails(const std::string& accountId);
	void ToggleCalendar(const std::string& calendarId);
	void SetEnabled(bool enabled);
	void RemoveAccount();

	BListView* fList = NULL;
	BGroupLayout* fRight = NULL;
	BView* fDetails = NULL;
	BStringView* fStatus = NULL;
	BButton* fSignInAgain = NULL;
	CalendarListView* fCalendars = NULL;
	BMessageRunner* fTicker = NULL;
	uint64 fRevision = 0;
	std::string fDetailsId;
	std::string fDetailsSignature;
};


AccountsWindow::AccountsWindow()
	:
	BWindow(BRect(0, 0, 680, 420), "Accounts", B_TITLED_WINDOW_LOOK,
		B_NORMAL_WINDOW_FEEL, B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE | B_ASYNCHRONOUS_CONTROLS)
{
	fList = new BListView("accounts", B_SINGLE_SELECTION_LIST);
	fList->SetSelectionMessage(new BMessage(kMsgAccountSelected));
	BScrollView* listScroll = new BScrollView("accountsScroll", fList, 0,
		false, true);
	listScroll->SetExplicitMinSize(BSize(230, 260));
	listScroll->SetExplicitPreferredSize(BSize(250, 320));

	BMenu* addMenu = new BMenu("Add Account");
	addMenu->SetRadioMode(false);
	addMenu->SetLabelFromMarked(false);
	addMenu->AddItem(new BMenuItem("Google" B_UTF8_ELLIPSIS,
		new BMessage(kMsgAddGoogle)));
	addMenu->AddItem(new BMenuItem("CalDAV" B_UTF8_ELLIPSIS,
		new BMessage(kMsgAddCalDav)));
	addMenu->AddItem(new BMenuItem("Todoist" B_UTF8_ELLIPSIS,
		new BMessage(kMsgAddTodoist)));
	BMenuField* addField = new BMenuField("add", NULL, addMenu);

	BGroupView* right = new BGroupView(B_VERTICAL, 0);
	fRight = right->GroupLayout();
	right->SetExplicitMinSize(BSize(360, B_SIZE_UNSET));

	BLayoutBuilder::Group<>(this, B_HORIZONTAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 0)
			.Add(listScroll)
			.Add(addField)
		.End()
		.Add(right, 1);

	RefreshList();
	if (fList->CountItems() > 0)
		fList->Select(0);
	RefreshDetails(true);

	BMessage tick(kMsgTick);
	fTicker = new BMessageRunner(BMessenger(this), &tick, 1000000);
}


AccountsWindow::~AccountsWindow()
{
	delete fTicker;
	Unregister(kAccountsKey, this);
}


std::string
AccountsWindow::SelectedId() const
{
	AccountItem* item = dynamic_cast<AccountItem*>(
		fList->ItemAt(fList->CurrentSelection()));
	return item != NULL ? item->Id() : std::string();
}


void
AccountsWindow::Select(const std::string& accountId)
{
	RefreshList();
	for (int32 i = 0; i < fList->CountItems(); i++) {
		AccountItem* item = dynamic_cast<AccountItem*>(fList->ItemAt(i));
		if (item != NULL && item->Id() == accountId) {
			fList->Select(i);
			fList->ScrollToSelection();
			break;
		}
	}
	RefreshDetails(true);
}


void
AccountsWindow::RefreshList()
{
	Store& store = TheApp()->Store();
	std::vector<Account> accounts;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		fRevision = store.Revision();
		for (auto& account : store.Accounts())
			accounts.push_back(*account);
	}

	bool same = (int32)accounts.size() == fList->CountItems();
	for (size_t i = 0; same && i < accounts.size(); i++) {
		AccountItem* item = dynamic_cast<AccountItem*>(fList->ItemAt(i));
		same = item != NULL && item->Id() == accounts[i].id;
	}
	if (same) {
		for (size_t i = 0; i < accounts.size(); i++) {
			AccountItem* item = static_cast<AccountItem*>(fList->ItemAt(i));
			if (item->Set(accounts[i]))
				fList->InvalidateItem(i);
		}
		return;
	}

	std::string selected = SelectedId();
	BMessage* selection = fList->SelectionMessage() != NULL
		? new BMessage(*fList->SelectionMessage()) : NULL;
	fList->SetSelectionMessage(NULL);
	while (BListItem* item = fList->RemoveItem((int32)0))
		delete item;
	int32 select = -1;
	for (size_t i = 0; i < accounts.size(); i++) {
		fList->AddItem(new AccountItem(accounts[i]));
		if (accounts[i].id == selected)
			select = i;
	}
	if (select < 0 && !accounts.empty())
		select = 0;
	if (select >= 0)
		fList->Select(select);
	fList->SetSelectionMessage(selection);
}


std::string
AccountsWindow::DetailsSignature(const std::string& accountId) const
{
	Store& store = TheApp()->Store();
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Account* account = store.FindAccount(accountId);
	if (account == NULL)
		return "-";
	std::string signature = account->name + "\n" + account->user + "\n"
		+ account->server + "\n" + (account->enabled ? "1" : "0")
		+ (NeedsSignIn(*account) ? "1" : "0");
	for (Calendar* calendar : store.CalendarsOf(accountId)) {
		signature += "\n" + calendar->id + calendar->name
			+ std::to_string(calendar->color) + (calendar->visible ? "v" : "h")
			+ (calendar->readOnly ? "r" : "w") + (calendar->events ? "e" : "")
			+ (calendar->tasks ? "t" : "");
	}
	return signature;
}


void
AccountsWindow::RefreshDetails(bool force)
{
	std::string id = SelectedId();
	std::string signature = DetailsSignature(id);
	if (!force && id == fDetailsId && signature == fDetailsSignature) {
		// Only the sync status text moves on.
		if (fStatus != NULL) {
			Store& store = TheApp()->Store();
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			Account* account = store.FindAccount(id);
			if (account != NULL) {
				std::string text = SyncText(*account);
				if (text != fStatus->Text()) {
					fStatus->SetText(text.c_str());
					fStatus->SetToolTip(text.c_str());
					bool error = account->enabled && !account->lastError.empty()
						&& account->type != AccountType::Local;
					if (error)
						fStatus->SetHighColor(ErrorColor());
					else
						fStatus->SetHighUIColor(B_PANEL_TEXT_COLOR);
					fStatus->Invalidate();
				}
			}
		}
		return;
	}

	fDetailsId = id;
	fDetailsSignature = signature;
	fStatus = NULL;
	fSignInAgain = NULL;
	fCalendars = NULL;
	if (fDetails != NULL) {
		fRight->RemoveView(fDetails);
		delete fDetails;
	}
	fDetails = BuildDetails(id);
	fRight->AddView(fDetails);
}


BView*
AccountsWindow::BuildDetails(const std::string& accountId)
{
	BGroupView* view = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
	Store& store = TheApp()->Store();
	Account account;
	std::vector<Calendar> calendars;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Account* found = store.FindAccount(accountId);
		if (found == NULL) {
			BStringView* empty = new BStringView("empty",
				"Add an account to sync calendars and tasks.");
			empty->SetAlignment(B_ALIGN_CENTER);
			empty->SetExplicitAlignment(BAlignment(B_ALIGN_CENTER,
				B_ALIGN_MIDDLE));
			BLayoutBuilder::Group<>(view)
				.AddGlue()
				.Add(empty)
				.AddGlue();
			return view;
		}
		account = *found;
		for (Calendar* calendar : store.CalendarsOf(accountId))
			calendars.push_back(*calendar);
	}
	bool local = account.type == AccountType::Local;

	BStringView* name = new BStringView("name", account.name.c_str());
	BFont big(be_bold_font);
	big.SetSize(ceilf(big.Size() * 1.25f));
	name->SetFont(&big);
	name->SetTruncation(B_TRUNCATE_END);

	std::string detail = TypeLabel(account.type);
	if (!local) {
		if (!account.user.empty() && account.user != account.name)
			detail += " \xC2\xB7 " + account.user;
		if (account.type == AccountType::CalDAV && !account.server.empty())
			detail += " \xC2\xB7 " + account.server;
	}
	BStringView* type = new BStringView("type", detail.c_str());
	type->SetTruncation(B_TRUNCATE_MIDDLE);

	std::string statusText = SyncText(account);
	fStatus = new BStringView("status", statusText.c_str());
	fStatus->SetTruncation(B_TRUNCATE_END);
	fStatus->SetToolTip(statusText.c_str());
	fStatus->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
	if (!local && account.enabled && !account.lastError.empty())
		fStatus->SetHighColor(ErrorColor());

	BCheckBox* enabled = new BCheckBox("enabled", "Enabled",
		new BMessage(kMsgEnabled));
	enabled->SetValue(account.enabled ? B_CONTROL_ON : B_CONTROL_OFF);

	fCalendars = new CalendarListView();
	for (const Calendar& calendar : calendars)
		fCalendars->AddItem(new CalendarItem(calendar));
	BScrollView* listScroll = new BScrollView("calendarsScroll", fCalendars, 0,
		false, true);
	listScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 140));

	BButton* syncNow = new BButton("syncNow", "Sync Now",
		new BMessage(kMsgSyncAccount));
	syncNow->SetEnabled(account.enabled);
	fSignInAgain = new BButton("signInAgain", "Sign In Again",
		new BMessage(kMsgSignInAgain));
	BButton* remove = new BButton("remove", "Remove Account" B_UTF8_ELLIPSIS,
		new BMessage(kMsgRemoveAccount));

	BStringView* calendarsLabel = new BStringView("calendarsLabel",
		calendars.empty() ? "No calendars or lists yet"
			: "Calendars and lists (click to show or hide):");

	BLayoutBuilder::Group<>(view)
		.Add(name)
		.Add(type)
		.Add(fStatus)
		.Add(enabled)
		.AddStrut(B_USE_SMALL_SPACING)
		.Add(calendarsLabel)
		.Add(listScroll, 1)
		.AddGroup(B_HORIZONTAL)
			.Add(syncNow)
			.Add(fSignInAgain)
			.AddGlue()
			.Add(remove)
		.End();

	SetVisible(enabled, !local);
	SetVisible(syncNow, !local);
	SetVisible(remove, !local);
	SetVisible(fSignInAgain, NeedsSignIn(account));
	return view;
}


void
AccountsWindow::ToggleCalendar(const std::string& calendarId)
{
	Store& store = TheApp()->Store();
	bool visible;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* calendar = store.FindCalendar(calendarId);
		if (calendar == NULL)
			return;
		calendar->visible = !calendar->visible;
		visible = calendar->visible;
	}
	store.Changed();
	TheApp()->ScheduleSave();

	// Update the row in place (a rebuild would lose the scroll position).
	for (int32 i = 0; fCalendars != NULL && i < fCalendars->CountItems(); i++) {
		CalendarItem* item = dynamic_cast<CalendarItem*>(fCalendars->ItemAt(i));
		if (item != NULL && item->Id() == calendarId) {
			item->SetVisible(visible);
			fCalendars->InvalidateItem(i);
		}
	}
	fDetailsSignature = DetailsSignature(fDetailsId);
}


void
AccountsWindow::SetEnabled(bool enabled)
{
	std::string id = SelectedId();
	Store& store = TheApp()->Store();
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Account* account = store.FindAccount(id);
		if (account == NULL)
			return;
		account->enabled = enabled;
		if (!enabled)
			account->lastError.clear();
	}
	store.Changed();
	TheApp()->ScheduleSave();
	if (enabled)
		TheApp()->Sync().Request(id, false);
	RefreshList();
	RefreshDetails(false);
}


void
AccountsWindow::RemoveAccount()
{
	std::string id = SelectedId();
	std::string name;
	{
		Store& store = TheApp()->Store();
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Account* account = store.FindAccount(id);
		if (account == NULL || account->type == AccountType::Local)
			return;
		name = account->name;
	}
	std::string text = "Remove the account \"" + name + "\"?\n\nIts calendars, "
		"events and tasks are removed from airCal. Nothing is deleted on the "
		"server.";
	BAlert* alert = new BAlert("Remove Account", text.c_str(), "Cancel",
		"Remove", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	if (alert->Go() != 1)
		return;

	Store& store = TheApp()->Store();
	TheApp()->Secrets().Remove("account:" + id);
	store.RemoveAccount(id);
	store.Changed();
	TheApp()->ScheduleSave();
	RefreshList();
	RefreshDetails(true);
}


void
AccountsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAccountSelected:
			RefreshDetails(false);
			break;
		case kMsgTick:
		case kMsgStoreChanged:
		case kMsgSyncStatus:
			// Account rows show relative times: refresh them on each tick.
			RefreshList();
			RefreshDetails(false);
			break;
		case kMsgAccountAdded:
		{
			const char* id;
			if (message->FindString("account", &id) == B_OK)
				Select(id);
			else
				RefreshList();
			break;
		}
		case kMsgEnabled:
			SetEnabled(message->GetInt32("be:value", 0) == B_CONTROL_ON);
			break;
		case kMsgCalendarToggled:
		{
			const char* id;
			if (message->FindString("calendar", &id) == B_OK)
				ToggleCalendar(id);
			break;
		}
		case kMsgSyncAccount:
		{
			std::string id = SelectedId();
			if (!id.empty()) {
				TheApp()->Sync().Request(id, false);
				if (fStatus != NULL)
					fStatus->SetText("Syncing" B_UTF8_ELLIPSIS);
			}
			break;
		}
		case kMsgSignInAgain:
		{
			std::string id = SelectedId();
			Store& store = TheApp()->Store();
			AccountType type;
			{
				std::lock_guard<std::recursive_mutex> lock(store.Mutex());
				Account* account = store.FindAccount(id);
				if (account == NULL)
					break;
				type = account->type;
			}
			OpenSignInWindow(type, id, this);
			break;
		}
		case kMsgRemoveAccount:
			RemoveAccount();
			break;
		case kMsgAddGoogle:
			OpenSignInWindow(AccountType::Google, "", this);
			break;
		case kMsgAddCalDav:
			OpenSignInWindow(AccountType::CalDAV, "", this);
			break;
		case kMsgAddTodoist:
			OpenSignInWindow(AccountType::Todoist, "", this);
			break;
		case B_COLORS_UPDATED:
			Theme::Refresh();
			RefreshDetails(true);
			BWindow::MessageReceived(message);
			break;
		default:
			BWindow::MessageReceived(message);
	}
}

}	// namespace


void
OpenAccountsWindow()
{
	if (ActivateRegistered(kAccountsKey))
		return;
	AccountsWindow* window = new AccountsWindow();
	Register(kAccountsKey, window);
	PlaceWindow(window, -1, -1);
	window->Show();
}


void
OpenAddAccount(const std::string& type)
{
	AccountType accountType = AccountTypeFromName(type);
	if (accountType == AccountType::Local)
		return;
	OpenSignInWindow(accountType, "", DialogKit::MainBWindow());
}
