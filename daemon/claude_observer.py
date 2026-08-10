#!/usr/bin/env python3
"""Claude Observer daemon.

Collects Claude Code usage statistics from local JSONL transcripts
(~/.claude/projects/**/*.jsonl), computes budgets/model splits,
and pushes a compact JSON payload over BLE to the ESP32 display.
"""

import asyncio
import calendar
import json
import logging
import sys
import time
import urllib.request
from collections import defaultdict
from datetime import datetime, date
from pathlib import Path

BASE = Path(__file__).resolve().parent
log = logging.getLogger("claude-observer")


# ---------------------------------------------------------------- config

def load_config() -> dict:
    cfg = json.loads((BASE / "config.json").read_text())
    secrets_path = BASE / "secrets.json"
    if secrets_path.exists():
        cfg.update(json.loads(secrets_path.read_text()))
    return cfg


# ---------------------------------------------------------------- official budget

USAGE_CONFIG = Path.home() / ".claude" / "claude_usage_config.json"


def _budget_from_extra(extra) -> tuple | None:
    """Extract monthly (used, limit) in USD from an `extra_usage` block."""
    if not extra or not extra.get("is_enabled"):
        return None
    return extra["used_credits"] / 100, extra["monthly_limit"] / 100


def _fetch_oauth_extra() -> tuple:
    """`extra_usage` via the Claude Code OAuth token in ~/.claude/.credentials.json.

    Returns (extra_usage_or_None, auth_error: bool).
    auth_error=True only when a token is present but explicitly rejected (4xx).
    Returns (None, False) when the token is simply missing/expired.
    """
    try:
        creds = json.loads(
            (Path.home() / ".claude" / ".credentials.json").read_text()
        ).get("claudeAiOauth", {})
        token = creds.get("accessToken")
        expires_ms = creds.get("expiresAt", 0)
        if not token or (expires_ms and time.time() * 1000 > expires_ms - 60_000):
            return None, False
        req = urllib.request.Request(
            "https://api.anthropic.com/api/oauth/usage",
            headers={
                "Authorization": f"Bearer {token}",
                "anthropic-beta": "oauth-2025-04-20",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return json.load(resp).get("extra_usage"), False
        except urllib.error.HTTPError as http_exc:
            if http_exc.code in (401, 403):
                log.warning("oauth token rejected (%d) — key may be invalid", http_exc.code)
                return None, True
            raise
    except Exception as exc:  # noqa: BLE001 — always fall back to estimates
        log.debug("oauth usage endpoint unavailable: %s", exc)
        return None, False


def _load_session_creds(cfg: dict) -> tuple:
    """(session_key, org_id): prefer secrets/config, else ~/.claude/claude_usage_config.json.

    Resolved per-field, NOT all-or-nothing: a secrets.json holding only a fresh
    session_key must still win over the (often stale) key in the usage config,
    borrowing just the missing org_id from it. Treating the pair atomically here
    silently discarded a freshly-pasted key whenever org_id was absent.
    """
    key, org = cfg.get("session_key"), cfg.get("org_id")
    if key and org:
        return key, org
    try:
        uc = json.loads(USAGE_CONFIG.read_text())
    except Exception:  # noqa: BLE001
        uc = {}
    return key or uc.get("session_key"), org or uc.get("org_id")


def _fetch_cookie_extra(cfg: dict) -> tuple:
    """`extra_usage` via a claude.ai browser sessionKey — works when the OAuth token is stale.

    claude.ai sits behind Cloudflare, so the request must present a real browser
    TLS fingerprint; curl_cffi's chrome impersonation does that. Plain urllib is
    tried only as a last resort (usually served a Cloudflare challenge instead).

    Returns (extra_usage_or_None, auth_error: bool).
    auth_error=True only when credentials are present but explicitly rejected (4xx).
    Returns (None, False) when credentials are simply absent.
    """
    key, org = _load_session_creds(cfg)
    if not key or not org:
        return None, False
    url = f"https://claude.ai/api/organizations/{org}/usage"
    try:
        try:
            from curl_cffi import requests as r
            resp = r.get(
                url,
                cookies={"sessionKey": key},
                headers={"User-Agent": "Mozilla/5.0"},
                impersonate="chrome110",
            )
            if resp.status_code in (401, 403):
                log.warning("sessionKey rejected (%d) — key may be expired", resp.status_code)
                return None, True
            data = resp.json()
        except ImportError:
            req = urllib.request.Request(
                url, headers={"User-Agent": "Mozilla/5.0", "Cookie": f"sessionKey={key}"}
            )
            try:
                with urllib.request.urlopen(req, timeout=10) as resp:
                    data = json.load(resp)
            except urllib.error.HTTPError as http_exc:
                if http_exc.code in (401, 403):
                    log.warning("sessionKey rejected (%d) — key may be expired", http_exc.code)
                    return None, True
                raise
        return data.get("extra_usage"), False
    except Exception as exc:  # noqa: BLE001 — always fall back to estimates
        log.debug("cookie usage endpoint unavailable: %s", exc)
        return None, False


def fetch_official_budget(cfg: dict) -> tuple:
    """Official monthly (used, limit) in USD from claude.ai's usage endpoint.

    Tries the OAuth token first, then the browser sessionKey.
    Returns (budget_or_None, auth_error: bool).

    auth_error=True signals that credentials ARE present but were explicitly
    rejected — the caller should surface this to the user (e.g. via the display).
    budget=None with auth_error=False means no auth configured or a transient
    network failure; callers fall back to config + JSONL estimates.
    """
    extra, oauth_auth_err = _fetch_oauth_extra()
    budget = _budget_from_extra(extra)
    if budget:
        return budget, False

    # A rejected OAuth token must NOT short-circuit the cookie path: on
    # enterprise/SSO logins that token is routinely stale, and the sessionKey
    # is what actually carries the data. Only report an auth error if the
    # cookie path also fails to produce a budget.
    extra, cookie_auth_err = _fetch_cookie_extra(cfg)
    budget = _budget_from_extra(extra)
    if budget:
        return budget, False
    return None, (cookie_auth_err or oauth_auth_err)


# ---------------------------------------------------------------- usage collection

def _model_family(model: str) -> str:
    """claude-opus-4-8 -> opus, claude-haiku-4-5-20251001 -> haiku ..."""
    for fam in ("fable", "mythos", "opus", "sonnet", "haiku"):
        if fam in model:
            return fam
    return model.replace("claude-", "").split("-")[0] or "other"


def _cost_usd(model: str, usage: dict, pricing: dict) -> float:
    fam = _model_family(model)
    p = pricing.get(fam, pricing.get("default"))
    return (
        usage.get("input_tokens", 0) * p["input"]
        + usage.get("output_tokens", 0) * p["output"]
        + usage.get("cache_creation_input_tokens", 0) * p["cache_write"]
        + usage.get("cache_read_input_tokens", 0) * p["cache_read"]
    ) / 1_000_000


def _top3(models: dict) -> list:
    total = sum(models.values()) or 1.0
    ranked = sorted(models.items(), key=lambda kv: -kv[1])[:3]
    return [[name, round(100 * cost / total)] for name, cost in ranked]


class UsageCollector:
    """Incremental aggregator over the JSONL transcripts.

    A full rescan of every file each cycle is wasteful: 99%+ of the bytes are
    unchanged and only the tail of one or two active files is ever new. This
    keeps per-day running totals in memory and, each refresh, reads only the
    bytes appended to files whose mtime/size changed since last time. The
    official-budget network call is cached separately so it isn't hit every
    cycle. State lives for the life of the process; a restart rebuilds it with
    one full scan.
    """

    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.pricing = cfg["pricing"]
        self.projects = Path(
            cfg.get("claude_projects_dir", str(Path.home() / ".claude" / "projects"))
        ).expanduser()
        self.official_ttl = cfg.get("official_budget_ttl_seconds", 300)

        # path -> (mtime, byte offset already consumed)
        self._files: dict = {}
        self._seen = set()  # (message.id, requestId) dedup — retried/streamed entries repeat
        # per-day running totals, pruned to the current-month window
        self._day_cost = defaultdict(float)
        self._day_models = defaultdict(lambda: defaultdict(float))  # day -> fam -> cost
        self._day_sessions = defaultdict(set)
        self._min_day = None  # oldest day we retain, set each refresh

        self._official = None       # cached (used, limit)
        self._official_at = 0.0     # monotonic-ish wall time of last fetch
        self._auth_error = False    # True when creds present but rejected
        # start-of-day value of the official monthly counter (gitignored,
        # survives restarts) — lets us derive today's real spend incl. web
        self._baseline_path = Path(__file__).parent / "day_baseline.json"

    # ---- ingest ----

    def _ingest(self, rec: dict) -> None:
        if rec.get("type") != "assistant":
            return
        msg = rec.get("message") or {}
        usage = msg.get("usage")
        ts = rec.get("timestamp")
        if not usage or not ts:
            return
        key = (msg.get("id"), rec.get("requestId"))
        if key in self._seen:
            return
        try:
            day = datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone().date()
        except ValueError:
            return
        if self._min_day and day < self._min_day:
            return
        self._seen.add(key)
        model = msg.get("model", "unknown")
        fam = _model_family(model)
        cost = _cost_usd(model, usage, self.pricing)
        self._day_cost[day] += cost
        self._day_models[day][fam] += cost
        sid = rec.get("sessionId")
        if sid:
            self._day_sessions[day].add(sid)

    def _read_new_bytes(self, path: Path, offset: int) -> int:
        """Consume complete (newline-terminated) lines from `offset`; return new offset."""
        with path.open("rb") as fh:
            fh.seek(offset)
            data = fh.read()
        if not data:
            return offset
        end = data.rfind(b"\n")
        if end == -1:  # only a partial line so far — wait for it to complete
            return offset
        for raw in data[: end + 1].splitlines():
            if b'"assistant"' not in raw and b'"usage"' not in raw:
                continue
            try:
                self._ingest(json.loads(raw))
            except json.JSONDecodeError:
                continue
        return offset + end + 1

    def refresh(self) -> None:
        """Scan for changed files and ingest only their appended bytes."""
        # Only the current month is reported now, so nothing older needs reading.
        self._min_day = date.today().replace(day=1)
        scan_from = datetime.combine(self._min_day, datetime.min.time()).astimezone()

        for path in self.projects.glob("*/*.jsonl"):
            try:
                st = path.stat()
                if datetime.fromtimestamp(st.st_mtime).astimezone() < scan_from:
                    continue
                prev_mtime, offset = self._files.get(path, (None, 0))
                if prev_mtime == st.st_mtime and offset == st.st_size:
                    continue  # unchanged since last cycle — skip entirely
                if st.st_size < offset:  # truncated/rotated — reread from start
                    offset = 0
                self._files[path] = (st.st_mtime, self._read_new_bytes(path, offset))
            except OSError:
                continue

        self._prune(self._min_day)

    def _prune(self, min_day) -> None:
        for bucket in (self._day_cost, self._day_models, self._day_sessions):
            for d in [d for d in bucket if d < min_day]:
                del bucket[d]

    # ---- output ----

    def _official_budget(self):
        now = time.time()
        if self._official is None or now - self._official_at >= self.official_ttl:
            fetched, auth_err = fetch_official_budget(self.cfg)
            self._auth_error = auth_err
            if fetched:
                self._official = fetched
            self._official_at = now
        return self._official

    def _official_day_spent(self, official_used: float, local_today: float) -> float:
        """Today's spend from deltas of the official monthly counter.

        The endpoint only exposes a monthly running total — but that total
        includes claude.ai web usage the local JSONL estimate can't see, so we
        persist its value at the start of each day and report used-minus-
        baseline. The very first run seeds the baseline with the local
        estimate (earlier web spend that day is unrecoverable).
        """
        today = date.today().isoformat()
        try:
            base = json.loads(self._baseline_path.read_text())
        except (OSError, ValueError):
            base = None
        if base is None:  # first ever run — seed from the local estimate
            base = {"day": today, "used": max(0.0, official_used - local_today)}
        elif base["day"] != today:  # new day starts at zero
            base = {"day": today, "used": official_used}
        elif official_used < base["used"]:  # monthly counter reset mid-day
            base = {"day": today, "used": 0.0}
        else:
            return official_used - base["used"]
        try:
            self._baseline_path.write_text(json.dumps(base))
        except OSError as exc:
            log.warning("cannot persist day baseline: %s", exc)
        return official_used - base["used"]

    def snapshot(self) -> dict:
        today = date.today()
        month_start = today.replace(day=1)

        month_cost = sum(c for d, c in self._day_cost.items() if d >= month_start)
        month_models = defaultdict(float)
        for d, fams in self._day_models.items():
            if d >= month_start:
                for fam, c in fams.items():
                    month_models[fam] += c

        # Official spend/limit beats the JSONL estimate + configured fallback budget.
        official = self._official_budget()
        day_spent = self._day_cost.get(today, 0.0)
        if official:
            month_cost, monthly_budget = official
            day_spent = self._official_day_spent(month_cost, day_spent)
        else:
            monthly_budget = float(self.cfg["monthly_budget_usd"])
        days_in_month = calendar.monthrange(today.year, today.month)[1]
        # Remaining budget spread over the weekdays left this month (incl. today).
        weekdays_left = sum(
            1 for day in range(today.day, days_in_month + 1)
            if date(today.year, today.month, day).weekday() < 5
        )
        day_budget = (monthly_budget - month_cost + day_spent) / max(weekdays_left, 1)

        snap = {
            "mb": [round(month_cost, 2), monthly_budget],
            "db": [round(day_spent, 2), round(day_budget, 2)],
            "tm": _top3(self._day_models.get(today, {})),
            "mm": _top3(month_models),
            "ses": len(self._day_sessions.get(today, set())),
        }
        if self._auth_error:
            snap["ae"] = 1  # auth error — credentials present but rejected
        return snap

    def collect(self) -> dict:
        self.refresh()
        return self.snapshot()


# ---------------------------------------------------------------- session-key prompt

def _osascript_ask(prompt_text: str, title: str) -> str | None:
    """Show a macOS input dialog; return the trimmed answer or None if cancelled."""
    import subprocess

    line1 = f'set msg to {prompt_text}'
    line2 = (
        f'display dialog msg default answer ""'
        f' with title "{title}" with icon caution'
        f' buttons {{"Cancel", "Save"}} default button "Save"'
    )
    try:
        result = subprocess.run(
            ["osascript", "-e", line1, "-e", line2],
            capture_output=True, text=True, timeout=300,
        )
    except Exception as exc:
        log.error("osascript failed to launch: %s", exc)
        return None

    if result.returncode != 0:
        if result.stderr.strip():
            log.warning("osascript error: %s", result.stderr.strip())
        else:
            log.info("user cancelled osascript dialog")
        return None

    # Output: "button returned:Save, text returned:<value>"
    # Use rfind so a comma inside the answer value doesn't break parsing.
    out = result.stdout.strip()
    marker = "text returned:"
    idx = out.rfind(marker)
    if idx == -1:
        return None
    return out[idx + len(marker):].strip() or None


def _prompt_new_session_key(cfg: dict) -> bool:
    """Ask the user for a new sessionKey (and org_id if missing). Updates secrets.json.

    Returns True if a new key was saved, False if the user cancelled.
    """
    secrets_path = BASE / "secrets.json"
    try:
        secrets = json.loads(secrets_path.read_text()) if secrets_path.exists() else {}
    except Exception:
        secrets = {}

    prompt_text = (
        '"ClaudeObserver: session key rejected (403)."'
        ' & return & return'
        ' & "Paste the new sessionKey from claude.ai:"'
        ' & return & "(DevTools → Application → Cookies → sessionKey)"'
    )
    new_key = _osascript_ask(prompt_text, "ClaudeObserver")
    if not new_key:
        return False
    secrets["session_key"] = new_key
    log.info("new sessionKey accepted (%d chars)", len(new_key))

    # Pin org_id into secrets.json alongside the key. It may already be
    # resolvable from claude_usage_config.json — copy that value rather than
    # asking, so the two credentials always travel together in one file.
    if not secrets.get("org_id"):
        _, resolved_org = _load_session_creds(cfg)
        if resolved_org:
            secrets["org_id"] = resolved_org
            log.info("org_id copied from claude_usage_config.json")
        else:
            org_prompt = (
                '"org_id is missing."'
                ' & return & return'
                ' & "Paste your org UUID from:"'
                ' & return & "https://claude.ai/api/organizations"'
            )
            org_id = _osascript_ask(org_prompt, "ClaudeObserver — org_id")
            if org_id:
                secrets["org_id"] = org_id
                log.info("org_id saved")

    secrets_path.write_text(json.dumps(secrets, indent=2))
    log.info("secrets.json updated")
    return True


# ---------------------------------------------------------------- BLE

CHUNK = 180  # bytes per GATT write; payload terminated by '\n'


async def find_device(cfg: dict):
    from bleak import BleakScanner

    return await BleakScanner.find_device_by_filter(
        lambda d, ad: cfg["ble_service_uuid"].lower() in [u.lower() for u in (ad.service_uuids or [])]
        or (d.name or "") == cfg["ble_device_name"],
        timeout=15.0,
    )


async def push_loop(cfg: dict):
    from bleak import BleakClient

    interval = cfg.get("update_interval_seconds", 60)
    collector = UsageCollector(cfg)
    _prompting = False          # guard: only one dialog open at a time
    _suppress_until = 0.0      # don't re-prompt before this time.time() value

    async def _handle_auth_error() -> None:
        """Prompt the user for a new key (once at a time) and reload state."""
        nonlocal cfg, _prompting, _suppress_until
        if _prompting or time.time() < _suppress_until:
            return
        _prompting = True
        try:
            saved = await asyncio.to_thread(_prompt_new_session_key, cfg)
            if saved:
                # Reload config so the new key is picked up, and reset the
                # official-budget cache so the next collect() retries at once.
                cfg = load_config()
                collector.cfg = cfg
                collector._official = None
                collector._official_at = 0.0
                collector._auth_error = False
                # Suppress re-prompts for 5 minutes — gives the daemon time to
                # test the new key and avoids spamming if it's still rejected.
                _suppress_until = time.time() + 300
            else:
                log.info("user cancelled key prompt — will keep showing XX eyes")
                # Don't re-open the dialog for 10 minutes after a cancellation.
                _suppress_until = time.time() + 600
        finally:
            _prompting = False

    while True:
        device = await find_device(cfg)
        if device is None:
            log.info("display not found, rescanning...")
            await asyncio.sleep(3)
            continue

        # Fires the moment the link drops — e.g. the ESP reboots — so we stop
        # waiting out the interval and loop straight back to rescan/reconnect.
        dropped = asyncio.Event()
        log.info("connecting to %s", device)
        try:
            async with BleakClient(
                device, disconnected_callback=lambda _c: dropped.set()
            ) as client:
                log.info("connected, streaming updates")
                while client.is_connected and not dropped.is_set():
                    stats = await asyncio.to_thread(collector.collect)
                    payload = (json.dumps(stats, separators=(",", ":")) + "\n").encode()
                    log.info("sending %d bytes: %s", len(payload), payload[:120])
                    for i in range(0, len(payload), CHUNK):
                        await asyncio.wait_for(
                            client.write_gatt_char(
                                cfg["ble_char_uuid"], payload[i : i + CHUNK], response=True
                            ),
                            timeout=10,
                        )
                    # Auth error: show XX eyes on the display and ask for a new key.
                    if stats.get("ae"):
                        task = asyncio.create_task(_handle_auth_error())
                        task.add_done_callback(
                            lambda t: log.error("auth-prompt task crashed: %s", t.exception())
                            if not t.cancelled() and t.exception() else None
                        )
                    # Sleep the interval, but wake immediately if the device drops.
                    try:
                        await asyncio.wait_for(dropped.wait(), timeout=interval)
                    except asyncio.TimeoutError:
                        pass
        except Exception as exc:  # noqa: BLE001 — keep daemon alive on any BLE hiccup
            log.warning("BLE connection lost: %s", exc)
        await asyncio.sleep(2)


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    cfg = load_config()
    if "--once" in sys.argv:  # debug: print stats and exit, no BLE
        stats = UsageCollector(cfg).collect()
        print(json.dumps(stats, indent=2))
        if stats.get("ae"):
            _prompt_new_session_key(cfg)
        return
    asyncio.run(push_loop(cfg))


if __name__ == "__main__":
    main()
