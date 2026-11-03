#!/usr/bin/env python3
"""fake-cloud.py - in-memory test doubles for the cloud services airCal syncs with.

One plain-HTTP port serves, under path prefixes:

  /google/...   Google OAuth 2.0 (code + PKCE), userinfo, Calendar API v3, Tasks API v1
  /todoist/...  Todoist OAuth (incl. RFC 7591 dynamic registration) and API v1
  /admin/...    reset (reseed), state dump, token expiry, sync-token invalidation

GET / lists the base URLs. Run `fake-cloud.py --help` for the options. Requests are
logged to stderr as "METHOD path status" (paths only: queries can carry codes).
Stdlib only; needs the system tz database (zoneinfo).
"""
import argparse
import base64
import copy
import hashlib
import json
import os
import re
import secrets
import sys
import threading
import time
import uuid
from datetime import date, datetime, time as dtime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, unquote, urlencode, urlsplit
from zoneinfo import ZoneInfo

UTC = timezone.utc
OPTS = None             # argparse namespace
LOCK = threading.RLock()  # every request runs under this lock
S = {}                  # the whole state; JSON-serialisable

# ----------------------------------------------------------------------------- basics


class HTTPError(Exception):
    def __init__(self, status, body=None, headers=None):
        super().__init__(status)
        self.status, self.body, self.headers = status, body, headers or {}


def utcnow():
    return datetime.now(UTC)


def ts_ms(dt=None):  # Google style: 2026-11-03T01:02:03.456Z
    dt = (dt or utcnow()).astimezone(UTC)
    return dt.strftime('%Y-%m-%dT%H:%M:%S.') + '%03dZ' % (dt.microsecond // 1000)


def ts_us(dt=None):  # Todoist style: 2026-11-03T01:02:03.456789Z
    return (dt or utcnow()).astimezone(UTC).strftime('%Y-%m-%dT%H:%M:%S.%fZ')


def parse_dt(s, tzname=None):
    """RFC 3339 -> aware datetime (naive values get tzname, else UTC). None if bad."""
    try:
        d = datetime.fromisoformat(str(s).strip().replace(' ', '+'))  # '+' arrives as ' '
    except ValueError:
        return None
    if d.tzinfo is None:
        d = d.replace(tzinfo=ZoneInfo(tzname) if tzname else UTC)
    return d


def b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b'=').decode()


def b64json(obj):
    return b64url(json.dumps(obj, separators=(',', ':')).encode())


def unb64json(s):
    try:
        return json.loads(base64.urlsafe_b64decode(s + '=' * (-len(s) % 4)))
    except Exception:
        return None


def det_id(kind, name, n=20, alphabet='0123456789abcdefghijklmnopqrstuv'):
    """Deterministic id for seed objects (so tests can hard-code them)."""
    v = int(hashlib.sha1(f'{kind}:{name}'.encode()).hexdigest(), 16)
    out = ''
    for _ in range(n):
        v, r = divmod(v, len(alphabet))
        out += alphabet[r]
    return out


B62 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'


def new_tid():  # Todoist v1 style id
    return ''.join(secrets.choice(B62) for _ in range(16))


def bump(obj):
    S['seq'] += 1
    obj['_seq'] = S['seq']
    return obj


def public(obj):
    return {k: v for k, v in obj.items() if not k.startswith('_')}


def deep_merge(dst, src):
    """PATCH semantics: objects merge, everything else (lists too) replaces, null deletes."""
    for k, v in src.items():
        if v is None:
            dst.pop(k, None)
        elif isinstance(v, dict) and isinstance(dst.get(k), dict):
            deep_merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)


def local_today():
    return datetime.now(ZoneInfo(OPTS.tz)).date()

# ----------------------------------------------------------------------------- OAuth (shared)


def pkce_ok(verifier, challenge, method):
    if not challenge:
        return True
    if not verifier:
        return False
    if (method or 'plain') == 'S256':
        return b64url(hashlib.sha256(verifier.encode()).digest()) == challenge
    return verifier == challenge


def issue_tokens(svc, client_id, scope, with_refresh=True):
    a = S['auth']
    access = ('ya29.fake-' + secrets.token_urlsafe(32)) if svc == 'google' else secrets.token_hex(20)
    a['tokens'][access] = {'svc': svc, 'client_id': client_id, 'scope': scope,
                           'exp': time.time() + OPTS.token_ttl}
    out = {'access_token': access, 'token_type': 'Bearer', 'expires_in': OPTS.token_ttl, 'scope': scope}
    if with_refresh:
        rt = ('1//fake-' + secrets.token_urlsafe(32)) if svc == 'google' else secrets.token_hex(24)
        a['refresh'][rt] = {'svc': svc, 'client_id': client_id, 'scope': scope}
        out['refresh_token'] = rt
    return out


def bearer_ok(req, svc):
    h = req.headers.get('Authorization', '')
    tok = h[7:].strip() if h[:7].lower() == 'bearer ' else ''
    if svc == 'todoist' and tok in OPTS.todoist_token:
        return True
    info = S['auth']['tokens'].get(tok)
    return bool(info and info['svc'] == svc and info['exp'] > time.time())


def redirect_with(uri, params):
    return 302, None, {'Location': uri + ('&' if '?' in uri else '?') + urlencode(params)}


def oauth_authorize(req, svc):
    q = req.q
    client_id, redirect = q.get('client_id'), q.get('redirect_uri')
    if not client_id or not redirect:
        raise HTTPError(400, '<h1>Error 400: invalid_request</h1><p>Missing client_id or redirect_uri.</p>')
    client = None
    if svc == 'todoist':
        client = S['todoist']['clients'].get(client_id)
        if not client:
            raise HTTPError(400, '<h1>invalid_client</h1><p>Unknown client_id.</p>')
        if client.get('redirect_uris') and redirect not in client['redirect_uris']:
            raise HTTPError(400, '<h1>invalid_request</h1><p>redirect_uri is not registered.</p>')
    err = {'state': q['state']} if 'state' in q else {}
    if q.get('response_type', 'code') != 'code':
        return redirect_with(redirect, {'error': 'unsupported_response_type', **err})
    challenge, method = q.get('code_challenge'), q.get('code_challenge_method')
    if challenge and (method or 'plain') not in ('S256', 'plain'):
        return redirect_with(redirect, {'error': 'invalid_request', **err})
    if client and client.get('token_endpoint_auth_method') == 'none' and not challenge:
        return redirect_with(redirect, {'error': 'invalid_request',
                                        'error_description': 'code_challenge required for public clients', **err})
    if q.get('deny') == '1':
        return redirect_with(redirect, {'error': 'access_denied', **err})
    if q.get('interactive') == '1':
        allow = {k: v for k, v in q.items() if k != 'interactive'}
        base = req.path.split('?')[0]
        return 200, ('<!doctype html><title>Fake consent</title><h1>%s test double</h1>'
                     '<p>Client <code>%s</code> asks for <code>%s</code>.</p>'
                     '<p><a id="allow" href="%s?%s">Allow</a> &nbsp; <a id="deny" href="%s?%s">Deny</a></p>'
                     % (svc.title(), client_id, q.get('scope', ''), base, urlencode(allow),
                        base, urlencode({**allow, 'deny': '1'}))), {}
    scope = q.get('scope', '')
    code = ('4/0fake-' if svc == 'google' else '') + secrets.token_urlsafe(24)
    S['auth']['codes'][code] = {'svc': svc, 'client_id': client_id, 'redirect_uri': redirect, 'scope': scope,
                                'challenge': challenge, 'method': method, 'exp': time.time() + 600}
    params = {'code': code, **err}
    if svc == 'google':
        params.update({'scope': scope, 'authuser': '0', 'prompt': 'consent'})
    return redirect_with(redirect, params)


def oauth_error(error, desc=None, status=400):
    body = {'error': error}
    if desc:
        body['error_description'] = desc
    return HTTPError(status, body)


def oauth_token(req, svc):
    f = dict(req.body)
    auth = req.headers.get('Authorization', '')
    if auth[:6].lower() == 'basic ':  # client_secret_basic
        try:
            cid, _, sec = base64.b64decode(auth[6:]).decode().partition(':')
            f.setdefault('client_id', unquote(cid))
            f.setdefault('client_secret', unquote(sec))
        except Exception:
            pass
    gt = f.get('grant_type') or ('authorization_code' if svc == 'todoist' else '')
    cid = f.get('client_id')
    if not cid:
        raise oauth_error('invalid_request', 'Could not determine client ID from request.')
    if svc == 'todoist':
        client = S['todoist']['clients'].get(cid)
        if not client:
            raise oauth_error('invalid_client', 'Unknown client', 401)
        if client.get('token_endpoint_auth_method') != 'none' and f.get('client_secret') != client.get('client_secret'):
            raise oauth_error('invalid_client', 'Bad client credentials', 401)
    a = S['auth']
    if gt == 'authorization_code':
        c = a['codes'].pop(f.get('code', ''), None)
        if not c or c['svc'] != svc or c['exp'] < time.time() or c['client_id'] != cid:
            raise oauth_error('invalid_grant', 'Malformed auth code.')
        if (svc == 'google' or 'redirect_uri' in f) and f.get('redirect_uri') != c['redirect_uri']:
            raise oauth_error('redirect_uri_mismatch' if svc == 'google' else 'invalid_grant', 'Bad Request')
        if not pkce_ok(f.get('code_verifier'), c['challenge'], c['method']):
            raise oauth_error('invalid_grant', 'Missing code verifier.' if not f.get('code_verifier')
                              else 'Invalid code verifier.')
        out = issue_tokens(svc, cid, c['scope'])
        if svc == 'google' and 'openid' in c['scope'].split():
            u, now = S['google']['user'], int(time.time())
            claims = {'iss': 'https://accounts.google.com', 'azp': cid, 'aud': cid, 'sub': u['sub'],
                      'email': u['email'], 'email_verified': True, 'name': u['name'], 'iat': now,
                      'exp': now + OPTS.token_ttl}
            out['id_token'] = '.'.join([b64json({'alg': 'RS256', 'kid': 'fake', 'typ': 'JWT'}),
                                        b64json(claims), b64url(b'fake-signature')])
        return out
    if gt == 'refresh_token':
        rt = f.get('refresh_token', '')
        r = a['refresh'].get(rt)
        if not r or r['svc'] != svc or r['client_id'] != cid:
            raise oauth_error('invalid_grant', 'Token has been expired or revoked.')
        if svc == 'google':  # Google keeps the refresh token
            return issue_tokens(svc, cid, r['scope'], with_refresh=False)
        del a['refresh'][rt]  # Todoist rotates it
        return issue_tokens(svc, cid, r['scope'])
    raise oauth_error('unsupported_grant_type', 'Invalid grant_type: ' + gt)

# ----------------------------------------------------------------------------- Google: errors, auth


GSTATUS = {400: 'INVALID_ARGUMENT', 401: 'UNAUTHENTICATED', 403: 'PERMISSION_DENIED', 404: 'NOT_FOUND',
           409: 'ALREADY_EXISTS', 410: 'FAILED_PRECONDITION', 412: 'FAILED_PRECONDITION'}


