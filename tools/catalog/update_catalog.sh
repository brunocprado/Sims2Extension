#!/usr/bin/env bash
# Regenerates ts2catalog.bin / ts2thumbs.bin in TSBin (base game + expansions + the Downloads folder, if it exists).
# Re-run it after adding/removing custom content, or after browsing more of the real Buy catalog in-game
# (that is what fills Documents/.../Thumbnails/ObjectThumbnails.package, the source of the previews).
#   tools/catalog/update_catalog.sh                       defaults
#   tools/catalog/update_catalog.sh --mods "/path/Downloads"   extra mods folder
set -euo pipefail
exec python3 "$(dirname "$0")/build_catalog.py" "$@"
