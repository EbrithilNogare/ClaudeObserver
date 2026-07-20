#!/bin/zsh
# Install the Claude Observer daemon as a macOS LaunchAgent (auto-starts at login).
set -euo pipefail

DAEMON_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV="$DAEMON_DIR/.venv"
PLIST_SRC="$DAEMON_DIR/com.claudeobserver.daemon.plist"
PLIST_DST="$HOME/Library/LaunchAgents/com.claudeobserver.daemon.plist"

echo "==> Creating venv + installing dependencies"
python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$DAEMON_DIR/requirements.txt"

if [[ ! -f "$DAEMON_DIR/secrets.json" ]]; then
  cp "$DAEMON_DIR/secrets.example.json" "$DAEMON_DIR/secrets.json"
  echo "==> Created secrets.json — edit your budget there"
fi

echo "==> Installing LaunchAgent"
mkdir -p "$HOME/Library/LaunchAgents"
sed -e "s|__VENV__|$VENV|g" -e "s|__DAEMON__|$DAEMON_DIR|g" "$PLIST_SRC" > "$PLIST_DST"

launchctl bootout "gui/$(id -u)" "$PLIST_DST" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST_DST"
echo "==> Done. Logs: /tmp/claudeobserver.log"
echo "    macOS will ask for Bluetooth permission on first run — allow it."