def gerr(code, message, reason):
    return HTTPError(code, {'error': {'code': code, 'message': message, 'errors': [
        {'domain': 'global', 'reason': reason, 'message': message}], 'status': GSTATUS.get(code, 'UNKNOWN')}})


def g_require_auth(req):
    if not bearer_ok(req, 'google'):
        raise HTTPError(401, {'error': {'code': 401, 'message': 'Invalid Credentials', 'errors': [
            {'domain': 'global', 'reason': 'authError', 'message': 'Invalid Credentials',
             'locationType': 'header', 'location': 'Authorization'}], 'status': 'UNAUTHENTICATED'}},
            {'WWW-Authenticate': 'Bearer realm="https://accounts.google.com/", error="invalid_token"'})


def g_userinfo(req):
    return dict(S['google']['user'])


def int_param(q, name, default, lo, hi, err):
    try:
        v = int(q.get(name, default))
    except ValueError:
        raise err
    if not lo <= v <= hi:
        raise err
    return v


def sync_token(seq):
    return b64json({'g': S['gen'], 's': seq})


def sync_seq(tok):
    t = unb64json(tok)
    if not isinstance(t, dict) or t.get('g') != S['gen'] or not isinstance(t.get('s'), int) or t['s'] > S['seq']:
        raise gerr(410, 'Sync token is no longer valid, a full sync is required.', 'fullSyncRequired')
    return t['s']


def page_slice(items, q, default, maximum, key='pageToken'):
    """-> (page, next_token or None, seq at the start of this listing)."""
    n = int_param(q, 'maxResults', default, 1, 10 ** 6, gerr(400, 'Invalid maxResults', 'invalid'))
    n = min(n, maximum)
    off, seq0 = 0, S['seq']
    if q.get(key):
        t = unb64json(q[key])
        if not isinstance(t, dict):
            raise gerr(400, 'Invalid page token', 'invalid')
        off, seq0 = t['o'], t['s']
    nxt = b64json({'o': off + n, 's': seq0}) if off + n < len(items) else None
    return items[off:off + n], nxt, seq0

# ----------------------------------------------------------------------------- Google Calendar: recurrence


WD = ['MO', 'TU', 'WE', 'TH', 'FR', 'SA', 'SU']


def parse_ical_dt(v, tzid, default_tz):
    v = v.strip()
    if len(v) == 8:
        return date(int(v[:4]), int(v[4:6]), int(v[6:8]))
    d = datetime.strptime(v[:15], '%Y%m%dT%H%M%S')
    return d.replace(tzinfo=UTC if v.endswith('Z') else ZoneInfo(tzid or default_tz))


def parse_recurrence(lines, tzname):
    rule, ex = None, []
    for line in lines or []:
        name, _, value = line.partition(':')
        parts = name.split(';')
        params = dict(p.split('=', 1) for p in parts[1:] if '=' in p)
        if parts[0].upper() == 'RRULE':
            rule = {k.upper(): v for k, v in (kv.split('=', 1) for kv in value.split(';') if '=' in kv)}
        elif parts[0].upper() == 'EXDATE':
            ex += [parse_ical_dt(v, params.get('TZID'), tzname) for v in value.split(',') if v.strip()]
    return rule, ex


def _byday(rule):
    out = []
    for item in filter(None, rule.get('BYDAY', '').split(',')):
        m = re.fullmatch(r'([+-]?\d+)?(MO|TU|WE|TH|FR|SA|SU)', item.strip().upper())
        if m:
            out.append((int(m[1]) if m[1] else 0, WD.index(m[2])))
    return out


