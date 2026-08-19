#!/usr/bin/env python3
"""Claude Observer daemon.

Polls the official Claude usage endpoint for the monthly spend/limit, keeps a
per-day record of that monthly counter, and pushes a compact JSON payload over
BLE to the ESP32 display. Today's spend is simply
`current monthly total - the highest total seen on an earlier day`.
"""

import asyncio
import calendar
import json
import logging
import sys
import time
import urllib.request
from datetime import date
from pathlib import Path

BASE = Path(__file__).resolve().parent
HISTORY = BASE / "daily_spend.json"
log = logging.getLogger("claude-observer")


# ---------------------------------------------------------------- config

def load_config() -> dict:
    cfg = json.loads((BASE / "config.json").read_text())
    secrets_path = BASE / "secrets.json"
    if secrets_path.exists():
        cfg.update(json.loads(secrets_path.read_text()))
    return cfg


# ---------------------------------------------------------------- official usage

USAGE_CONFIG = Path.home() / ".claude" / "claude_usage_config.json"


def _budget_from_extra(extra) -> tuple | None:
    """Monthly (used, limit) in USD from an `extra_usage` block."""
    if not extra or not extra.get("is_enabled"):
        return None
    return extra["used_credits"] / 100, extra["monthly_limit"] / 100


def _fetch_oauth_extra() -> tuple:
    """`extra_usage` via the Claude Code OAuth token in ~/.claude/.credentials.json.

    Returns (extra_usage_or_None, auth_error). auth_error is True only when a
    token is present but explicitly rejected (401/403).
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
                log.warning("oauth token rejected (%d)", http_exc.code)
                return None, True
            raise
    except Exception as exc:  # noqa: BLE001 — never let a fetch kill the daemon
        log.debug("oauth usage endpoint unavailable: %s", exc)
        return None, False


def _load_session_creds(cfg: dict) -> tuple:
    """(session_key, org_id): prefer secrets/config, else ~/.claude/claude_usage_config.json.

    Resolved per-field, NOT all-or-nothing: a secrets.json holding only a fresh
    session_key must still win over the (often stale) key in the usage config,
    borrowing just the missing org_id from it.
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
    """`extra_usage` via a claude.ai browser sessionKey — works when OAuth is stale.

    claude.ai sits behind Cloudflare, so the request needs a real browser TLS
    fingerprint; curl_cffi's chrome impersonation provides one. Plain urllib is
    only a last resort (usually served a Cloudflare challenge instead).
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
                    log.warning("sessionKey rejected (%d)", http_exc.code)
                    return None, True
                raise
        return data.get("extra_usage"), False
    except Exception as exc:  # noqa: BLE001
        log.debug("claude.ai usage endpoint unavailable: %s", exc)
        return None, False


def fetch_usage(cfg: dict) -> tuple:
    """((monthly_used, monthly_limit) or None, auth_error).

    OAuth first, then the browser cookie. A rejected OAuth token must not
    short-circuit the cookie path: on enterprise/SSO logins that token is
    routinely stale and the sessionKey is what carries the data.
    """
    extra, oauth_err = _fetch_oauth_extra()
    budget = _budget_from_extra(extra)
    if budget:
        return budget, False
    extra, cookie_err = _fetch_cookie_extra(cfg)
    budget = _budget_from_extra(extra)
    if budget:
        return budget, False
    return None, (cookie_err or oauth_err)


# ---------------------------------------------------------------- daily history

def _record_day(used: float) -> float:
    """Store today's monthly total and return today's spend.

    The endpoint only exposes a monthly running total, so per-day spend is a
    delta: today = used - the highest total recorded on any earlier day. Each
    day keeps [first_seen, last_seen] of the counter; `first_seen` is the
    fallback baseline when there is no earlier day yet (a fresh install, or the
    1st of the month), and a drop below it means the monthly counter reset.
    """
    today = date.today().isoformat()
    try:
        hist = json.loads(HISTORY.read_text())
    except (OSError, ValueError):
        hist = {}

    first, _ = hist.get(today, [used, used])
    if used < first:  # counter reset (new billing month) — restart from here
        first = used
        hist = {k: v for k, v in hist.items() if k >= today}
    hist[today] = [first, used]

    # Keep the current month only; anything older can't affect a delta.
    cutoff = date.today().replace(day=1).isoformat()
    hist = {k: v for k, v in hist.items() if k >= cutoff}

    earlier = [v[1] for k, v in hist.items() if k < today]
    base = max(earlier) if earlier else first

    try:
        HISTORY.write_text(json.dumps(hist, indent=2, sort_keys=True))
    except OSError as exc:
        log.warning("cannot persist daily history: %s", exc)
    return max(0.0, used - base)


def collect(cfg: dict) -> dict:
    """Payload for the display: month [used, limit], day [used, budget]."""
    usage, auth_error = fetch_usage(cfg)
    if not usage:
        snap = {"mb": [0.0, float(cfg["monthly_budget_usd"])], "db": [0.0, 0.0]}
        if auth_error:
            snap["ae"] = 1  # credentials present but rejected
        return snap

    month_used, month_limit = usage
    day_used = _record_day(month_used)

    today = date.today()
    days_in_month = calendar.monthrange(today.year, today.month)[1]
    # Budget left at the start of today, spread over the weekdays remaining.
    weekdays_left = sum(
        1 for day in range(today.day, days_in_month + 1)
        if date(today.year, today.month, day).weekday() < 5
    )
    day_budget = (month_limit - month_used + day_used) / max(weekdays_left, 1)

    return {
        "mb": [round(month_used, 2), month_limit],
        "db": [round(day_used, 2), round(day_budget, 2)],
    }


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
    except Exception as exc:  # noqa: BLE001
        log.error("osascript failed to launch: %s", exc)
        return None

    if result.returncode != 0:
        if result.stderr.strip():
            log.warning("osascript error: %s", result.stderr.strip())
        else:
            log.info("user cancelled osascript dialog")
        return None

    # Output: "button returned:Save, text returned:<value>"
    # rfind so a comma inside the answer doesn't break parsing.
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
    prompting = False       # guard: only one dialog open at a time
    suppress_until = 0.0    # don't re-prompt before this time.time() value

    async def handle_auth_error() -> None:
        nonlocal cfg, prompting, suppress_until
        if prompting or time.time() < suppress_until:
            return
        prompting = True
        try:
            if await asyncio.to_thread(_prompt_new_session_key, cfg):
                cfg = load_config()  # pick up the new key
                suppress_until = time.time() + 300
            else:
                log.info("user cancelled key prompt — will keep showing XX eyes")
                suppress_until = time.time() + 600
        finally:
            prompting = False

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
                    stats = await asyncio.to_thread(collect, cfg)
                    payload = (json.dumps(stats, separators=(",", ":")) + "\n").encode()
                    log.info("sending %d bytes: %s", len(payload), payload[:120])
                    for i in range(0, len(payload), CHUNK):
                        await asyncio.wait_for(
                            client.write_gatt_char(
                                cfg["ble_char_uuid"], payload[i : i + CHUNK], response=True
                            ),
                            timeout=10,
                        )
                    if stats.get("ae"):  # XX eyes on the display + ask for a new key
                        task = asyncio.create_task(handle_auth_error())
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
        stats = collect(cfg)
        print(json.dumps(stats, indent=2))
        if stats.get("ae"):
            _prompt_new_session_key(cfg)
        return
    asyncio.run(push_loop(cfg))


if __name__ == "__main__":
    main()
