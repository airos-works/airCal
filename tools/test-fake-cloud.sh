#!/usr/bin/env bash
# Smoke test for tools/fake-cloud.py: starts it on a free port and drives the
# Google (OAuth+PKCE, Calendar v3, Tasks v1) and Todoist (DCR, OAuth+PKCE, API v1)
# endpoints with curl, the way airCal does. Exits non-zero on any failure.
#
#   tools/test-fake-cloud.sh [-v]      (-v prints the server log at the end)
set -uo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VERBOSE=${1:-}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fake-cloud-test.XXXXXX")
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
B=http://127.0.0.1:$PORT
python3 "$HERE/fake-cloud.py" --host 127.0.0.1 --port "$PORT" >"$WORK/server.log" 2>&1 &
PID=$!
cleanup() {
	kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
	[[ $VERBOSE == -v ]] && cat "$WORK/server.log"
	rm -rf "$WORK"
}
trap cleanup EXIT
for _ in $(seq 50); do curl -sf "$B/" >/dev/null && break; sleep 0.1; done

PASS=0 FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $*"; }
# req METHOD URL [curl args...] -> sets $CODE, body in $WORK/body, headers in $WORK/hdr
req() {
	local m=$1 u=$2; shift 2
	CODE=$(curl -s -X "$m" -D "$WORK/hdr" -o "$WORK/body" -w '%{http_code}' "$@" "$u")
}
# jget EXPR: evaluate a Python expression on the last body (bound to d)
jget() { python3 -c 'import json,sys; d=json.load(open(sys.argv[2])); r=eval(sys.argv[1]); print(r if not isinstance(r,(dict,list)) else json.dumps(r))' "$1" "$WORK/body"; }
expect() { # expect CODE "description" [python-condition-on-d]
	local want=$1 what=$2 cond=${3:-True}
	if [[ $CODE != "$want" ]]; then bad "$what: HTTP $CODE (want $want): $(head -c 300 "$WORK/body")"; return 1; fi
	if [[ $cond != True && $(jget "bool($cond)" 2>&1) != True ]]; then bad "$what: condition failed: $cond :: $(head -c 400 "$WORK/body")"; return 1; fi
	ok "$what"
}
qp() { python3 -c 'import sys,urllib.parse as u; print(dict(u.parse_qsl(u.urlsplit(sys.argv[1]).query)).get(sys.argv[2],""))' "$1" "$2"; }
enc() { python3 -c 'import sys,urllib.parse as u; print(u.quote(sys.argv[1], safe=""))' "$1"; }
pkce() { python3 -c 'import sys,hashlib,base64; print(base64.urlsafe_b64encode(hashlib.sha256(sys.argv[1].encode()).digest()).rstrip(b"=").decode())' "$1"; }
location() { tr -d '\r' <"$WORK/hdr" | sed -n 's/^[Ll]ocation: //p'; }

REDIR=http://127.0.0.1:54321/oauth/callback
VERIFIER=$(python3 -c 'import secrets; print(secrets.token_urlsafe(48))')
CHALLENGE=$(pkce "$VERIFIER")

echo "== Google OAuth"
SCOPE='openid email https://www.googleapis.com/auth/calendar https://www.googleapis.com/auth/tasks'
gauth() {
	req GET "$B/google/o/oauth2/v2/auth" -G --data-urlencode "client_id=aircal.apps.googleusercontent.com" \
		--data-urlencode "redirect_uri=$REDIR" --data-urlencode response_type=code --data-urlencode "scope=$SCOPE" \
		--data-urlencode state=xyz123 --data-urlencode "code_challenge=$CHALLENGE" \
		--data-urlencode code_challenge_method=S256 --data-urlencode access_type=offline --data-urlencode prompt=consent
}
gauth
LOC=$(location); CODE1=$(qp "$LOC" code)
[[ $CODE == 302 && $LOC == "$REDIR?"* && $(qp "$LOC" state) == xyz123 && -n $CODE1 ]] && ok "auth redirects with code+state" || bad "auth redirect: $CODE $LOC"
req POST "$B/google/token" -d "grant_type=authorization_code" --data-urlencode "code=$CODE1" \
	--data-urlencode "code_verifier=wrong-verifier-wrong-verifier-wrong-verifier" -d client_id=aircal.apps.googleusercontent.com \
	--data-urlencode "redirect_uri=$REDIR"