def _nth_weekday(y, m, n, wd):
    first = date(y, m, 1)
    if n > 0:
        d = first + timedelta(days=(wd - first.weekday()) % 7 + 7 * (n - 1))
    else:
        last = (date(y + m // 12, m % 12 + 1, 1) - timedelta(days=1))
        d = last - timedelta(days=(last.weekday() - wd) % 7 + 7 * (-n - 1))
    return d.day if d.month == m else None


MAX_PERIODS = 40000  # hard stop for rules that never match


def iter_rule(start, rule):
    """Candidate local (naive) start datetimes, ascending, >= start. Basic RFC 5545 subset."""
    freq, step = rule.get('FREQ', '').upper(), max(1, int(rule.get('INTERVAL', 1)))
    byday = _byday(rule)
    wds = sorted({wd for _, wd in byday})
    bymd = [int(x) for x in filter(None, rule.get('BYMONTHDAY', '').split(','))]
    if freq == 'DAILY':
        d = start
        for _ in range(MAX_PERIODS):
            if not wds or d.weekday() in wds:
                yield d
            d += timedelta(days=step)
    elif freq == 'WEEKLY':
        week = start - timedelta(days=start.weekday())
        for _ in range(MAX_PERIODS):
            for wd in wds or [start.weekday()]:
                c = week + timedelta(days=wd)
                if c >= start:
                    yield c
            week += timedelta(weeks=step)
    elif freq == 'MONTHLY':
        y, m = start.year, start.month
        for _ in range(MAX_PERIODS):
            if byday:
                days = [_nth_weekday(y, m, n or 1, wd) for n, wd in byday]
            else:
                days = bymd or [start.day]
            for day in sorted(d for d in days if d):
                try:
                    c = start.replace(year=y, month=m, day=day)
                except ValueError:
                    continue
                if c >= start:
                    yield c
            m += step
            y, m = y + (m - 1) // 12, (m - 1) % 12 + 1
    elif freq == 'YEARLY':
        y = start.year
        for _ in range(MAX_PERIODS):
            try:
                yield start.replace(year=y)
            except ValueError:
                pass
            y += step


def ev_tz(ev, caltz):
    return ev['start'].get('timeZone') or caltz


def ev_bounds(ev, caltz):
    out = []
    for k in ('start', 'end'):
        t = ev.get(k) or {}
        if 'dateTime' in t:
            out.append(parse_dt(t['dateTime'], t.get('timeZone') or caltz))
        else:
            out.append(datetime.combine(date.fromisoformat(t['date']), dtime(), ZoneInfo(caltz)))
    return out


def occurrences(ev, caltz, lo=None, hi=None, cap=730):
    """Yield aware original start times of a recurring master overlapping [lo, hi)."""
    rule, ex = parse_recurrence(ev.get('recurrence'), ev_tz(ev, caltz))
    if not rule or rule.get('FREQ', '').upper() not in ('DAILY', 'WEEKLY', 'MONTHLY', 'YEARLY'):
        return
    allday = 'date' in ev['start']
    tz = ZoneInfo(caltz if allday else ev_tz(ev, caltz))
    s0, e0 = ev_bounds(ev, caltz)
    dur = e0 - s0
    until = None
    if 'UNTIL' in rule:
        u = parse_ical_dt(rule['UNTIL'], None, tz.key)
        until = datetime.combine(u, dtime(23, 59, 59), tz) if type(u) is date else u
    count = int(rule['COUNT']) if 'COUNT' in rule else None
    exdates = {x for x in ex if type(x) is date}
    extimes = {x for x in ex if type(x) is not date}
    yielded = 0
    for n, c in enumerate(iter_rule(s0.astimezone(tz).replace(tzinfo=None), rule), 1):
        if (count and n > count) or yielded >= cap:
            return
        occ = c.replace(tzinfo=tz)
        if (until and occ > until) or (hi and occ >= hi):
            return
        if occ.date() in exdates or occ in extimes:
            continue
        if lo and occ + dur <= lo:
            continue
        yielded += 1
        yield occ


def inst_suffix(ev, occ):
    return occ.strftime('%Y%m%d') if 'date' in ev['start'] else occ.astimezone(UTC).strftime('%Y%m%dT%H%M%SZ')


def make_instance(master, occ, caltz):
    inst = {k: copy.deepcopy(v) for k, v in master.items() if not k.startswith('_') and k != 'recurrence'}
    s0, e0 = ev_bounds(master, caltz)
    inst['id'] = master['id'] + '_' + inst_suffix(master, occ)
    inst['recurringEventId'] = master['id']
    if 'date' in master['start']:
        days = (date.fromisoformat(master['end']['date']) - date.fromisoformat(master['start']['date'])).days
        inst['start'] = {'date': occ.date().isoformat()}
        inst['end'] = {'date': (occ.date() + timedelta(days=days)).isoformat()}
        inst['originalStartTime'] = {'date': occ.date().isoformat()}
    else:
        tzname = master['start'].get('timeZone')
        inst['start'] = {'dateTime': occ.isoformat(), **({'timeZone': tzname} if tzname else {})}
        etz = master['end'].get('timeZone')
        end = (occ + (e0 - s0)).astimezone(ZoneInfo(etz) if etz else occ.tzinfo)
        inst['end'] = {'dateTime': end.isoformat(), **({'timeZone': etz} if etz else {})}
        inst['originalStartTime'] = dict(inst['start'])
    return inst


def resolve_instance(cal, eid):
    """'<master>_<stamp>' -> (master, occurrence) or None."""
    m = re.fullmatch(r'(.+)_(\d{8})(T\d{6}Z)?', eid)
    master = m and cal['events'].get(m[1])
    if not master or not master.get('recurrence') or master['status'] == 'cancelled':
        return None
    caltz = cal['meta']['timeZone']
    if m[3]:
        target = datetime.strptime(m[2] + m[3], '%Y%m%dT%H%M%SZ').replace(tzinfo=UTC)
    else:
        target = datetime.combine(datetime.strptime(m[2], '%Y%m%d').date(), dtime(), ZoneInfo(caltz))
    for occ in occurrences(master, caltz, target - timedelta(days=1), target + timedelta(days=2)):
        if inst_suffix(master, occ) == eid[len(master['id']) + 1:]:
            return master, occ
    return None

# ----------------------------------------------------------------------------- Google Calendar: handlers


def g_cal(cid, write=False):
    cals = S['google']['calendars']
    if cid == 'primary':
        cid = S['google']['user']['email']
    cal = cals.get(cid)
    if not cal:
        raise gerr(404, 'Not Found', 'notFound')
    if write and cal['meta']['accessRole'] not in ('owner', 'writer'):
        raise gerr(403, 'You need to have writer access to this calendar.', 'requiredAccessLevel')
    return cal


def g_etag(seq):
    return '"%d"' % (3500000000000000 + seq * 1000)


def g_cal_entry(cal):
    m = cal['meta']
    out = {'kind': 'calendar#calendarListEntry', 'etag': g_etag(m.get('_seq', 0))}
    out.update(public(m))
    if not out.get('primary'):
        out.pop('primary', None)
    return out


def g_calendar_list(req):
    q = req.q
    since = sync_seq(q['syncToken']) if q.get('syncToken') else None
    roles = ['freeBusyReader', 'reader', 'writer', 'owner']
    minrole = roles.index(q.get('minAccessRole', 'freeBusyReader'))
    items = [g_cal_entry(c) for c in S['google']['calendars'].values()
             if roles.index(c['meta']['accessRole']) >= minrole and (since is None or c['meta']['_seq'] > since)]
    page, nxt, seq0 = page_slice(items, q, 100, 250)
    out = {'kind': 'calendar#calendarList', 'etag': g_etag(S['seq']), 'items': page}
    out.update({'nextPageToken': nxt} if nxt else {'nextSyncToken': sync_token(seq0)})
    return out


def g_calendar_list_get(req, cid):
    return g_cal_entry(g_cal(cid))


def g_calendar_get(req, cid):
    m = g_cal(cid)['meta']
    return {'kind': 'calendar#calendar', 'etag': g_etag(m['_seq']), 'id': m['id'], 'summary': m['summary'],
            'timeZone': m['timeZone'], 'conferenceProperties': {'allowedConferenceSolutionTypes': ['hangoutsMeet']}}


def ev_out(ev):
    if ev.get('status') == 'cancelled':  # Google only guarantees these for deleted events / cancelled exceptions
        keep = ('kind', 'etag', 'id', 'status', 'updated', 'recurringEventId', 'originalStartTime')
        return {k: ev[k] for k in keep if k in ev}
    return public(ev)


def g_overlaps(s, e, tmin, tmax):
    return (tmin is None or e > tmin or (s == e and s >= tmin)) and (tmax is None or s < tmax)


def g_time_param(q, name):
    if name not in q:
        return None
    d = parse_dt(q[name])
    if d is None or not re.search(r'(Z|[+-]\d\d:?\d\d)$', q[name].strip().replace(' ', '+')):
        raise gerr(400, 'Bad Request', 'badRequest')
    return d


def g_collect(cal, q, since=None, only_master=None):
    caltz = cal['meta']['timeZone']
    single = q.get('singleEvents') == 'true' or only_master is not None
    show_del = q.get('showDeleted') == 'true' or since is not None
    tmin, tmax = g_time_param(q, 'timeMin'), g_time_param(q, 'timeMax')
    upd_min = g_time_param(q, 'updatedMin')
    evs = list(cal['events'].values())
    exc_ids = {e['id'] for e in evs if e.get('recurringEventId')}
    out = []

    def changed(e):
        return (since is None or e['_seq'] > since) and (upd_min is None or parse_dt(e['updated']) >= upd_min)

    def overlap(e):
        return g_overlaps(*ev_bounds(e, caltz), tmin, tmax)

    for e in evs:
        if only_master is not None and only_master not in (e['id'], e.get('recurringEventId')):
            continue
        cancelled = e['status'] == 'cancelled'
        if e.get('recurringEventId'):  # exception (modified or cancelled instance)
            if changed(e) and (not cancelled or show_del or not single) and overlap(e):
                out.append(e)
        elif e.get('recurrence'):
            if cancelled:
                if show_del and changed(e):
                    out.append(e)
            elif not single:
                if changed(e) and (tmin is None and tmax is None or next(occurrences(e, caltz, tmin, tmax), None)):
                    out.append(e)
            elif changed(e):
                for occ in occurrences(e, caltz, tmin, tmax):
                    inst = make_instance(e, occ, caltz)
                    if inst['id'] not in exc_ids:
                        out.append(inst)
        elif only_master is None and (show_del or not cancelled) and changed(e) and overlap(e):
            out.append(e)
    if q.get('q'):
        needle = q['q'].lower()
        out = [e for e in out if needle in ' '.join(str(e.get(k, '')) for k in
                                                     ('summary', 'description', 'location')).lower()]
    if q.get('iCalUID'):
        out = [e for e in out if e.get('iCalUID') == q['iCalUID']]
    if q.get('orderBy') == 'updated':
        out.sort(key=lambda e: e['updated'])
    else:
        out.sort(key=lambda e: (ev_bounds(e, caltz)[0] if e.get('start') else utcnow(), e['id']))
    return [ev_out(e) for e in out]


def g_events_response(cal, items, q):
    page, nxt, seq0 = page_slice(items, q, 250, 2500)
    m = cal['meta']
    out = {'kind': 'calendar#events', 'etag': g_etag(S['seq']), 'summary': m['summary'], 'description': '',
           'updated': ts_ms(), 'timeZone': m['timeZone'], 'accessRole': m['accessRole'],
           'defaultReminders': m.get('defaultReminders', []), 'items': page}
    out.update({'nextPageToken': nxt} if nxt else {'nextSyncToken': sync_token(seq0)})
    return out


def g_events_list(req, cid):
    cal, q = g_cal(cid), req.q
    if q.get('orderBy') == 'startTime' and q.get('singleEvents') != 'true':
        raise gerr(400, 'The requested ordering is not available for the particular query.', 'invalid')
    since = None
    if q.get('syncToken'):
        bad = [k for k in ('timeMin', 'timeMax', 'orderBy', 'q', 'updatedMin', 'iCalUID') if k in q]
        if bad:
            raise gerr(400, 'Sync token cannot be used with ' + ', '.join(bad) + '.', 'invalid')
        since = sync_seq(q['syncToken'])
    return g_events_response(cal, g_collect(cal, q, since), q)


def g_event_instances(req, cid, eid):
    cal = g_cal(cid)
    m = cal['events'].get(eid)
    if not m or not m.get('recurrence'):
        raise gerr(404, 'Not Found', 'notFound')
    return g_events_response(cal, g_collect(cal, req.q, only_master=eid), req.q)


READONLY_EV = ('kind', 'etag', 'id', 'htmlLink', 'created', 'updated', 'creator', 'iCalUID',
               'recurringEventId', 'originalStartTime', 'sequence')


def g_validate(ev):
    s, e = ev.get('start') or {}, ev.get('end') or {}
    if not s:
        raise gerr(400, 'Missing start time.', 'required')
    if not e:
        raise gerr(400, 'Missing end time.', 'required')
    if ('date' in s) != ('date' in e) or not (('date' in s) or ('dateTime' in s)):
        raise gerr(400, 'Start and end times must either both be date or both be dateTime.', 'invalid')
    if 'dateTime' in s:
        if parse_dt(s['dateTime']) is None or parse_dt(e['dateTime']) is None:
            raise gerr(400, 'Invalid start or end time.', 'invalid')
        if ev.get('recurrence') and not s.get('timeZone'):
            raise gerr(400, 'Missing time zone definition for start time.', 'invalid')
        if ev.get('recurrence') and not e.get('timeZone'):
            raise gerr(400, 'Missing time zone definition for end time.', 'invalid')
        for t in (s, e):
            if t.get('timeZone'):
                try:
                    ZoneInfo(t['timeZone'])
                except Exception:
                    raise gerr(400, 'Invalid time zone definition for start time.', 'invalid')
    try:
        s0, e0 = ev_bounds(ev, 'UTC')
    except (ValueError, TypeError):
        raise gerr(400, 'Invalid start or end time.', 'invalid')
    if e0 < s0:
        raise gerr(400, 'The specified time range is empty.', 'timeRangeEmpty')


def g_touch(ev, schedule_changed=False):
    bump(ev)
    ev['updated'] = ts_ms()
    ev['etag'] = g_etag(ev['_seq'])
    if schedule_changed:
        ev['sequence'] = ev.get('sequence', 0) + 1
    return ev


def g_new_event(cal, eid, body):
    cid, now = cal['meta']['id'], ts_ms()
    ev = {'kind': 'calendar#event', 'id': eid, 'status': 'confirmed',
          'htmlLink': 'https://www.google.com/calendar/event?eid=' + b64url(f'{eid} {cid}'.encode()),
          'created': now, 'updated': now, 'creator': {'email': S['google']['user']['email'], 'self': True},
          'organizer': {'email': cid, 'displayName': cal['meta']['summary'], 'self': True},
          'iCalUID': eid + '@google.com', 'sequence': 0, 'reminders': {'useDefault': True},
          'eventType': 'default'}
    if cal['meta'].get('primary'):
        ev['organizer'].pop('displayName')
    for k, v in body.items():
        if k not in READONLY_EV and v is not None:
            ev[k] = copy.deepcopy(v)
    ev['status'] = 'confirmed' if body.get('status') in (None, 'cancelled') else body['status']
    return ev


def g_event_insert(req, cid):
    cal = g_cal(cid, write=True)
    body = req.body
    eid = body.get('id') or uuid.uuid4().hex
    if not re.fullmatch(r'[a-v0-9]{5,1024}', eid):
        raise gerr(400, 'Invalid resource id value.', 'invalid')
    if eid in cal['events']:
        raise gerr(409, 'The requested identifier already exists.', 'duplicate')
    ev = g_new_event(cal, eid, body)
    g_validate(ev)
    cal['events'][eid] = g_touch(ev)
    return public(ev)


def g_check_match(req, etag):
    im = req.headers.get('If-Match')
    if im and im != '*' and im != etag:
        raise gerr(412, 'Precondition Failed', 'conditionNotMet')


def g_find(cal, eid):
    """-> (stored event or None, synthesized instance or None)."""
    ev = cal['events'].get(eid)
    if ev:
        return ev, None
    r = resolve_instance(cal, eid)
    if not r:
        raise gerr(404, 'Not Found', 'notFound')
    return None, make_instance(r[0], r[1], cal['meta']['timeZone'])


def g_event_get(req, cid, eid):
    ev, inst = g_find(g_cal(cid), eid)
    return public(ev or inst)


def g_event_update(req, cid, eid, replace=False):
    cal = g_cal(cid, write=True)
    ev, inst = g_find(cal, eid)
    cur = ev or inst
    g_check_match(req, cur['etag'])
    body = {k: v for k, v in req.body.items() if k not in READONLY_EV}
    before = (cur.get('start'), cur.get('end'), cur.get('recurrence'))
    new = copy.deepcopy(cur)
    if replace:
        new = {k: v for k, v in new.items() if k in READONLY_EV or k.startswith('_') or k == 'organizer'}
        new.update(copy.deepcopy({k: v for k, v in body.items() if v is not None}))
        new.setdefault('status', 'confirmed')
        new.setdefault('reminders', {'useDefault': True})
    else:
        deep_merge(new, body)
    if inst and 'recurrence' in new:
        raise gerr(400, 'Invalid recurrence for an instance.', 'invalid')
    g_validate(new)
    g_touch(new, before != (new.get('start'), new.get('end'), new.get('recurrence')))
    cal['events'][eid] = new
    return public(new)


def g_event_delete(req, cid, eid):
    cal = g_cal(cid, write=True)
    ev, inst = g_find(cal, eid)
    cur = ev or inst
    g_check_match(req, cur['etag'])
    if ev and ev['status'] == 'cancelled':
        raise gerr(410, 'Resource has been deleted', 'deleted')
    cur['status'] = 'cancelled'
    cal['events'][eid] = g_touch(cur)
    if ev and ev.get('recurrence'):
        for x in cal['events'].values():
            if x.get('recurringEventId') == eid and x['status'] != 'cancelled':
                x['status'] = 'cancelled'
                g_touch(x)
    return 204, None, {}

# ----------------------------------------------------------------------------- Google Tasks


def gt_list(tl):
    lists = S['gtasks']
    if tl == '@default':
        tl = next(iter(lists))
    if tl not in lists:
        raise gerr(404, 'Task list not found.', 'notFound')
    return lists[tl]


def gt_list_out(req, l):
    m = l['meta']
    return {'kind': 'tasks#taskList', 'id': m['id'], 'etag': g_etag(m['_seq']), 'title': m['title'],
            'updated': m['updated'], 'selfLink': f'http://{req.host}/google/tasks/v1/users/@me/lists/{m["id"]}'}


def gt_lists(req):
    items = [gt_list_out(req, l) for l in S['gtasks'].values()]
    page, nxt, _ = page_slice(items, req.q, 1000, 1000)
    out = {'kind': 'tasks#taskLists', 'etag': g_etag(S['seq']), 'items': page}
    if nxt:
        out['nextPageToken'] = nxt
    return out


def gt_list_insert(req):
    title = (req.body.get('title') or '').strip()
    if not title:
        raise gerr(400, 'Missing title.', 'required')
    tid = secrets.token_urlsafe(16)
    l = {'meta': bump({'id': tid, 'title': title, 'updated': ts_ms()}), 'tasks': {}}
    S['gtasks'][tid] = l
    return gt_list_out(req, l)


def gt_list_get(req, tl):
    return gt_list_out(req, gt_list(tl))


def gt_list_update(req, tl):
    l = gt_list(tl)
    if req.body.get('title'):
        l['meta']['title'] = req.body['title']
    l['meta']['updated'] = ts_ms()
    bump(l['meta'])
    return gt_list_out(req, l)


def gt_list_delete(req, tl):
    del S['gtasks'][gt_list(tl)['meta']['id']]
    return 204, None, {}


def gt_due(v):
    if v in (None, ''):
        return None
    d = parse_dt(v) if 'T' in str(v) else None
    try:
        day = d.date() if d else date.fromisoformat(str(v)[:10])
    except ValueError:
        raise gerr(400, 'Invalid value for: due', 'invalid')
    return day.isoformat() + 'T00:00:00.000Z'  # Google drops the time of day


def gt_out(req, l, t):
    out = {'kind': 'tasks#task', 'id': t['id'], 'etag': g_etag(t['_seq']), 'title': t.get('title', ''),
           'updated': t['updated'],
           'selfLink': f'http://{req.host}/google/tasks/v1/lists/{l["meta"]["id"]}/tasks/{t["id"]}'}
    for k in ('parent', 'position', 'notes', 'status', 'due', 'completed'):
        if t.get(k) is not None:
            out[k] = t[k]
    for k in ('deleted', 'hidden'):
        if t.get(k):
            out[k] = True
    out['links'] = []
    out['webViewLink'] = 'https://tasks.google.com/task/' + t['id']
    return out


def gt_renumber(l, parent):
    sib = sorted((t for t in l['tasks'].values() if t.get('parent') == parent and not t.get('deleted')),
                 key=lambda t: t['_ord'])
    for i, t in enumerate(sib):
        t['_ord'], t['position'] = i, '%020d' % i


def gt_place(l, t, parent, previous):
    """Put t under parent right after previous (or first among siblings)."""
    if parent and parent not in l['tasks']:
        raise gerr(400, 'Invalid parent.', 'invalid')
    prev = l['tasks'].get(previous) if previous else None
    if previous and (not prev or prev.get('parent') != parent):
        raise gerr(400, 'Invalid previous.', 'invalid')
    old_parent = t.get('parent')
    t['parent'] = parent or None
    t['_ord'] = prev['_ord'] + 0.5 if prev else -0.5
    gt_renumber(l, t['parent'])
    if old_parent != t['parent']:
        gt_renumber(l, old_parent)


def gt_apply(t, body, replace=False):
    if replace:
        for k in ('notes', 'due', 'completed'):
            t.pop(k, None)
    for k in ('title', 'notes', 'deleted', 'hidden'):
        if k in body:
            t[k] = body[k]
    if 'due' in body:
        t['due'] = gt_due(body['due'])
    if 'completed' in body and body['completed']:
        if parse_dt(body['completed']) is None:
            raise gerr(400, 'Invalid value for: completed', 'invalid')
        t['completed'] = ts_ms(parse_dt(body['completed']))
    st = body.get('status', t.get('status') if not replace else 'needsAction') or 'needsAction'
    if st not in ('needsAction', 'completed'):
        raise gerr(400, 'Invalid value for: status', 'invalid')
    t['status'] = st
    if st == 'completed':
        t.setdefault('completed', ts_ms())
        t['completed'] = t['completed'] or ts_ms()
    else:
        t.pop('completed', None)
        t['hidden'] = False
    t['updated'] = ts_ms()
    bump(t)


def gt_task(l, tid):
    t = l['tasks'].get(tid)
    if not t:
        raise gerr(404, 'Task not found.', 'notFound')
    return t


def gt_tasks(req, tl):
    l, q = gt_list(tl), req.q
    show_completed = q.get('showCompleted', 'true') == 'true'
    show_hidden, show_deleted = q.get('showHidden') == 'true', q.get('showDeleted') == 'true'
    lim = {k: g_time_param(q, k) for k in ('updatedMin', 'dueMin', 'dueMax', 'completedMin', 'completedMax')}

    def keep(t):
        if t.get('deleted') and not show_deleted:
            return False
        if t.get('hidden') and not show_hidden:
            return False
        if t['status'] == 'completed' and not show_completed:
            return False
        for k, field, ge in (('updatedMin', 'updated', True), ('dueMin', 'due', True), ('dueMax', 'due', False),
                             ('completedMin', 'completed', True), ('completedMax', 'completed', False)):
            if lim[k] is not None:
                v = parse_dt(t[field]) if t.get(field) else None
                if v is None or (v < lim[k] if ge else v > lim[k]):
                    return False
        return True

    ordered = []

    def walk(parent):
        for t in sorted((t for t in l['tasks'].values() if t.get('parent') == parent), key=lambda t: t['_ord']):
            ordered.append(t)
            walk(t['id'])
    walk(None)
    items = [gt_out(req, l, t) for t in ordered if keep(t)]
    page, nxt, _ = page_slice(items, q, 20, 100)
    out = {'kind': 'tasks#tasks', 'etag': g_etag(S['seq']), 'items': page}
    if nxt:
        out['nextPageToken'] = nxt
    return out


def gt_task_insert(req, tl):
    l = gt_list(tl)
    t = {'id': secrets.token_urlsafe(16), 'status': 'needsAction'}
    gt_apply(t, {k: v for k, v in req.body.items() if k not in ('id', 'parent', 'position')})
    l['tasks'][t['id']] = t
    gt_place(l, t, req.q.get('parent'), req.q.get('previous'))
    return gt_out(req, l, t)


def gt_task_get(req, tl, tid):
    l = gt_list(tl)
    return gt_out(req, l, gt_task(l, tid))


def gt_task_update(req, tl, tid, replace=False):
    l = gt_list(tl)
    t = gt_task(l, tid)
    g_check_match(req, g_etag(t['_seq']))
    gt_apply(t, req.body, replace)
    return gt_out(req, l, t)


def gt_task_delete(req, tl, tid):
    l = gt_list(tl)
    todo = [gt_task(l, tid)]
    while todo:
        t = todo.pop()
        t['deleted'] = True
        t['updated'] = ts_ms()
        bump(t)
        todo += [c for c in l['tasks'].values() if c.get('parent') == t['id'] and not c.get('deleted')]
    return 204, None, {}


def gt_task_move(req, tl, tid):
    l = gt_list(tl)
    t = gt_task(l, tid)
    dest = req.q.get('destinationTasklist')
    if dest and gt_list(dest) is not l:
        d = gt_list(dest)
        moving = [t] + [c for c in l['tasks'].values() if c.get('parent') == tid]
        for x in moving:
            d['tasks'][x['id']] = l['tasks'].pop(x['id'])
        old = t.get('parent')
        t['parent'] = None
        gt_renumber(l, old)
        l = d
    gt_place(l, t, req.q.get('parent'), req.q.get('previous'))
    t['updated'] = ts_ms()
    bump(t)
    return gt_out(req, l, t)


def gt_clear(req, tl):
    for t in gt_list(tl)['tasks'].values():
        if t['status'] == 'completed' and not t.get('hidden') and not t.get('deleted'):
            t['hidden'] = True
            t['updated'] = ts_ms()
            bump(t)
    return 204, None, {}

# ----------------------------------------------------------------------------- Todoist: dates


WDN = ['monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday', 'sunday']
TD_COLORS = ['berry_red', 'red', 'orange', 'yellow', 'olive_green', 'lime_green', 'green', 'mint_green', 'teal',
             'sky_blue', 'light_blue', 'blue', 'grape', 'violet', 'lavender', 'magenta', 'salmon', 'charcoal',
             'grey', 'taupe']


def wd_of(word):
    for i, n in enumerate(WDN):
        if word in (n, n[:3]):
            return i
    return None


def add_months(d, n):
    y, m = d.year + (d.month - 1 + n) // 12, (d.month - 1 + n) % 12 + 1
    for day in range(d.day, 27, -1):
        try:
            return d.replace(year=y, month=m, day=day)
        except ValueError:
            pass
    return d.replace(year=y, month=m, day=min(d.day, 28))


def parse_recur(spec):
    spec = {'day': 'day', 'daily': 'day', 'week': 'week', 'weekly': 'week', 'month': 'month',
            'monthly': 'month', 'year': 'year', 'yearly': 'year'}.get(spec, spec)
    m = re.fullmatch(r'(?:(\d+|other)\s+)?(day|week|month|year)s?', spec)
    if m:
        return [m[2], 2 if m[1] == 'other' else int(m[1] or 1)]
    if spec in ('weekday', 'workday'):
        return ['wd', [0, 1, 2, 3, 4]]
    days = [wd_of(w) for w in re.split(r'\s*(?:,|\band\b)\s*|\s+', spec) if w]
    if days and None not in days:
        return ['wd', sorted(set(days))]
    raise ValueError(spec)


def recur_first(rule, start):
    if rule[0] == 'wd':
        while start.weekday() not in rule[1]:
            start += timedelta(days=1)
    return start


def recur_next(rule, d):
    kind, n = rule
    if kind == 'day':
        return d + timedelta(days=n)
    if kind == 'week':
        return d + timedelta(weeks=n)
    if kind == 'month':
        return add_months(d, n)
    if kind == 'year':
        return add_months(d, 12 * n)
    d += timedelta(days=1)
    return recur_first(rule, d)


def parse_due_string(s):
    """Tiny subset of Todoist's natural language -> (date|None, time|None, rule|None)."""
    text = re.sub(r'\s+', ' ', s.strip().lower())
    if text in ('', 'no date', 'no due date'):
        return None, None, None
    today = start = local_today()
    m = re.fullmatch(r'(.*?) (?:starting|from) (?:on )?(\d{4}-\d{2}-\d{2})', text)
    if m:
        text, start = m[1], date.fromisoformat(m[2])
    tm = None
    m = (re.fullmatch(r'(.*?) ?(?:\bat )?\b(\d{1,2})(?::(\d{2}))? ?(am|pm)', text)
         or re.fullmatch(r'(.*?) at (\d{1,2})(?::(\d{2}))?()', text))
    if m:
        h = int(m[2]) % 12 + (12 if m[4] == 'pm' else 0) if m[4] else int(m[2])
        tm, text = dtime(h, int(m[3] or 0)), m[1].strip()
    if text.startswith('every') or text in ('daily', 'weekly', 'monthly', 'yearly'):
        rule = parse_recur(text[5:].lstrip('!').strip() if text.startswith('every') else text)
        return recur_first(rule, start), tm, rule
    if text in ('today', 'tod', ''):
        return today, tm, None
    if text in ('tomorrow', 'tom'):
        return today + timedelta(days=1), tm, None
    if text == 'yesterday':
        return today - timedelta(days=1), tm, None
    if text == 'next week':
        return today + timedelta(days=7 - today.weekday()), tm, None
    m = re.fullmatch(r'in (\d+) days?', text)
    if m:
        return today + timedelta(days=int(m[1])), tm, None
    m = re.fullmatch(r'(?:next )?(\w+)', text)
    if m and wd_of(m[1]) is not None:
        return today + timedelta(days=(wd_of(m[1]) - today.weekday() - 1) % 7 + 1), tm, None
    if re.fullmatch(r'\d{4}-\d{2}-\d{2}', text):
        return date.fromisoformat(text), tm, None
    raise ValueError(s)


def human_date(d, tm=None):
    out = f'{d:%b} {d.day}' + (f' {d.year}' if d.year != local_today().year else '')
    if tm:
        out += f' {(tm.hour - 1) % 12 + 1}:{tm.minute:02d} {"PM" if tm.hour >= 12 else "AM"}'
    return out


def td_due(body):
    """Build a v1 due object from due_string / due_date / due_datetime. Returns (present, due)."""
    if body.get('due_datetime'):
        d = parse_dt(body['due_datetime'])
        if d is None:
            raise terr(400, 'Invalid date format', 'INVALID_DATE_FORMAT', 52, {'argument': 'due_datetime'})
        d = d.astimezone(UTC)
        return True, {'date': d.strftime('%Y-%m-%dT%H:%M:%SZ'), 'timezone': 'UTC', 'lang': 'en',
                      'string': human_date(d.date(), d.time()), 'is_recurring': False}
    if body.get('due_date'):
        try:
            d = date.fromisoformat(body['due_date'])
        except ValueError:
            raise terr(400, 'Invalid date format', 'INVALID_DATE_FORMAT', 52, {'argument': 'due_date'})
        return True, {'date': d.isoformat(), 'timezone': None, 'string': human_date(d), 'lang': 'en',
                      'is_recurring': False}
    if 'due_string' in body:
        try:
            d, tm, rule = parse_due_string(body['due_string'] or '')
        except ValueError:
            raise terr(400, 'Invalid date format', 'INVALID_DATE_FORMAT', 52, {'argument': 'due_string'})
        if d is None:
            return True, None
        return True, {'date': d.isoformat() + (f'T{tm:%H:%M:%S}' if tm else ''), 'timezone': None,
                      'string': body['due_string'], 'lang': body.get('due_lang', 'en'), 'is_recurring': bool(rule)}
    if 'due' in body and body['due'] is None:
        return True, None
    return False, None


def td_advance(due):
    """Move a recurring due to its next occurrence; False if the rule is not understood."""
    text = due['string'].lower()
    try:
        _, _, rule = parse_due_string(due['string'])
    except ValueError:
        return False
    if not rule:
        return False
    day, rest = due['date'][:10], due['date'][10:]
    base = local_today() if 'every!' in text else date.fromisoformat(day)
    nxt = recur_next(rule, base)
    while 'every!' not in text and nxt < local_today():
        nxt = recur_next(rule, nxt)  # Todoist skips occurrences that are already in the past
    due['date'] = nxt.isoformat() + rest
    return True

# ----------------------------------------------------------------------------- Todoist: API


def terr(code, msg, tag, ecode=0, extra=None):
    return HTTPError(code, {'error': msg, 'error_code': ecode, 'error_extra': extra or {},
                            'error_tag': tag, 'http_code': code})


def td_require_auth(req):
    if not bearer_ok(req, 'todoist'):
        raise terr(401, 'Invalid token', 'AUTH_INVALID_TOKEN', 401, {'retry_after': 0})


def td_page(items, q):
    try:
        lim = int(q.get('limit', 50))
    except ValueError:
        lim = 0
    if not 1 <= lim <= 200:
        raise terr(400, 'Invalid argument value', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'limit'})
    off = 0
    if q.get('cursor'):
        c = unb64json(q['cursor'])
        if not isinstance(c, dict):
            raise terr(400, 'Invalid argument value', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'cursor'})
        off = c['o']
    nxt = b64json({'o': off + lim}) if off + lim < len(items) else None
    return items[off:off + lim], nxt


