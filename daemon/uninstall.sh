#!/bin/zsh
set -euo pipefail
PLIST="$HOME/Library/LaunchAgents/com.claudeobserver.daemon.plist"
launchctl bootout "gui/$(id -u)" "$PLIST" 2>/dev/null || true
rm -f "$PLIST"
echo "Claude Observer daemon uninstalled."
