#!/bin/bash
# Build and install a self-contained Vitreo app for the current user.

set -euo pipefail
cd "$(dirname "$0")"

DESTINATION="${1:-${HOME}/Applications/Vitreo.app}"
SOURCE="build/release/Vitreo.app"

cmake --build ../build --target vitreo
./build.sh release

# Every app bundle on disk registers with Launch Services and adds its own entry
# to Finder's "Open With" menu for .exe files. Keep the backup outside the
# Applications tree so a rollback copy never becomes a second "Vitreo" there.
BACKUP_DIR="${HOME}/Library/Application Support/Vitreo/Backups"
# The previous product name. This install places Vitreo.app beside it and
# moves the old bundle out of Applications so Launch Services drops it.
LEGACY="${HOME}/Applications/Vitrum.app"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

mkdir -p "$(dirname "${DESTINATION}")"
if [ -e "${DESTINATION}" ]; then
    BACKUP="${BACKUP_DIR}/$(basename "${DESTINATION}").previous"
    if [ -e "${BACKUP}" ]; then
        echo "Refusing to overwrite existing backup: ${BACKUP}" >&2
        exit 1
    fi
    mkdir -p "${BACKUP_DIR}"
    mv "${DESTINATION}" "${BACKUP}"
    echo "Previous installation saved as ${BACKUP}"
fi
cp -R "${SOURCE}" "${DESTINATION}"

# Drop the staging bundle for the same reason: the installed app should be the
# only Vitreo users see in Open With.
rm -rf "${SOURCE}"

# The previous product name still registers as its own Open With handler.
if [ "${DESTINATION}" = "${HOME}/Applications/Vitreo.app" ] && [ -e "${LEGACY}" ]; then
    LEGACY_BACKUP="${BACKUP_DIR}/Vitrum.app.previous"
    if [ -e "${LEGACY_BACKUP}" ]; then
        echo "Refusing to overwrite existing backup: ${LEGACY_BACKUP}" >&2
        exit 1
    fi
    mkdir -p "${BACKUP_DIR}"
    mv "${LEGACY}" "${LEGACY_BACKUP}"
    echo "Previous Vitrum installation saved as ${LEGACY_BACKUP}"
    [ -x "${LSREGISTER}" ] && "${LSREGISTER}" -u "${LEGACY_BACKUP}" >/dev/null 2>&1 || true
fi

# Launch Services caches the bundle it replaced; ask it to look at the new one
# so the Open With entry is correct immediately. Best effort only — the
# association does not depend on this tool.
[ -x "${LSREGISTER}" ] && "${LSREGISTER}" -f "${DESTINATION}" >/dev/null 2>&1 || true

# The command-line tool is a symlink to the engine inside the app, so replacing
# the app updates `vitreo` without leaving a second copy behind.
ENGINE="${DESTINATION}/Contents/Helpers/vitreo"
LINK="/usr/local/bin/vitreo"
if [ ! -x "${ENGINE}" ]; then
    echo "Installed Vitreo at ${DESTINATION}" >&2
    echo "error: engine is missing at ${ENGINE}" >&2
    exit 1
fi
if [ -d "/usr/local/bin" ] && [ -w "/usr/local/bin" ]; then
    if [ -e "${LINK}" ] && [ ! -L "${LINK}" ]; then
        echo "Installed Vitreo at ${DESTINATION}" >&2
        echo "Refusing to replace an existing file that is not a symlink: ${LINK}" >&2
        exit 1
    fi
    ln -sfn "${ENGINE}" "${LINK}"
    echo "Command-line tool: ${LINK} -> ${ENGINE}"
else
    echo "Vitreo is installed, but ${LINK} could not be created."
    echo "Link it with: sudo ln -sfn \"${ENGINE}\" \"${LINK}\""
fi

echo "Installed Vitreo at ${DESTINATION}"
echo "Open it with: open \"${DESTINATION}\""
echo "Run a Windows program with: vitreo run <path-to-exe> --capsule <name>"