def td_obj(kind, oid, missing_ok=False):
    o = S['todoist'][kind].get(oid)
    if not o or o.get('is_deleted'):
        if missing_ok:
            return None
        raise terr(404, kind[:-1].title() + ' not found', 'NOT_FOUND', 404)
    return o


def td_touch(o):
    o['updated_at'] = ts_us()
    return bump(o)


def td_user(req):
    u = copy.deepcopy(public(S['todoist']['user']))
    now = datetime.now(ZoneInfo(u['tz_info']['timezone']))
    off = now.utcoffset()
    mins = int(off.total_seconds() // 60)
    u['tz_info'].update({'gmt_string': '%s%02d:%02d' % ('-' if mins < 0 else '+', abs(mins) // 60, abs(mins) % 60),
                         'hours': mins // 60, 'minutes': abs(mins) % 60, 'is_dst': int(bool(now.dst()))})
    return u


def td_list(kind):
    def handler(req):
        q = req.q
        objs = [o for o in S['todoist'][kind].values() if not o.get('is_deleted') and not o.get('is_archived')]
        if kind != 'labels' and q.get('project_id'):
            objs = [o for o in objs if o.get('project_id') == q['project_id']]
        order = {'projects': 'child_order', 'sections': 'section_order', 'labels': 'order'}[kind]
        objs.sort(key=lambda o: (o.get('inbox_project') is not True, o.get(order, 0)))
        page, nxt = td_page([public(o) for o in objs], q)
        return {'results': page, 'next_cursor': nxt}
    return handler


def td_get(kind):
    return lambda req, oid: public(td_obj(kind, oid))


def td_color(v):
    if v not in TD_COLORS:
        raise terr(400, 'Invalid argument value', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'color'})
    return v


def td_project_create(req, oid=None):
    b = req.body
    if oid:
        p = td_obj('projects', oid)
    else:
        if not b.get('name'):
            raise terr(400, 'Required argument is missing', 'ARGUMENT_MISSING', 19, {'argument': 'name'})
        if b.get('parent_id'):
            td_obj('projects', b['parent_id'])
        sib = [x for x in S['todoist']['projects'].values() if x.get('parent_id') == b.get('parent_id')]
        p = new_project(new_tid(), b['name'], b.get('color', 'charcoal'), b.get('parent_id'), len(sib) + 1)
    for k in ('name', 'description', 'is_favorite', 'view_style', 'is_collapsed'):
        if k in b:
            p[k] = b[k]
    if 'color' in b:
        p['color'] = td_color(b['color'])
    S['todoist']['projects'][p['id']] = td_touch(p)
    return public(p)


def td_project_delete(req, oid):
    p = td_obj('projects', oid)
    if p.get('inbox_project'):
        raise terr(400, 'Inbox project cannot be deleted', 'INVALID_ARGUMENT_VALUE', 20)
    p['is_deleted'] = True
    td_touch(p)
    for t in S['todoist']['tasks'].values():
        if t['project_id'] == oid and not t['is_deleted']:
            t['is_deleted'] = True
            td_touch(t)
    return 204, None, {}


def td_section_create(req):
    b = req.body
    if not b.get('name') or not b.get('project_id'):
        raise terr(400, 'Required argument is missing', 'ARGUMENT_MISSING', 19,
                   {'argument': 'name' if not b.get('name') else 'project_id'})
    td_obj('projects', b['project_id'])
    s = new_section(new_tid(), b['project_id'], b['name'], len(S['todoist']['sections']) + 1)
    S['todoist']['sections'][s['id']] = td_touch(s)
    return public(s)


def td_label_create(req):
    b = req.body
    if not b.get('name'):
        raise terr(400, 'Required argument is missing', 'ARGUMENT_MISSING', 19, {'argument': 'name'})
    lab = new_label(new_tid(), b['name'], td_color(b.get('color', 'charcoal')), len(S['todoist']['labels']) + 1)
    S['todoist']['labels'][lab['id']] = td_touch(lab)
    return public(lab)


def td_tasks(req):
    q = req.q
    ts = [t for t in S['todoist']['tasks'].values() if not t['checked'] and not t['is_deleted']]
    for k in ('project_id', 'section_id', 'parent_id'):
        if q.get(k):
            ts = [t for t in ts if t.get(k) == q[k]]
    if q.get('label'):
        ts = [t for t in ts if q['label'] in t['labels']]
    if q.get('ids'):
        ids = set(q['ids'].split(','))
        ts = [t for t in ts if t['id'] in ids]
    porder = {p['id']: p['child_order'] for p in S['todoist']['projects'].values()}
    ts.sort(key=lambda t: (porder.get(t['project_id'], 0), t['child_order'], t['added_at']))
    page, nxt = td_page([public(t) for t in ts], q)
    return {'results': page, 'next_cursor': nxt}


def td_apply(t, b):
    if 'content' in b:
        if not str(b['content']).strip():
            raise terr(400, 'Required argument is missing', 'ARGUMENT_MISSING', 19, {'argument': 'content'})
        t['content'] = b['content']
    for k in ('description', 'is_collapsed', 'day_order', 'child_order'):
        if k in b:
            t[k] = b[k]
    if 'order' in b:
        t['child_order'] = b['order']
    if 'labels' in b:
        if not isinstance(b['labels'], list):
            raise terr(400, 'Invalid argument value', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'labels'})
        t['labels'] = list(b['labels'])
    if 'priority' in b:
        if b['priority'] not in (1, 2, 3, 4):
            raise terr(400, 'Invalid argument value', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'priority'})
        t['priority'] = b['priority']
    present, due = td_due(b)
    if present:
        t['due'] = due
    if 'duration' in b or 'duration_unit' in b:
        if b.get('duration') is None:
            t['duration'] = None
        elif b.get('duration_unit') not in ('minute', 'day') or not isinstance(b['duration'], int) or b['duration'] < 1:
            raise terr(400, 'Invalid argument value', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'duration'})
        else:
            t['duration'] = {'amount': b['duration'], 'unit': b['duration_unit']}
    if 'deadline_date' in b:
        t['deadline'] = {'date': b['deadline_date'], 'lang': b.get('deadline_lang', 'en')} if b['deadline_date'] else None
    if 'assignee_id' in b:
        t['responsible_uid'] = b['assignee_id']