expect 400 "token exchange rejects a bad PKCE verifier" "d['error']=='invalid_grant'"
gauth; CODE2=$(qp "$(location)" code)
req POST "$B/google/token" -d "grant_type=authorization_code" --data-urlencode "code=$CODE2" \
	--data-urlencode "code_verifier=$VERIFIER" -d client_id=aircal.apps.googleusercontent.com \
	-d client_secret=whatever --data-urlencode "redirect_uri=$REDIR"
expect 200 "code+PKCE exchange" "d['token_type']=='Bearer' and d['expires_in']==3600 and d['refresh_token'] and d['id_token'].count('.')==2"
GAT=$(jget "d['access_token']"); GRT=$(jget "d['refresh_token']")
req POST "$B/google/token" -d "grant_type=authorization_code" --data-urlencode "code=$CODE2" \
	--data-urlencode "code_verifier=$VERIFIER" -d client_id=aircal.apps.googleusercontent.com --data-urlencode "redirect_uri=$REDIR"
expect 400 "auth code is single use" "d['error']=='invalid_grant'"
GA=(-H "Authorization: Bearer $GAT")
req GET "$B/google/oauth2/v3/userinfo" "${GA[@]}"
expect 200 "userinfo" "d['email']=='tester@example.com' and d['sub']"
req GET "$B/google/calendar/v3/users/me/calendarList"
expect 401 "calendarList without token is 401" "d['error']['status']=='UNAUTHENTICATED'"

echo "== Google Calendar"
req GET "$B/google/calendar/v3/users/me/calendarList" "${GA[@]}"
expect 200 "calendarList" "len(d['items'])==3 and [c for c in d['items'] if c.get('primary')][0]['id']=='tester@example.com' and 'nextSyncToken' in d and {c['accessRole'] for c in d['items']}=={'owner','reader'}"
WORKCAL=$(jget "[c['id'] for c in d['items'] if c['summary']=='Work'][0]")
HOLCAL=$(jget "[c['id'] for c in d['items'] if c['accessRole']=='reader'][0]")
W=$B/google/calendar/v3/calendars/$(enc "$WORKCAL")/events
req GET "$W" "${GA[@]}"
expect 200 "Work events, singleEvents=false: masters + both exceptions" \
	"len([e for e in d['items'] if e.get('recurringEventId')])==2 and any(e.get('status')=='cancelled' for e in d['items']) and any(e.get('recurrence') for e in d['items']) and d['nextSyncToken']"
SYNC=$(jget "d['nextSyncToken']")
TEAM=$(jget "[e['id'] for e in d['items'] if e.get('summary')=='Team sync' and e.get('recurrence')][0]")
STANDUP=$(jget "[e['id'] for e in d['items'] if e.get('summary')=='Daily standup'][0]")
SPRINT=$(jget "[e['id'] for e in d['items'] if e.get('summary')=='Sprint planning'][0]")
read -r TMIN TMAX WED THU <<<"$(python3 -c '
from datetime import *; from zoneinfo import ZoneInfo
tz=ZoneInfo("Australia/Hobart"); t=datetime.now(tz).date(); m=t-timedelta(days=t.weekday())
f=lambda d,h=0,mi=0: datetime.combine(d,time(h,mi),tz)
print(f(m).isoformat(), f(m+timedelta(14)).isoformat(),
      f(m+timedelta(2),9,15).astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ"),
      f(m+timedelta(3),9,15).astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))')"
