#!/bin/bash
# From a PC (Git Bash / Linux / macOS): copies the sources to the Steam Frame over ssh, builds the AppImage there and
# copies it back into dist/.
#   FRAME_HOST=frame   ssh host or alias of the headset (key login; see README)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${FRAME_HOST:-frame}"
REMOTE_DIR="framepip-src"

tar -czf - CMakeLists.txt LICENSE THIRD_PARTY_NOTICES.md README.md packaging resources src third_party |
  ssh "$HOST" "rm -rf ~/$REMOTE_DIR && mkdir -p ~/$REMOTE_DIR && tar -xzf - -C ~/$REMOTE_DIR"
ssh "$HOST" "cd ~/$REMOTE_DIR && bash packaging/build-appimage.sh"
mkdir -p dist
scp "$HOST:$REMOTE_DIR/dist/*.AppImage" dist/
ls -l dist/