def td_placement(b, t=None):
    """Resolve project/section/parent consistently -> (project_id, section_id, parent_id)."""
    td = S['todoist']
    if b.get('parent_id'):
        par = td_obj('tasks', b['parent_id'])
        return par['project_id'], par.get('section_id'), par['id']
    if b.get('section_id'):
        sec = td_obj('sections', b['section_id'])
        return sec['project_id'], sec['id'], None
    pid = b.get('project_id') or (t and t['project_id']) or td['user']['inbox_project_id']
    td_obj('projects', pid)
    return pid, None, None


def td_task_create(req):
    b = req.body
    if not str(b.get('content') or '').strip():
        raise terr(400, 'Required argument is missing', 'ARGUMENT_MISSING', 19, {'argument': 'content'})
    pid, sid, par = td_placement(b)
    t = new_task(new_tid(), b['content'], pid)
    t.update(section_id=sid, parent_id=par)
    t['child_order'] = 1 + max([x['child_order'] for x in S['todoist']['tasks'].values()
                                if x['project_id'] == pid] or [0])
    td_apply(t, b)
    S['todoist']['tasks'][t['id']] = td_touch(t)
    return public(t)


def td_task_get(req, tid):
    return public(td_obj('tasks', tid))


def td_task_update(req, tid):
    t = td_obj('tasks', tid)
    td_apply(t, req.body)
    return public(td_touch(t))