req GET "$W" "${GA[@]}" -G --data-urlencode "timeMin=$TMIN" --data-urlencode "timeMax=$TMAX" -d singleEvents=true -d orderBy=startTime
expect 200 "singleEvents=true expands (moved instance shown, cancelled hidden)" \
	"sum(e['summary']=='Team sync (moved)' for e in d['items'])==1 and sum(e['summary']=='Team sync' for e in d['items'])==0 and sum(e['summary']=='Daily standup' for e in d['items'])==10 and all(e['status']=='confirmed' for e in d['items']) and [e['start'].get('dateTime',e['start'].get('date')) for e in d['items']]==sorted(e['start'].get('dateTime',e['start'].get('date')) for e in d['items'])"
req GET "$W" "${GA[@]}" -G --data-urlencode "timeMin=$TMIN" --data-urlencode "timeMax=$TMAX" -d singleEvents=true -d showDeleted=true
expect 200 "singleEvents=true&showDeleted=true includes the cancelled instance" "sum(e.get('status')=='cancelled' for e in d['items'])==1"
req GET "$B/google/calendar/v3/calendars/$(enc "$WORKCAL")/events/$TEAM/instances" "${GA[@]}" -G --data-urlencode "timeMin=$TMIN" --data-urlencode "timeMax=$TMAX"
expect 200 "events/{id}/instances" "len(d['items'])==1 and d['items'][0]['summary']=='Team sync (moved)'"
req PATCH "$W/$SPRINT" "${GA[@]}" -H 'Content-Type: application/json' -d '{"summary":"Sprint planning (room 5)","location":"Room 5"}'
expect 200 "PATCH event" "d['summary']=='Sprint planning (room 5)' and d['start']['dateTime']"
ETAG=$(jget "d['etag']")
req PATCH "$W/$SPRINT" "${GA[@]}" -H 'If-Match: "1"' -H 'Content-Type: application/json' -d '{"summary":"x"}'
expect 412 "PATCH with stale If-Match"
req GET "$W" "${GA[@]}" -G --data-urlencode "syncToken=$SYNC"
expect 200 "syncToken incremental returns only the patched event" "[e['id'] for e in d['items']]==['$SPRINT'] and d['items'][0]['etag']=='$(sed 's/"/\\"/g' <<<"$ETAG")' and d['nextSyncToken']"
SYNC2=$(jget "d['nextSyncToken']")
req PATCH "$W/${STANDUP}_$WED" "${GA[@]}" -H 'Content-Type: application/json' -d '{"summary":"Standup (demo day)"}'
expect 200 "PATCH instance creates an exception" "d['id']=='${STANDUP}_$WED' and d['recurringEventId']=='$STANDUP' and d['originalStartTime']['dateTime'] and d['summary']=='Standup (demo day)' and 'recurrence' not in d"
req DELETE "$W/${STANDUP}_$THU" "${GA[@]}"
expect 204 "DELETE instance"
req GET "$W/${STANDUP}_$THU" "${GA[@]}"
expect 200 "GET the deleted instance shows status cancelled" "d['status']=='cancelled'"
req GET "$W/${STANDUP}_20200101T000000Z" "${GA[@]}"
expect 404 "GET of a non-occurrence instance id is 404"
req GET "$W" "${GA[@]}" -G --data-urlencode "syncToken=$SYNC2"
expect 200 "syncToken sees the modified and the cancelled instance" \
	"sorted((e['id'],e['status']) for e in d['items'])==sorted([('${STANDUP}_$WED','confirmed'),('${STANDUP}_$THU','cancelled')])"
req GET "$W" "${GA[@]}" -G --data-urlencode "timeMin=$TMIN" --data-urlencode "timeMax=$TMAX" -d singleEvents=true
expect 200 "expansion uses the exceptions" "sum(e['summary']=='Daily standup' for e in d['items'])==8 and sum(e['summary']=='Standup (demo day)' for e in d['items'])==1"
req GET "$W" "${GA[@]}" -G --data-urlencode "syncToken=bogus"
expect 410 "unknown syncToken is 410 Gone" "d['error']['errors'][0]['reason']=='fullSyncRequired'"
P=$B/google/calendar/v3/calendars/primary/events
req POST "$P" "${GA[@]}" -H 'Content-Type: application/json' \
	-d '{"summary":"Smoke test","start":{"dateTime":"2026-11-20T10:00:00+11:00"},"end":{"dateTime":"2026-11-20T11:00:00+11:00"},"reminders":{"useDefault":false,"overrides":[{"method":"popup","minutes":5}]}}'
