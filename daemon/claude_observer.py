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

def fetch_official_budget():
    """Official monthly (used, limit) in USD from Anthropic's OAuth usage endpoint.

    Same undocumented endpoint claude.ai uses; authenticated with the OAuth
    token Claude Code keeps in ~/.claude/.credentials.json. Returns None on
    any failure (expired token, no extra-usage billing, network) — callers
    fall back to config + JSONL estimates.
    """
    try:
        creds = json.loads(
            (Path.home() / ".claude" / ".credentials.json").read_text()
        ).get("claudeAiOauth", {})
        token = creds.get("accessToken")
        expires_ms = creds.get("expiresAt", 0)
        if not token or (expires_ms and time.time() * 1000 > expires_ms - 60_000):
            return None
        req = urllib.request.Request(
            "https://api.anthropic.com/api/oauth/usage",
            headers={
                "Authorization": f"Bearer {token}",
                "anthropic-beta": "oauth-2025-04-20",
            },
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            extra = json.load(resp).get("extra_usage") or {}
        if not extra.get("is_enabled"):
            return None
        return extra["used_credits"] / 100, extra["monthly_limit"] / 100
    except Exception as exc:  # noqa: BLE001 — always fall back to estimates
        log.debug("official usage endpoint unavailable: %s", exc)
        return None


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


def _total_tokens(usage: dict) -> int:
    return (
        usage.get("input_tokens", 0)
        + usage.get("output_tokens", 0)
        + usage.get("cache_creation_input_tokens", 0)
        + usage.get("cache_read_input_tokens", 0)
    )


def collect_usage(cfg: dict) -> dict:
    """Scan transcripts covering last month + this month and aggregate."""
    now = datetime.now().astimezone()
    today = now.date()
    month_start = today.replace(day=1)
    last_month_end = month_start
    last_month_start = (month_start.replace(day=1) - date.resolution).replace(day=1)
    scan_from = datetime.combine(last_month_start, datetime.min.time()).astimezone()

    pricing = cfg["pricing"]
    projects = Path(cfg.get("claude_projects_dir", str(Path.home() / ".claude" / "projects"))).expanduser()

    seen = set()  # (message.id, requestId) dedup — retried/streamed entries repeat
    month_cost = today_cost = last_month_cost = 0.0
    month_tokens = today_tokens = 0
    month_models = defaultdict(float)   # family -> cost
    today_models = defaultdict(float)
    today_sessions = set()

    for path in projects.glob("*/*.jsonl"):
        try:
            if datetime.fromtimestamp(path.stat().st_mtime).astimezone() < scan_from:
                continue
            with path.open() as fh:
                for line in fh:
                    if '"assistant"' not in line and '"usage"' not in line:
                        continue
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if rec.get("type") != "assistant":
                        continue
                    msg = rec.get("message") or {}
                    usage = msg.get("usage")
                    ts = rec.get("timestamp")
                    if not usage or not ts:
                        continue
                    key = (msg.get("id"), rec.get("requestId"))
                    if key in seen:
                        continue
                    seen.add(key)
                    try:
                        when = datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()
                    except ValueError:
                        continue
                    day = when.date()
                    if day < last_month_start:
                        continue
                    model = msg.get("model", "unknown")
                    cost = _cost_usd(model, usage, pricing)
                    tokens = _total_tokens(usage)
                    fam = _model_family(model)
                    if last_month_start <= day < last_month_end:
                        last_month_cost += cost
                    if day >= month_start:
                        month_cost += cost
                        month_tokens += tokens
                        month_models[fam] += cost
                    if day == today:
                        today_cost += cost
                        today_tokens += tokens
                        today_models[fam] += cost
                        sid = rec.get("sessionId")
                        if sid:
                            today_sessions.add(sid)
        except OSError:
            continue

    def top3(models: dict) -> list:
        total = sum(models.values()) or 1.0
        ranked = sorted(models.items(), key=lambda kv: -kv[1])[:3]
        return [[name, round(100 * cost / total)] for name, cost in ranked]

    # Official spend/limit beats the JSONL estimate + configured fallback budget.
    official = fetch_official_budget()
    if official:
        month_cost, monthly_budget = official
    else:
        monthly_budget = float(cfg["monthly_budget_usd"])
    days_in_month = calendar.monthrange(today.year, today.month)[1]

    return {
        "mb": [round(month_cost, 2), monthly_budget],
        "db": [round(today_cost, 2), round(monthly_budget / days_in_month, 2)],
        "tm": top3(today_models),
        "mm": top3(month_models),
        "lm": round(last_month_cost, 2),
        "ses": len(today_sessions),
        "tt": today_tokens,
        "mt": month_tokens,
    }


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
                    stats = await asyncio.to_thread(collect_usage, cfg)
                    payload = (json.dumps(stats, separators=(",", ":")) + "\n").encode()
                    log.info("sending %d bytes: %s", len(payload), payload[:120])
                    for i in range(0, len(payload), CHUNK):
                        await asyncio.wait_for(
                            client.write_gatt_char(
                                cfg["ble_char_uuid"], payload[i : i + CHUNK], response=True
                            ),
                            timeout=10,
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
        stats = collect_usage(cfg)
        print(json.dumps(stats, indent=2))
        return
    asyncio.run(push_loop(cfg))


if __name__ == "__main__":
    main()