def td_descendants(tid):
    out, todo = [], [tid]
    while todo:
        cur = todo.pop()
        kids = [t for t in S['todoist']['tasks'].values() if t.get('parent_id') == cur and not t['is_deleted']]
        out += kids
        todo += [k['id'] for k in kids]
    return out


def td_task_delete(req, tid):
    for t in [td_obj('tasks', tid)] + td_descendants(tid):
        t['is_deleted'] = True
        td_touch(t)
    return 204, None, {}


def td_task_close(req, tid):
    t = td_obj('tasks', tid)
    if t.get('due') and t['due'].get('is_recurring') and td_advance(t['due']):
        td_touch(t)
        return 204, None, {}
    now = ts_us()
    for x in [t] + td_descendants(tid):
        if not x['checked']:
            x.update(checked=True, completed_at=now, completed_by_uid=S['todoist']['user']['id'])
            td_touch(x)
    return 204, None, {}


def td_task_reopen(req, tid):
    t = td_obj('tasks', tid)
    t.update(checked=False, completed_at=None, completed_by_uid=None)
    td_touch(t)
    return 204, None, {}


def td_task_move(req, tid):
    t, b = td_obj('tasks', tid), req.body
    given = [k for k in ('project_id', 'section_id', 'parent_id') if b.get(k)]
    if len(given) != 1:
        raise terr(400, 'Exactly one of project_id, section_id or parent_id is required', 'INVALID_ARGUMENT_VALUE', 20)
    if b.get('parent_id') and b['parent_id'] in [tid] + [d['id'] for d in td_descendants(tid)]:
        raise terr(400, 'Invalid parent', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'parent_id'})
    pid, sid, par = td_placement(b, t)
    t.update(project_id=pid, section_id=sid, parent_id=par)
    for d in td_descendants(tid):
        d.update(project_id=pid, section_id=sid)
        td_touch(d)
    return public(td_touch(t))


def td_completed(req):
    q = req.q
    since, until = parse_dt(q.get('since', '')), parse_dt(q.get('until', ''))
    if not since or not until:
        raise terr(400, 'Required argument is missing', 'ARGUMENT_MISSING', 19,
                   {'argument': 'since' if not since else 'until'})
    if until - since > timedelta(weeks=12) or until < since:
        raise terr(400, 'Invalid date range (max 3 months)', 'INVALID_ARGUMENT_VALUE', 20, {'argument': 'until'})
    ts = [t for t in S['todoist']['tasks'].values() if t['checked'] and not t['is_deleted']
          and since <= parse_dt(t['completed_at']) <= until
          and (not q.get('project_id') or t['project_id'] == q['project_id'])]
    ts.sort(key=lambda t: t['completed_at'], reverse=True)
    page, nxt = td_page([public(t) for t in ts], q)
    return {'items': page, 'next_cursor': nxt}


def td_sync(req):
    """Read-only subset of POST /api/v1/sync (no commands)."""
    b = req.body
    if b.get('commands') not in (None, '', '[]', []):
        raise terr(400, 'commands are not supported by fake-cloud', 'INVALID_ARGUMENT_VALUE', 20)
    rt = b.get('resource_types', '["all"]')
    rt = json.loads(rt) if isinstance(rt, str) else rt
    tok = b.get('sync_token', '*')
    full = tok == '*'
    since = 0
    if not full:
        t = unb64json(tok)
        if not isinstance(t, dict) or t.get('g') != S['gen']:
            raise terr(400, 'Invalid sync token', 'INVALID_SYNC_TOKEN', 35)
        since = t['s']
    out = {'sync_token': b64json({'g': S['gen'], 's': S['seq']}), 'full_sync': full, 'temp_id_mapping': {},
           'sync_status': {}}
    if full:
        out['full_sync_date_utc'] = ts_us()
    for name, kind in (('items', 'tasks'), ('projects', 'projects'), ('sections', 'sections'), ('labels', 'labels')):
        if 'all' in rt or name in rt:
            objs = S['todoist'][kind].values()
            if full:
                objs = [o for o in objs if not o.get('is_deleted') and not o.get('checked')]
            else:
                objs = [o for o in objs if o['_seq'] > since]
            out[name] = [public(o) for o in objs]
    if 'all' in rt or 'user' in rt:
        out['user'] = td_user(req)
    return out


def td_register(req):
    b = req.body
    uris = b.get('redirect_uris')
    if not isinstance(uris, list) or not uris or not all(isinstance(u, str) and u for u in uris):
        raise HTTPError(400, {'error': 'invalid_redirect_uri', 'error_description': 'redirect_uris is required'})
    method = b.get('token_endpoint_auth_method', 'client_secret_post')
    if method not in ('none', 'client_secret_post', 'client_secret_basic'):
        raise HTTPError(400, {'error': 'invalid_client_metadata',
                              'error_description': 'unsupported token_endpoint_auth_method'})
    c = {'client_id': 'tdd_' + secrets.token_hex(12), 'client_id_issued_at': int(time.time()),
         'client_name': b.get('client_name', ''), 'redirect_uris': uris,
         'grant_types': b.get('grant_types', ['authorization_code', 'refresh_token']),
         'response_types': b.get('response_types', ['code']), 'token_endpoint_auth_method': method,
         'scope': b.get('scope', 'data:read_write,data:delete,project:delete')}
    if method != 'none':
        c.update(client_secret=secrets.token_hex(20), client_secret_expires_at=0)
    S['todoist']['clients'][c['client_id']] = c
    return 201, c, {}

# ----------------------------------------------------------------------------- seed data


def new_project(pid, name, color, parent_id=None, order=1, inbox=False):
    now = ts_us()
    return {'id': pid, 'name': name, 'color': color, 'parent_id': parent_id, 'child_order': order,
            'is_collapsed': False, 'is_shared': False, 'is_favorite': False, 'is_archived': False,
            'is_deleted': False, 'is_frozen': False, 'inbox_project': inbox, 'view_style': 'list',
            'description': '', 'can_assign_tasks': False, 'default_order': 0, 'creator_uid': S['todoist']['user']['id'],
            'created_at': now, 'updated_at': now, 'access': {'visibility': 'restricted'}, 'role': 'CREATOR'}


def new_section(sid, pid, name, order):
    now = ts_us()
    return {'id': sid, 'user_id': S['todoist']['user']['id'], 'project_id': pid, 'name': name, 'section_order': order,
            'is_collapsed': False, 'is_archived': False, 'is_deleted': False, 'added_at': now, 'updated_at': now,
            'archived_at': None}


def new_label(lid, name, color, order):
    return {'id': lid, 'name': name, 'color': color, 'order': order, 'is_favorite': False}


def new_task(tid, content, pid):
    now, uid = ts_us(), S['todoist']['user']['id']
    return {'id': tid, 'user_id': uid, 'project_id': pid, 'section_id': None, 'parent_id': None,
            'added_by_uid': uid, 'assigned_by_uid': None, 'responsible_uid': None, 'completed_by_uid': None,
            'content': content, 'description': '', 'priority': 1, 'labels': [], 'due': None, 'deadline': None,
            'duration': None, 'checked': False, 'is_deleted': False, 'is_collapsed': False, 'added_at': now,
            'completed_at': None, 'updated_at': now, 'child_order': 1, 'day_order': -1, 'note_count': 0}