expect 200 "insert event" "d['status']=='confirmed' and d['iCalUID'].endswith('@google.com') and d['etag'] and d['htmlLink'] and d['created']"
NEW=$(jget "d['id']")
req POST "$P" "${GA[@]}" -H 'Content-Type: application/json' \
	-d '{"summary":"bad","start":{"dateTime":"2026-11-20T10:00:00+11:00"},"end":{"dateTime":"2026-11-20T11:00:00+11:00"},"recurrence":["RRULE:FREQ=DAILY"]}'
expect 400 "recurring timed event without timeZone is rejected (like Google)"
req PUT "$P/$NEW" "${GA[@]}" -H 'Content-Type: application/json' \
	-d '{"summary":"Smoke test (put)","start":{"date":"2026-11-21"},"end":{"date":"2026-11-22"}}'
expect 200 "PUT replaces the event" "d['summary']=='Smoke test (put)' and d['start']=={'date':'2026-11-21'} and d['reminders']=={'useDefault':True} and d['sequence']==1"
req DELETE "$P/$NEW" "${GA[@]}"
expect 204 "DELETE event"
req DELETE "$P/$NEW" "${GA[@]}"
expect 410 "DELETE again is 410"
req GET "$P" "${GA[@]}" -d showDeleted=true -G
expect 200 "showDeleted lists the cancelled event" "any(e['id']=='$NEW' and e['status']=='cancelled' for e in d['items'])"
req GET "$P" "${GA[@]}" -d maxResults=3 -G
expect 200 "maxResults paginates" "len(d['items'])==3 and 'nextPageToken' in d and 'nextSyncToken' not in d"
N=3; PT=$(jget "d['nextPageToken']")
while [[ -n $PT ]]; do
	req GET "$P" "${GA[@]}" -G -d maxResults=3 --data-urlencode "pageToken=$PT"
	N=$((N + $(jget "len(d['items'])"))); PT=$(jget "d.get('nextPageToken','')")
	LASTSYNC=$(jget "d.get('nextSyncToken','')")
done
[[ $N == 10 && -n $LASTSYNC ]] && ok "pages add up to the 10 primary events; last page has nextSyncToken" || bad "pagination total $N sync '$LASTSYNC'"
req POST "$B/google/calendar/v3/calendars/$(enc "$HOLCAL")/events" "${GA[@]}" -H 'Content-Type: application/json' \
	-d '{"summary":"x","start":{"date":"2026-11-21"},"end":{"date":"2026-11-22"}}'
expect 403 "read-only calendar refuses writes"

echo "== Google token refresh / expiry"
req POST "$B/google/admin-nonexistent"
expect 404 "unknown path is 404"
req POST "$B/admin/expire-tokens"
expect 200 "admin expire-tokens"
req GET "$B/google/calendar/v3/users/me/calendarList" "${GA[@]}"
expect 401 "expired access token is 401"
req POST "$B/google/token" -d grant_type=refresh_token --data-urlencode "refresh_token=$GRT" -d client_id=aircal.apps.googleusercontent.com
expect 200 "refresh_token grant" "d['access_token'] and 'refresh_token' not in d and d['expires_in']==3600"
GAT=$(jget "d['access_token']"); GA=(-H "Authorization: Bearer $GAT")
req POST "$B/google/token" -d grant_type=refresh_token -d refresh_token=nope -d client_id=aircal.apps.googleusercontent.com
expect 400 "bad refresh token is invalid_grant" "d['error']=='invalid_grant'"
req GET "$B/google/calendar/v3/users/me/calendarList" "${GA[@]}"
expect 200 "refreshed token works"

