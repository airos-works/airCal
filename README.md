# airCal

Calendars and to-dos for [air/OS](https://airos.works), in the spirit of
Fantastical: type what you mean, see it in a clean day, week, month or year
view, and keep it in sync with the services you already use.

- **Natural-language entry.** "Lunch with Sam at Cafe Rio tomorrow 12:30pm",
  "Standup every weekday at 9:15am for 15 minutes /Work",
  "todo Pay rent due Nov 1 !!", "remind me to call Mum tomorrow at 6pm".
  A live preview under the field shows what will be created.
- **Day, week, month and year views**, a mini month with busy-day dots, and an
  upcoming list (events and tasks by day) in the sidebar. Drag on empty space
  to create an event, drag events to move them (across days too), drag the
  bottom edge to change the length. Recurring events ask whether a change is
  for this event, future events or all events.
- **Event details**: time zones (shown and typed in the event's own zone, with
  a zone picker), invitees (sent as Google invitations or CalDAV ATTENDEEs for
  servers that schedule), alerts, repeats, URL and video-call links, notes.
- **Undo and redo** (Edit menu) for every change made in airCal; an undone
  change syncs like any other edit.
- **Tasks**: Today (with overdue), Upcoming, All (by list), Completed and per
  list, with priorities, labels, subtasks, due dates and times, recurring
  tasks, and tasks shown in the calendar on their due date.
- **Sync**
  - **Google Calendar** (all calendars, colours, reminders, recurring events and
    their exceptions, incremental sync) and **Google Tasks**.
  - **CalDAV**: iCloud (with an app-specific password), Fastmail, Nextcloud,
    Radicale, Yahoo and others; events and VTODO tasks; edits keep every
    property airCal doesn't know about.
  - **Todoist** (API v1): sign in with the browser, or paste your API token,
    and your existing projects and tasks appear. Recurring Todoist dates
    ("every monday") are kept and completing one moves it to the next date.
  - Works offline: changes are kept locally and sent when the service is
    reachable again.
- **Alerts** as air/OS notifications for events and timed tasks.
- **Light and dark**: airCal takes all its colours from the system palette and
  follows air/OS's light/dark switch live.
- Import `.ics` files (drop them on airCal or File › Import) and export events.

<!-- airos-ci:latest-builds:start -->
## Latest builds

Built automatically by air/OS CI from commit `3092d61` on 2026-11-03 ([all files](https://github.com/airos-works/airCal/releases/tag/latest)).

| Architecture | Package |
|---|---|
| arm64 | [aircal-1.0.0-3-arm64.hpkg](https://github.com/airos-works/airCal/releases/download/latest/aircal-1.0.0-3-arm64.hpkg) |
| x86_64 | [aircal-1.0.0-3-x86_64.hpkg](https://github.com/airos-works/airCal/releases/download/latest/aircal-1.0.0-3-x86_64.hpkg) |

Install with `pkgman install <file>`, or copy the file into `/boot/system/packages`. Haiku SDK: x86_64 hrev60207-2447-gcbdd6fbb4a, arm64 hrev60207-2447-gcbdd6fbb4a.
<!-- airos-ci:latest-builds:end -->

## Accounts

**Calendar › Accounts…** (or **Add Account…** in the sidebar's Calendars tab).

### Todoist
*Sign in with Todoist* opens your browser; after you allow access, airCal
receives the sign-in on a local address and your projects and tasks appear.
Alternatively paste your API token (Todoist: Settings › Integrations ›
Developer). airCal registers itself with Todoist on the fly (OAuth dynamic
client registration with PKCE), so there is nothing to set up.

### Google Calendar and Google Tasks
Google requires every desktop application to use an OAuth client registered
in Google Cloud. Until airCal ships with a published client, create your own
(free, a few minutes):

1. In [Google Cloud Console](https://console.cloud.google.com/), create a
   project and enable the **Google Calendar API** and the **Google Tasks API**.
2. Configure the consent screen (Google Auth platform › Branding/Audience); for
   a personal account choose *External* and add yourself as a test user.
3. Clients › Create client › **Desktop app**, then download its JSON.
4. Copy it to `~/config/settings/airCal/google-client.json`, or paste the client
   id and secret into the Google sign-in sheet in airCal. A client already set
   up for Mail (`~/config/settings/Mail/google-client.json`) works too if the
   two APIs are enabled for it.

Then *Sign in with Google*. airCal asks for calendar and tasks access only.

### CalDAV
Enter the server address, user name and password. airCal finds the calendars
through the standard discovery (`/.well-known/caldav`, principal, calendar
home). For iCloud use `https://caldav.icloud.com` and an
[app-specific password](https://support.apple.com/102654).

Passwords and tokens are kept in the air/OS keystore.

## Building

On air/OS (needs `curl_devel`):

    make            # build-haiku/airCal
    make package    # artifacts/aircal-<version>-<arch>.hpkg
    make check      # core tests

Cross-compiling on Linux with an air/OS SDK (the build server's layout):

    tools/build-cross.sh x86_64      # or arm64
    make check-host                  # core tests on Linux (needs ICU dev files)

## Testing without real accounts

- `tools/fake-cloud.py` is a faithful stand-in for Google (OAuth, Calendar v3,
  Tasks v1) and Todoist (OAuth with dynamic registration, API v1) with seed
  data; `tools/test-fake-cloud.sh` checks it. Start airCal with
  `AIRCAL_CLOUD_BASE=http://host:18480` to use it.
- `tools/caldav-server.sh --daemon --seed` runs Radicale with seeded calendars
  (user `tester`, password `secret`).
- `make integration-host` runs the real providers end to end against both.
  `AIRCAL_CALDAV_URL=… AIRCAL_CALDAV_USER=… AIRCAL_CALDAV_PASSWORD=…
  build-host/aircal_integration generic` checks any other CalDAV server; it
  passes on Radicale 3.9, Xandikos and Nextcloud (sabre/dav).
- `AIRCAL_FILE_SECRETS=1` keeps credentials in the data folder instead of the
  keyring (for test runs).
- `tools/vm/run-vm.sh` starts a Haiku test VM (VNC 5914, SSH 2235) where the
  host is `10.0.2.2`.
- `AIRCAL_SETTINGS_DIR` points airCal at another data folder.

Scripting: `hey airCal 'svew' with view=month`, `hey airCal 'gdat' with
date=2026-11-03`, `hey airCal 'qent' with text="Lunch tomorrow at noon"`,
`hey airCal 'sync'`, and for tasks `hey airCal stsk with title="…"` plus
`and newTitle="…"`, `and due=2026-11-06`, `and completed=true` or
`and delete=true` (the reply gives the task's ids); `qent` replies with the id
of what it made.

## Layout

- `src/core`: model, store, iCalendar, recurrence rules, quick-entry parser,
  editing (portable, tested on Linux)
- `src/sync`: Google, CalDAV and Todoist providers, OAuth, the sync engine
- `src/net`: libcurl transport
- `src/ui`, `src/platform`: the air/OS application

## License

MIT. Includes [nlohmann/json](vendor/nlohmann) (MIT).