def seed(keep_auth=None):
    """(Re)build the seed world relative to the current week in --tz."""
    global S
    gen = S.get('gen', 0) + 1
    S = {'gen': gen, 'seq': 0, 'auth': keep_auth or {'codes': {}, 'tokens': {}, 'refresh': {}},
         'google': {'user': {'sub': '110000000000000000001', 'email': 'tester@example.com', 'email_verified': True,
                             'name': 'Test User', 'given_name': 'Test', 'family_name': 'User', 'locale': 'en',
                             'picture': 'https://lh3.googleusercontent.com/a/fake-avatar'}, 'calendars': {}},
         'gtasks': {}, 'todoist': {'clients': {}, 'projects': {}, 'sections': {}, 'labels': {}, 'tasks': {}}}
    tzname = OPTS.tz
    tz = ZoneInfo(tzname)
    today = local_today()
    mon = today - timedelta(days=today.weekday())
    email = S['google']['user']['email']

    # -- Google calendars
    def cal(cid, summary, bg, fg, role, color_id, reminders, primary=False):
        meta = bump({'id': cid, 'summary': summary, 'timeZone': tzname, 'colorId': color_id, 'backgroundColor': bg,
                     'foregroundColor': fg, 'selected': True, 'accessRole': role, 'defaultReminders': reminders,
                     'primary': primary, 'conferenceProperties': {'allowedConferenceSolutionTypes': ['hangoutsMeet']}})
        if role == 'owner':
            meta['notificationSettings'] = {'notifications': [{'type': 'eventCreation', 'method': 'email'}]}
        S['google']['calendars'][cid] = c = {'meta': meta, 'events': {}}
        return c

    P = cal(email, email, '#9fe1e7', '#000000', 'owner', '14', [{'method': 'popup', 'minutes': 10}], primary=True)
    W = cal('c_8a1f0work4aircal@group.calendar.google.com', 'Work', '#7bd148', '#000000', 'owner', '8',
            [{'method': 'popup', 'minutes': 15}])
    H = cal('en.australian#holiday@group.v.calendar.google.com', 'Holidays in Australia', '#16a765', '#000000',
            'reader', '7', [])

    def T(day, h, m=0, zone=None):
        return {'dateTime': datetime.combine(mon + timedelta(days=day), dtime(h, m), ZoneInfo(zone or tzname))
                .isoformat(), 'timeZone': zone or tzname}

    def D(day):
        return {'date': (mon + timedelta(days=day)).isoformat()}

    def ev(c, key, summary, start, end, **extra):
        e = g_new_event(c, det_id('gev', key), {'summary': summary, 'start': start, 'end': end, **extra})
        e['created'] = ts_ms(utcnow() - timedelta(days=30))
        c['events'][e['id']] = g_touch(e)
        return e

    def exception(c, master, day_offset, patch=None, cancel=False):
        occ = datetime.fromisoformat(T(day_offset, *map(int, master['start']['dateTime'][11:16].split(':')))['dateTime'])
        e = make_instance(master, occ, tzname)
        if cancel:
            e['status'] = 'cancelled'
        deep_merge(e, patch or {})
        c['events'][e['id']] = g_touch(e)

    pop30 = {'useDefault': False, 'overrides': [{'method': 'popup', 'minutes': 30}, {'method': 'email', 'minutes': 1440}]}
    ev(P, 'dentist', 'Dentist appointment', T(1, 9), T(1, 10), location='12 Smith St, Hobart TAS', reminders=pop30)
    ev(P, 'lunch', 'Lunch with Sam', T(2, 12, 30), T(2, 13, 30), description='Try the new dumpling place.')
    ev(P, 'bday', "Mum's birthday", D(3), D(4), recurrence=['RRULE:FREQ=YEARLY'],
       reminders={'useDefault': False, 'overrides': [{'method': 'popup', 'minutes': 1440}]}, transparency='transparent')
    ev(P, 'away', 'Weekend away: Freycinet', D(4), D(7), location='Coles Bay TAS', transparency='transparent')
    until = (mon + timedelta(weeks=8)).strftime('%Y%m%d') + 'T000000Z'
    ev(P, 'gym', 'Gym', T(-14, 7), T(-14, 8), recurrence=[f'RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR;UNTIL={until}'])
    ev(P, 'flight', 'Red-eye flight to Perth', T(5, 22), T(6, 6), location='HBA airport',
       description='Overnight, crosses midnight.')
    ev(P, 'uscall', 'Call with US team', T(3, 15, 0, 'America/Los_Angeles'), T(3, 16, 0, 'America/Los_Angeles'),
       hangoutLink='https://meet.google.com/abc-defg-hij')
    first = {'date': today.replace(day=1).isoformat()}
    ev(P, 'rent', 'Pay rent', first, {'date': (today.replace(day=1) + timedelta(days=1)).isoformat()},
       recurrence=['RRULE:FREQ=MONTHLY;COUNT=12'])
    ev(P, 'bookclub', 'Book club', T(-4, 19), T(-4, 21), location='Library')
    ev(P, 'concert', 'Concert', T(10, 20), T(10, 23), location='Theatre Royal')

    attendees = [{'email': email, 'self': True, 'organizer': True, 'responseStatus': 'accepted'},
                 {'email': 'alex@example.com', 'displayName': 'Alex', 'responseStatus': 'accepted'},
                 {'email': 'robin@example.com', 'displayName': 'Robin', 'responseStatus': 'needsAction'}]
    sync = ev(W, 'teamsync', 'Team sync', T(-13, 10), T(-13, 10, 30), recurrence=['RRULE:FREQ=WEEKLY;BYDAY=TU'],
              attendees=attendees, hangoutLink='https://meet.google.com/xyz-team-syn',
              description='Weekly team sync. This week moved to 2pm; next week cancelled.')
    exception(W, sync, 1, {'summary': 'Team sync (moved)', 'start': T(1, 14), 'end': T(1, 14, 30)})
    exception(W, sync, 8, cancel=True)
    ev(W, 'sprint', 'Sprint planning', T(0, 13), T(0, 15), location='Room 3')
    ev(W, 'oneonone', '1:1 with Alex', T(3, 15), T(3, 15, 30))
    ev(W, 'release', 'Release day', D(4), D(5))
    ev(W, 'standup', 'Daily standup', T(-7, 9, 15), T(-7, 9, 30),
       recurrence=['RRULE:FREQ=DAILY;BYDAY=MO,TU,WE,TH,FR;COUNT=30'])

    for key, name, day in (('xmas', 'Christmas Day', date(today.year, 12, 25)),
                           ('boxing', 'Boxing Day', date(today.year, 12, 26)),
                           ('nye', "New Year's Day", date(today.year + 1, 1, 1))):
        ev(H, key, name, {'date': day.isoformat()}, {'date': (day + timedelta(days=1)).isoformat()},
           transparency='transparent', visibility='public')
    ev(H, 'showday', 'Show Day (fake)', D(2), D(3), transparency='transparent', visibility='public')

    # -- Google Tasks
    def tlist(key, title):
        lid = b64url(hashlib.sha1(('gtl' + key).encode()).digest()[:16])
        S['gtasks'][lid] = l = {'meta': bump({'id': lid, 'title': title, 'updated': ts_ms()}), 'tasks': {}}
        return l

    def gtask(l, key, title, parent=None, **fields):
        t = {'id': b64url(hashlib.sha1(('gt' + key).encode()).digest()[:16]), 'title': title, 'status': 'needsAction',
             '_ord': len(l['tasks'])}
        l['tasks'][t['id']] = t
        gt_apply(t, fields)
        t['parent'] = parent and parent['id']
        return t

    day = lambda n: (today + timedelta(days=n)).isoformat() + 'T00:00:00.000Z'
    yest = ts_ms(utcnow() - timedelta(days=1))
    mt = tlist('mytasks', 'My Tasks')
    gtask(mt, 'car', 'Book car service', due=day(2))
    pp = gtask(mt, 'passport', 'Renew passport', notes='Form PC7, need two photos.', due=day(9))
    gtask(mt, 'photos', 'Get passport photos', parent=pp, due=day(5))
    gtask(mt, 'expenses', 'File expense report', status='completed', completed=yest)
    gtask(mt, 'plumber', 'Call plumber', notes='Kitchen tap drips.')
    gr = tlist('groceries', 'Groceries')
    gtask(gr, 'milk', 'Milk')
    gtask(gr, 'eggs', 'Eggs', notes='Free range, dozen')
    gtask(gr, 'coffee', 'Coffee beans', status='completed', completed=yest)
    gtask(gr, 'bread', 'Bread', due=day(0))
    for l in S['gtasks'].values():
        for parent in {t.get('parent') for t in l['tasks'].values()}:
            gt_renumber(l, parent)

    # -- Todoist
    td = S['todoist']
    tid = lambda key: det_id('td', key, 16, B62)
    td['user'] = {'id': '2671355', 'email': 'tester@example.com', 'full_name': 'Test User',
                  'inbox_project_id': tid('inbox'), 'tz_info': {'timezone': tzname}, 'start_day': 1,
                  'start_page': 'today', 'lang': 'en', 'is_premium': True, 'premium_status': 'current_personal_plan',
                  'weekly_goal': 25, 'daily_goal': 5, 'time_format': 0, 'date_format': 0, 'week_start': 1,
                  'avatar_big': None, 'avatar_medium': None, 'avatar_s640': None, 'avatar_small': None,
                  'business_account_id': None, 'is_deleted': False, 'joined_at': '2024-01-15T09:00:00.000000Z'}
    for key, name, color, parent, order, inbox in (('inbox', 'Inbox', 'charcoal', None, 0, True),
                                                   ('work', 'Work', 'blue', None, 1, False),
                                                   ('home', 'Home', 'green', None, 2, False),
                                                   ('garden', 'Garden', 'lime_green', tid('home'), 1, False)):
        td['projects'][tid(key)] = bump(new_project(tid(key), name, color, parent, order, inbox))
    td['projects'][tid('work')]['is_favorite'] = True
    for i, (key, pkey, name) in enumerate((('thisweek', 'work', 'This week'), ('backlog', 'work', 'Backlog'))):
        td['sections'][tid(key)] = bump(new_section(tid(key), tid(pkey), name, i + 1))
    for i, (name, color) in enumerate((('errand', 'orange'), ('phone', 'sky_blue'), ('waiting', 'grey'),
                                       ('deep-work', 'violet'))):
        td['labels'][tid('label-' + name)] = bump(new_label(tid('label-' + name), name, color, i + 1))

    def ttask(key, content, pkey, order, completed_days=None, **b):
        t = new_task(tid(key), content, tid(pkey))
        t['child_order'] = order
        for k in ('section_id', 'parent_id'):
            if k in b:
                t[k] = tid(b.pop(k))
        td_apply(t, b)
        if completed_days is not None:
            t.update(checked=True, completed_at=ts_us(utcnow() - timedelta(days=completed_days)),
                     completed_by_uid=td['user']['id'])
        td['tasks'][t['id']] = bump(t)

    tomorrow2pm = datetime.combine(today + timedelta(days=1), dtime(14, 0), tz).astimezone(UTC).isoformat()
    ttask('card', 'Buy birthday card', 'inbox', 1, due_string='today', priority=3, labels=['errand'])
    ttask('article', 'Read article on CalDAV sync', 'inbox', 2, priority=1,
          description='https://datatracker.ietf.org/doc/html/rfc6578')
    ttask('report', 'Weekly report', 'work', 1, due_string='every monday', priority=2, section_id='thisweek')
    ttask('slides', 'Prepare slides for review', 'work', 2, due_datetime=tomorrow2pm, priority=4,
          duration=60, duration_unit='minute', section_id='thisweek',
          deadline_date=(mon + timedelta(days=4)).isoformat())
    ttask('vendor', 'Reply to vendor email', 'work', 3, priority=3, labels=['waiting'], due_string='tomorrow')
    ttask('refactor', 'Refactor sync engine', 'work', 4, priority=2, labels=['deep-work'], section_id='backlog')
    ttask('synctests', 'Write tests for sync engine', 'work', 5, parent_id='refactor', section_id='backlog',
          due_date=(today + timedelta(days=6)).isoformat())
    ttask('timesheet', 'Submit timesheet', 'work', 6, completed_days=2, priority=2)
    ttask('power', 'Pay electricity bill', 'home', 1, priority=4, labels=['errand'],
          due_date=(today + timedelta(days=3)).isoformat())
    ttask('tap', 'Fix leaking tap', 'home', 2, priority=2)
    ttask('council', 'Call council about bins', 'home', 3, labels=['phone'], due_string='tomorrow at 10am')
    ttask('plants', 'Water plants', 'home', 4, due_string='every day')
    ttask('recycling', 'Take out recycling', 'home', 5, completed_days=5)
    ttask('tomatoes', 'Plant tomatoes', 'garden', 1, due_date=(today + timedelta(days=12)).isoformat(), priority=3)
    ttask('mulch', 'Buy mulch', 'garden', 2, labels=['errand'])
    # A pre-registered confidential client for apps that ship a fixed client id/secret.
    td['clients']['aircal-test'] = {'client_id': 'aircal-test', 'client_secret': 'aircal-secret', 'redirect_uris': [],
                                    'token_endpoint_auth_method': 'client_secret_post',
                                    'grant_types': ['authorization_code', 'refresh_token']}