echo "== Google Tasks"
T=$B/google/tasks/v1
req GET "$T/users/@me/lists" "${GA[@]}"
expect 200 "task lists" "[l['title'] for l in d['items']]==['My Tasks','Groceries']"
TL=$(jget "d['items'][0]['id']")
req GET "$T/lists/$TL/tasks" "${GA[@]}"
expect 200 "tasks (completed shown by default)" "len(d['items'])==5 and any(t['status']=='completed' and t['completed'] for t in d['items']) and any(t.get('parent') for t in d['items']) and all(t['updated'].endswith('Z') for t in d['items'])"
PARENT=$(jget "[t['id'] for t in d['items'] if t['title']=='Renew passport'][0]")
req GET "$T/lists/$TL/tasks" "${GA[@]}" -G -d showCompleted=false -d maxResults=2
expect 200 "showCompleted=false + maxResults" "len(d['items'])==2 and 'nextPageToken' in d and all(t['status']=='needsAction' for t in d['items'])"
req POST "$T/lists/$TL/tasks?parent=$PARENT" "${GA[@]}" -H 'Content-Type: application/json' -d '{"title":"Fill in form","due":"2026-11-10T15:30:00.000Z","notes":"PC7"}'
expect 200 "insert subtask" "d['parent']=='$PARENT' and d['due']=='2026-11-10T00:00:00.000Z' and d['status']=='needsAction' and d['position']=='00000000000000000000'"
TID=$(jget "d['id']")
req PATCH "$T/lists/$TL/tasks/$TID" "${GA[@]}" -H 'Content-Type: application/json' -d '{"title":"Fill in PC7 form"}'
expect 200 "patch task" "d['title']=='Fill in PC7 form' and d['notes']=='PC7'"
req PATCH "$T/lists/$TL/tasks/$TID" "${GA[@]}" -H 'Content-Type: application/json' -d '{"status":"completed"}'
expect 200 "complete task" "d['status']=='completed' and d['completed'].endswith('Z')"
req PATCH "$T/lists/$TL/tasks/$TID" "${GA[@]}" -H 'Content-Type: application/json' -d '{"status":"needsAction"}'
expect 200 "uncomplete task clears completed" "d['status']=='needsAction' and 'completed' not in d"
req DELETE "$T/lists/$TL/tasks/$TID" "${GA[@]}"
expect 204 "delete task"
req GET "$T/lists/$TL/tasks" "${GA[@]}" -G -d showDeleted=true -d showHidden=true
expect 200 "showDeleted shows it as deleted" "any(t['id']=='$TID' and t.get('deleted') for t in d['items'])"
req POST "$T/lists/$TL/clear" "${GA[@]}"
expect 204 "clear completed"
req GET "$T/lists/$TL/tasks" "${GA[@]}"
expect 200 "cleared tasks are hidden" "not any(t['status']=='completed' for t in d['items'])"
req POST "$T/users/@me/lists" "${GA[@]}" -H 'Content-Type: application/json' -d '{"title":"Smoke"}'
expect 200 "insert task list" "d['title']=='Smoke'"

echo "== Todoist OAuth"
req POST "$B/todoist/oauth/register" -H 'Content-Type: application/json' -d '{"client_name":"x"}'
expect 400 "registration needs redirect_uris" "d['error']=='invalid_redirect_uri'"
req POST "$B/todoist/oauth/register" -H 'Content-Type: application/json' \
	-d "{\"client_name\":\"airCal\",\"redirect_uris\":[\"$REDIR\"],\"token_endpoint_auth_method\":\"none\",\"grant_types\":[\"authorization_code\",\"refresh_token\"]}"
expect 201 "dynamic registration (public client)" "d['client_id'].startswith('tdd_') and 'client_secret' not in d"
TCID=$(jget "d['client_id']")
tauth() {
	req GET "$B/todoist/oauth/authorize" -G --data-urlencode "client_id=$TCID" --data-urlencode "scope=data:read_write,data:delete" \
		--data-urlencode state=s7 --data-urlencode "redirect_uri=$REDIR" -d response_type=code \
		--data-urlencode "code_challenge=$CHALLENGE" -d code_challenge_method=S256
}
tauth; TCODE=$(qp "$(location)" code)
[[ $CODE == 302 && $(qp "$(location)" state) == s7 && -n $TCODE ]] && ok "todoist authorize redirects" || bad "todoist authorize: $CODE"
req POST "$B/todoist/oauth/access_token" -d "client_id=$TCID" --data-urlencode "code=$TCODE" -d grant_type=authorization_code --data-urlencode "code_verifier=$VERIFIER"
expect 200 "todoist code+PKCE exchange" "d['token_type']=='Bearer' and d['refresh_token'] and d['expires_in']==3600"
TAT=$(jget "d['access_token']"); TRT=$(jget "d['refresh_token']")
req POST "$B/todoist/oauth/access_token" -d "client_id=$TCID" -d grant_type=refresh_token --data-urlencode "refresh_token=$TRT"
expect 200 "todoist refresh rotates" "d['refresh_token']!='$TRT'"
TAT=$(jget "d['access_token']")
req POST "$B/todoist/oauth/access_token" -d "client_id=$TCID" -d grant_type=refresh_token --data-urlencode "refresh_token=$TRT"
expect 400 "old todoist refresh token is dead" "d['error']=='invalid_grant'"
req POST "$B/todoist/oauth/register" -H 'Content-Type: application/json' -d "{\"redirect_uris\":[\"$REDIR\"],\"token_endpoint_auth_method\":\"client_secret_post\"}"
expect 201 "dynamic registration (confidential client)" "d['client_secret']"
CCID=$(jget "d['client_id']"); CSEC=$(jget "d['client_secret']")
req GET "$B/todoist/oauth/authorize" -G -d "client_id=$CCID" -d scope=data:read_write -d state=q --data-urlencode "redirect_uri=$REDIR"
CC=$(qp "$(location)" code)
req POST "$B/todoist/oauth/access_token" -d "client_id=$CCID" -d client_secret=wrong --data-urlencode "code=$CC"
expect 401 "confidential client with a wrong secret" "d['error']=='invalid_client'"

echo "== Todoist API v1"
A=$B/todoist/api/v1
TA=(-H "Authorization: Bearer $TAT")
req GET "$A/projects" -H 'Authorization: Bearer nope'
expect 401 "bad token is 401" "d['error_tag']=='AUTH_INVALID_TOKEN' and d['http_code']==401"
req GET "$A/user" -H 'Authorization: Bearer test-todoist-token'
expect 200 "personal API token accepted; /user" "d['email']=='tester@example.com' and d['inbox_project_id'] and d['tz_info']['timezone']"
INBOX=$(jget "d['inbox_project_id']")
req GET "$A/projects" "${TA[@]}"
expect 200 "projects" "[p['name'] for p in d['results']][:1]==['Inbox'] and d['results'][0]['inbox_project'] and any(p['parent_id'] for p in d['results']) and d['next_cursor'] is None"
WORKP=$(jget "[p['id'] for p in d['results'] if p['name']=='Work'][0]")
req GET "$A/sections" "${TA[@]}" -G -d "project_id=$WORKP"
expect 200 "sections" "len(d['results'])==2"
req GET "$A/labels" "${TA[@]}"
expect 200 "labels" "len(d['results'])==4"
req GET "$A/tasks" "${TA[@]}" -G -d limit=5
expect 200 "tasks limit=5" "len(d['results'])==5 and d['next_cursor']"
N=5; CUR=$(jget "d['next_cursor']")
while [[ -n $CUR && $CUR != None ]]; do
	req GET "$A/tasks" "${TA[@]}" -G -d limit=5 --data-urlencode "cursor=$CUR"
	N=$((N + $(jget "len(d['results'])"))); CUR=$(jget "d['next_cursor'] or ''")