# ----------------------------------------------------------------------------- admin, index


def admin_reset(req):
    keep = S['auth'] if req.q.get('auth') != '1' else None
    clients = {k: v for k, v in S['todoist']['clients'].items() if k != 'aircal-test'} if keep else {}
    seed(keep)
    S['todoist']['clients'].update(clients)
    return {'ok': True, 'gen': S['gen']}


def admin_state(req):
    return S


def admin_expire(req):
    n = 0
    for t in S['auth']['tokens'].values():
        t['exp'] = 0
        n += 1
    if req.q.get('refresh') == '1':
        S['auth']['refresh'].clear()
    return {'ok': True, 'expired': n, 'refresh_revoked': req.q.get('refresh') == '1'}


def admin_invalidate_sync(req):
    S['gen'] += 1
    return {'ok': True, 'gen': S['gen']}


def index(req):
    b = 'http://' + req.host
    return {'service': 'fake-cloud', 'google': {
        'auth': b + '/google/o/oauth2/v2/auth', 'token': b + '/google/token', 'revoke': b + '/google/revoke',
        'userinfo': b + '/google/oauth2/v3/userinfo', 'calendar': b + '/google/calendar/v3',
        'tasks': b + '/google/tasks/v1'}, 'todoist': {
        'register': b + '/todoist/oauth/register', 'authorize': b + '/todoist/oauth/authorize',
        'token': b + '/todoist/oauth/access_token', 'api': b + '/todoist/api/v1'},
        'admin': [b + '/admin/reset', b + '/admin/state', b + '/admin/expire-tokens', b + '/admin/invalidate-sync']}


def g_revoke(req):
    tok = req.body.get('token') or req.q.get('token', '')
    S['auth']['tokens'].pop(tok, None)
    S['auth']['refresh'].pop(tok, None)
    return {}

# ----------------------------------------------------------------------------- routing / HTTP


GC, GT, TD = 'google/calendar/v3', 'google/tasks/v1', 'todoist/api/v1'
ROUTES = [
    ('GET', '', index),
    ('GET', 'google/o/oauth2/v2/auth', lambda r: oauth_authorize(r, 'google')),
    ('GET', 'google/o/oauth2/auth', lambda r: oauth_authorize(r, 'google')),
    ('POST', 'google/token', lambda r: oauth_token(r, 'google')),
    ('POST', 'google/revoke', g_revoke),
    ('GET', 'google/oauth2/v3/userinfo', g_userinfo),
    ('GET', 'google/openidconnect/v1/userinfo', g_userinfo),
    ('GET', GC + '/users/me/calendarList', g_calendar_list),
    ('GET', GC + '/users/me/calendarList/{c}', g_calendar_list_get),
    ('GET', GC + '/calendars/{c}', g_calendar_get),
    ('GET', GC + '/calendars/{c}/events', g_events_list),
    ('POST', GC + '/calendars/{c}/events', g_event_insert),
    ('GET', GC + '/calendars/{c}/events/{e}', g_event_get),
    ('GET', GC + '/calendars/{c}/events/{e}/instances', g_event_instances),
    ('PATCH', GC + '/calendars/{c}/events/{e}', g_event_update),
    ('PUT', GC + '/calendars/{c}/events/{e}', lambda r, c, e: g_event_update(r, c, e, replace=True)),
    ('DELETE', GC + '/calendars/{c}/events/{e}', g_event_delete),
    ('GET', GT + '/users/@me/lists', gt_lists),
    ('POST', GT + '/users/@me/lists', gt_list_insert),
    ('GET', GT + '/users/@me/lists/{l}', gt_list_get),
    ('PATCH', GT + '/users/@me/lists/{l}', gt_list_update),
    ('PUT', GT + '/users/@me/lists/{l}', gt_list_update),
    ('DELETE', GT + '/users/@me/lists/{l}', gt_list_delete),
    ('GET', GT + '/lists/{l}/tasks', gt_tasks),
    ('POST', GT + '/lists/{l}/tasks', gt_task_insert),
    ('POST', GT + '/lists/{l}/clear', gt_clear),
    ('GET', GT + '/lists/{l}/tasks/{t}', gt_task_get),
    ('PATCH', GT + '/lists/{l}/tasks/{t}', gt_task_update),
    ('PUT', GT + '/lists/{l}/tasks/{t}', lambda r, l, t: gt_task_update(r, l, t, replace=True)),
    ('DELETE', GT + '/lists/{l}/tasks/{t}', gt_task_delete),
    ('POST', GT + '/lists/{l}/tasks/{t}/move', gt_task_move),
    ('POST', 'todoist/oauth/register', td_register),
    ('GET', 'todoist/oauth/authorize', lambda r: oauth_authorize(r, 'todoist')),
    ('POST', 'todoist/oauth/access_token', lambda r: oauth_token(r, 'todoist')),
    ('GET', TD + '/user', td_user),
    ('GET', TD + '/projects', td_list('projects')),
    ('POST', TD + '/projects', td_project_create),
    ('GET', TD + '/projects/{p}', td_get('projects')),
    ('POST', TD + '/projects/{p}', td_project_create),
    ('DELETE', TD + '/projects/{p}', td_project_delete),
    ('GET', TD + '/sections', td_list('sections')),
    ('POST', TD + '/sections', td_section_create),
    ('GET', TD + '/sections/{s}', td_get('sections')),
    ('GET', TD + '/labels', td_list('labels')),
    ('POST', TD + '/labels', td_label_create),
    ('GET', TD + '/tasks', td_tasks),
    ('POST', TD + '/tasks', td_task_create),
    ('GET', TD + '/tasks/completed/by_completion_date', td_completed),
    ('GET', TD + '/tasks/{t}', td_task_get),
    ('POST', TD + '/tasks/{t}', td_task_update),
    ('DELETE', TD + '/tasks/{t}', td_task_delete),
    ('POST', TD + '/tasks/{t}/close', td_task_close),
    ('POST', TD + '/tasks/{t}/reopen', td_task_reopen),
    ('POST', TD + '/tasks/{t}/move', td_task_move),
    ('POST', TD + '/sync', td_sync),
    ('POST', 'admin/reset', admin_reset),
    ('GET', 'admin/state', admin_state),
    ('POST', 'admin/expire-tokens', admin_expire),
    ('POST', 'admin/invalidate-sync', admin_invalidate_sync),
]
ROUTES = [(m, tuple(p.split('/')) if p else (), f) for m, p, f in ROUTES]


def route(method, segs):
    allowed = False
    for m, pat, f in ROUTES:
        if len(pat) != len(segs):
            continue
        caps = []
        for p, s in zip(pat, segs):
            if p.startswith('{'):
                caps.append(s)
            elif p != s:
                break
        else:
            if m == method:
                return f, caps
            allowed = True
    return None, allowed


class Req:
    pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'fake-cloud/1.0'

    def log_message(self, fmt, *args):  # replaced by our one-line log (no queries, no tokens)
        pass

    def _send(self, status, body, headers):
        if isinstance(body, (dict, list)):
            data, ctype = json.dumps(body, indent=1).encode(), 'application/json; charset=UTF-8'
        elif isinstance(body, str):
            data, ctype = body.encode(), 'text/html; charset=utf-8'
        else:
            data, ctype = b'', None
        self.send_response(status)
        if ctype:
            self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    def _handle(self):
        u = urlsplit(self.path)
        segs = tuple(unquote(s) for s in u.path.strip('/').split('/')) if u.path.strip('/') else ()
        req = Req()
        req.path, req.headers = u.path, self.headers
        req.host = self.headers.get('Host') or '%s:%d' % self.server.server_address[:2]
        req.q = dict(parse_qsl(u.query, keep_blank_values=True))
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        req.body = {}
        status, body, headers = 500, None, {}
        try:
            if raw:
                ctype = self.headers.get('Content-Type', '')
                if 'json' in ctype or raw.lstrip()[:1] in (b'{', b'['):
                    try:
                        req.body = json.loads(raw)
                    except ValueError:
                        raise HTTPError(400, {'error': 'invalid_request', 'error_description': 'Invalid JSON body'})
                    if not isinstance(req.body, dict):
                        raise HTTPError(400, {'error': 'invalid_request', 'error_description': 'Expected an object'})
                else:
                    req.body = dict(parse_qsl(raw.decode('utf-8', 'replace'), keep_blank_values=True))
            with LOCK:
                f, caps = route(self.command, segs)
                if f is None:
                    raise HTTPError(405 if caps else 404, {'error': {'code': 405 if caps else 404,
                                                                     'message': 'Method Not Allowed' if caps else 'Not Found'}})
                p = '/'.join(segs)
                if p.startswith(('google/calendar/', 'google/tasks/', 'google/oauth2/', 'google/openidconnect/')):
                    g_require_auth(req)
                elif p.startswith('todoist/api/'):
                    td_require_auth(req)
                r = f(req, *caps)
                status, body, headers = r if isinstance(r, tuple) else (200, r, {})
                if self.command != 'GET' and OPTS.state:
                    save_state()
        except HTTPError as e:
            status, body, headers = e.status, e.body, e.headers
        except Exception as e:  # a bug in the fake: say so loudly
            import traceback
            traceback.print_exc()
            status, body = 500, {'error': {'code': 500, 'message': 'fake-cloud internal error: %r' % e}}
        sys.stderr.write('%s %s %d\n' % (self.command, u.path, status))
        self._send(status, body, headers)

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = _handle


def save_state():
    tmp = OPTS.state + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(S, f)
    os.replace(tmp, OPTS.state)


def main():
    global OPTS, S
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=18480)
    ap.add_argument('--state', help='JSON file to load state from and save it to after every change')
    ap.add_argument('--seed', action='store_true', help='start from seed data even if --state exists')
    ap.add_argument('--token-ttl', type=int, default=3600, help='access token lifetime in seconds (default 3600)')
    ap.add_argument('--todoist-token', action='append', default=None,
                    help='personal Todoist API token accepted as Bearer (repeatable; default test-todoist-token)')
    ap.add_argument('--tz', default='Australia/Hobart', help='calendar / Todoist user time zone for seed data')
    OPTS = ap.parse_args()
    OPTS.todoist_token = OPTS.todoist_token or ['test-todoist-token']
    ZoneInfo(OPTS.tz)
    if OPTS.state and os.path.exists(OPTS.state) and not OPTS.seed:
        with open(OPTS.state) as f:
            S = json.load(f)
    else:
        seed()
        if OPTS.state:
            save_state()
    srv = ThreadingHTTPServer((OPTS.host, OPTS.port), Handler)
    srv.daemon_threads = True
    sys.stderr.write('fake-cloud listening on http://%s:%d/ (tz %s, token ttl %ds)\n'
                     % (OPTS.host, srv.server_address[1], OPTS.tz, OPTS.token_ttl))
    sys.stderr.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