done
[[ $N == 13 ]] && ok "cursor pages add up to 13 active tasks" || bad "tasks pagination total $N"
req GET "$A/tasks" "${TA[@]}" -G -d limit=500
expect 400 "limit>200 rejected"
req POST "$A/tasks" "${TA[@]}" -H 'Content-Type: application/json' -d '{"content":"Smoke task","due_datetime":"2026-11-05T14:00:00Z","priority":4,"labels":["errand"],"duration":30,"duration_unit":"minute","deadline_date":"2026-11-07"}'
expect 200 "create task" "d['project_id']=='$INBOX' and d['due']=={'date':'2026-11-05T14:00:00Z','timezone':'UTC','string':'Nov 5 2:00 PM','lang':'en','is_recurring':False} and d['duration']=={'amount':30,'unit':'minute'} and d['deadline']['date']=='2026-11-07' and not d['checked']"
TT=$(jget "d['id']")
req POST "$A/tasks/$TT" "${TA[@]}" -H 'Content-Type: application/json' -d '{"content":"Smoke task (edited)","due_date":"2026-11-06","priority":2}'
expect 200 "update task" "d['content']=='Smoke task (edited)' and d['due']['date']=='2026-11-06' and d['due']['timezone'] is None and d['priority']==2"
req POST "$A/tasks/$TT/close" "${TA[@]}"
expect 204 "close task"
req GET "$A/tasks/$TT" "${TA[@]}"
expect 200 "closed task is checked" "d['checked'] and d['completed_at']"
req GET "$A/tasks" "${TA[@]}" -G -d "ids=$TT"
expect 200 "closed task left the active list" "d['results']==[]"
SINCE=$(python3 -c 'from datetime import *; print((datetime.now(timezone.utc)-timedelta(days=7)).strftime("%Y-%m-%dT%H:%M:%SZ"))')
UNTIL=$(python3 -c 'from datetime import *; print((datetime.now(timezone.utc)+timedelta(minutes=5)).strftime("%Y-%m-%dT%H:%M:%SZ"))')
req GET "$A/tasks/completed/by_completion_date" "${TA[@]}" -G --data-urlencode "since=$SINCE" --data-urlencode "until=$UNTIL"
expect 200 "completed by completion date" "len(d['items'])==3 and d['items'][0]['id']=='$TT' and 'next_cursor' in d"
req POST "$A/tasks/$TT/reopen" "${TA[@]}"
expect 204 "reopen task"
req GET "$A/tasks/$TT" "${TA[@]}"
expect 200 "reopened task" "not d['checked'] and d['completed_at'] is None"
req POST "$A/tasks" "${TA[@]}" -H 'Content-Type: application/json' -d '{"content":"Recurring smoke","due_string":"every monday","project_id":"'"$WORKP"'"}'
expect 200 "create recurring task" "d['due']['is_recurring'] and d['due']['string']=='every monday'"
RT=$(jget "d['id']"); RDUE=$(jget "d['due']['date']")
req POST "$A/tasks/$RT/close" "${TA[@]}"
expect 204 "close recurring task"
req GET "$A/tasks/$RT" "${TA[@]}"
expect 200 "recurring close advances due by a week" "not d['checked'] and d['due']['date']=='$(python3 -c "from datetime import *; print(date.fromisoformat('$RDUE')+timedelta(7))")'"
req POST "$A/tasks/$TT/move" "${TA[@]}" -H 'Content-Type: application/json' -d "{\"project_id\":\"$WORKP\"}"
expect 200 "move task" "d['project_id']=='$WORKP'"
req DELETE "$A/tasks/$TT" "${TA[@]}"
expect 204 "delete task"
req GET "$A/tasks/$TT" "${TA[@]}"
expect 404 "deleted task is 404"
req POST "$A/sync" "${TA[@]}" -d sync_token='*' --data-urlencode 'resource_types=["items","projects"]'
expect 200 "read-only /sync full" "d['full_sync'] and len(d['projects'])==4 and d['sync_token']"

echo "== admin"
req POST "$B/admin/reset"
expect 200 "reset"
req GET "$W" "${GA[@]}" -G --data-urlencode "syncToken=$SYNC2"
expect 410 "sync tokens from before a reset are 410"

echo
echo "passed $PASS, failed $FAIL"
[[ $FAIL == 0 ]]
