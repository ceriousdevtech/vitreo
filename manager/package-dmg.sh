#!/bin/bash
# Build a Release Vitreo.app and pack it into a compressed DMG.
#
# Local:
#   manager/package-dmg.sh
#   VITREO_VERSION=1.0.0 manager/package-dmg.sh
#
# The GitHub Actions workflow .github/workflows/release.yml runs this script.
# The DMG is ad-hoc signed unless VITREO_CODESIGN_IDENTITY names another
# identity. Ad-hoc signatures are not accepted by Gatekeeper on other Macs.
# Notarization is not part of this script.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
    echo "error: Vitreo disk images are built on Apple Silicon Macs" >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake is not installed" >&2
    exit 1
fi
cmake_version="$(cmake --version | awk 'NR==1 { print $3 }')"
cmake_major="${cmake_version%%.*}"
cmake_rest="${cmake_version#*.}"
cmake_minor="${cmake_rest%%.*}"
if [ "${cmake_major}" -lt 3 ] || { [ "${cmake_major}" -eq 3 ] && [ "${cmake_minor}" -lt 25 ]; }; then
    echo "error: CMake 3.25 or newer is required (found ${cmake_version})" >&2
    exit 1
fi

version_is_safe() {
    [[ "$1" =~ ^[0-9A-Za-z][0-9A-Za-z._+-]*$ ]]
}

if [ -n "${VITREO_VERSION:-}" ]; then
    VERSION="${VITREO_VERSION}"
elif git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    VERSION="$(git -C "${ROOT}" describe --tags --always --dirty)"
    VERSION="${VERSION#v}"
else
    echo "error: set VITREO_VERSION (this directory is not a git checkout)" >&2
    exit 1
fi
if ! version_is_safe "${VERSION}"; then
    echo "error: version must be a filename-safe label such as 1.0.0 (got ${VERSION})" >&2
    exit 1
fi

BUILD_DIR="${VITREO_BUILD_DIR:-${ROOT}/build/package}"

echo "=== Configuring Release engine (${BUILD_DIR}) ==="
# Pin arm64. An x86_64 cmake (Rosetta) would otherwise select the in-development
# Intel backend from CMAKE_SYSTEM_PROCESSOR, and this disk image is arm64 only.
if command -v ninja >/dev/null 2>&1; then
    cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DVITREO_BUILD_TESTS=OFF \
        -DCMAKE_OSX_ARCHITECTURES=arm64
else
    cmake -S "${ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DVITREO_BUILD_TESTS=OFF \
        -DCMAKE_OSX_ARCHITECTURES=arm64
fi

echo "=== Building vitreo ==="
cmake --build "${BUILD_DIR}" --target vitreo --parallel

ENGINE="${BUILD_DIR}/src/vitreo"
if [ ! -x "${ENGINE}" ]; then
    echo "error: engine was not produced at ${ENGINE}" >&2
    exit 1
fi
engine_arch="$(lipo -archs "${ENGINE}")"
if [ "${engine_arch}" != "arm64" ]; then
    echo "error: engine architecture is ${engine_arch}, expected arm64" >&2
    exit 1
fi

echo "=== Building Vitreo.app ==="
VITREO_ENGINE="${ENGINE}" "${ROOT}/manager/build.sh" release

APP="${ROOT}/manager/build/release/Vitreo.app"
HELPERS="${APP}/Contents/Helpers"
RESOURCES="${APP}/Contents/Resources"
NOTICES="${RESOURCES}/third-party-notices"
if [ ! -x "${APP}/Contents/MacOS/Vitreo" ] || [ ! -x "${HELPERS}/vitreo" ]; then
    echo "error: ${APP} is missing the app or the engine" >&2
    exit 1
fi
for binary in "${APP}/Contents/MacOS/Vitreo" "${HELPERS}/vitreo" "${APP}/Contents/Resources/VitreoLauncher"; do
    [ -e "${binary}" ] || continue
    binary_arch="$(lipo -archs "${binary}")"
    if [ "${binary_arch}" != "arm64" ]; then
        echo "error: ${binary} architecture is ${binary_arch}, expected arm64" >&2
        exit 1
    fi
done
if [ ! -f "${RESOURCES}/LICENSE" ]; then
    echo "error: ${RESOURCES}/LICENSE is missing" >&2
    exit 1
fi
notice_count=0
for notice in "${ROOT}/third_party/notices/runtime/"*; do
    [ -f "${notice}" ] || continue
    notice_count=$((notice_count + 1))
    name="$(basename "${notice}")"
    if [ ! -f "${NOTICES}/${name}" ]; then
        echo "error: app is missing third-party notice ${name}" >&2
        exit 1
    fi
done
if [ "${notice_count}" -eq 0 ]; then
    echo "error: third_party/notices/runtime is empty" >&2
    exit 1
fi

IDENTITY="${VITREO_CODESIGN_IDENTITY:--}"
echo "=== Signing (${IDENTITY}) ==="
codesign --force --sign "${IDENTITY}" "${HELPERS}/vitreo"
if [ -x "${APP}/Contents/Resources/VitreoLauncher" ]; then
    codesign --force --sign "${IDENTITY}" "${APP}/Contents/Resources/VitreoLauncher"
fi
codesign --force --sign "${IDENTITY}" "${APP}"
codesign --verify --strict --verbose=2 "${APP}"

DIST="${ROOT}/dist"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/vitreo-dmg.XXXXXX")"
MOUNT=""
cleanup() {
    if [ -n "${MOUNT}" ]; then
        hdiutil detach "${MOUNT}" >/dev/null 2>&1 || true
        case "${MOUNT}" in
            /Volumes/*) ;;
            *) rmdir "${MOUNT}" 2>/dev/null || true ;;
        esac
    fi
    rm -rf "${STAGE}"
}
trap cleanup EXIT

mkdir -p "${STAGE}/root" "${DIST}"
ditto "${APP}" "${STAGE}/root/Vitreo.app"
ln -s /Applications "${STAGE}/root/Applications"

ONE_X="${STAGE}/background.png"
BACKGROUND="${STAGE}/background.png"
layout="$(swift "${ROOT}/manager/dmg-background.swift" "${ONE_X}" 1)"
layout_line="$(printf '%s\n' "${layout}" | awk '/^LAYOUT / { print; exit }')"
if [ -z "${layout_line}" ]; then
    echo "error: background generator did not report a layout" >&2
    printf '%s\n' "${layout}" >&2
    exit 1
fi
# shellcheck disable=SC2086
set -- ${layout_line}
window_width="$3"
window_height="$4"
icon_size="$6"
app_x="$8"
app_y="$9"
applications_x="${11}"
applications_y="${12}"
# Finder on macOS 26 does not paint a background that still has an alpha
# channel, or a multi-image TIFF. Flatten to one opaque picture.
sips -s format jpeg -s formatOptions 95 "${ONE_X}" --out "${STAGE}/background.jpg" >/dev/null
sips -s format png "${STAGE}/background.jpg" --out "${BACKGROUND}" >/dev/null

VOLUME_ICON="${STAGE}/VolumeIcon.icns"
swift "${ROOT}/manager/dmg-volume-icon.swift" \
    "${APP}/Contents/Resources/AppIcon.icns" \
    "${VOLUME_ICON}"

DMG="${DIST}/Vitreo-${VERSION}-arm64.dmg"
SUM="${DMG}.sha256"
rm -f "${DMG}" "${SUM}"
RW_IMAGE="${STAGE}/vitreo-rw.dmg"

payload_kb="$(du -sk "${STAGE}/root" | awk '{ print $1 }')"
image_mb="$((payload_kb / 1024 + 48))"

if [ -d /Volumes/Vitreo ]; then
    hdiutil detach /Volumes/Vitreo -force >/dev/null 2>&1 \
        || diskutil unmount force /Volumes/Vitreo >/dev/null 2>&1 \
        || true
fi

echo "=== Creating ${DMG} ==="
hdiutil create \
    -size "${image_mb}m" \
    -fs HFS+ \
    -volname "Vitreo" \
    -ov \
    "${RW_IMAGE}"

attach_output="$(hdiutil attach "${RW_IMAGE}")"
printf '%s\n' "${attach_output}"
MOUNT="$(printf '%s\n' "${attach_output}" | awk '/Apple_HFS/ { print $NF; exit }')"
if [ -z "${MOUNT}" ] || [ ! -d "${MOUNT}" ]; then
    echo "error: the disk image did not mount" >&2
    exit 1
fi
ditto "${STAGE}/root/Vitreo.app" "${MOUNT}/Vitreo.app"
swift "${ROOT}/manager/dmg-applications-alias.swift" "${MOUNT}/Applications"
# One hidden file at the volume root. A picture inside .background/ is stored
# with a null in the alias path, and Finder then leaves the window white.
cp "${BACKGROUND}" "${MOUNT}/.background.png"
if [ ! -f "${VOLUME_ICON}" ]; then
    echo "error: the disk image icon was not created" >&2
    exit 1
fi
cp "${VOLUME_ICON}" "${MOUNT}/.VolumeIcon.icns"
if command -v SetFile >/dev/null 2>&1; then
    SetFile -a C "${MOUNT}"
fi

if command -v SetFile >/dev/null 2>&1; then
    SetFile -a E "${MOUNT}/Vitreo.app"
fi

# The layout is written directly. Driving Finder needs an Automation
# permission this script does not have, and macOS 26 drops a background
# whose store entry includes a bookmark.
python3 -m pip install --disable-pip-version-check --quiet \
    --target "${STAGE}/py" 'ds_store==1.3.1' 'mac_alias==2.2.2'
PYTHONPATH="${STAGE}/py${PYTHONPATH:+:${PYTHONPATH}}" \
    python3 "${ROOT}/manager/dmg-layout.py" \
    "${MOUNT}" \
    "${MOUNT}/.background.png" \
    "${window_width}" "${window_height}" "${icon_size}" \
    "${app_x}" "${app_y}" "${applications_x}" "${applications_y}" \
    "${MOUNT}/.DS_Store"

if [ ! -s "${MOUNT}/.DS_Store" ]; then
    echo "error: the disk-image layout was not written" >&2
    exit 1
fi
sync
detach_attempt=0
while [ "${detach_attempt}" -lt 5 ]; do
    if hdiutil detach "${MOUNT}"; then
        break
    fi
    detach_attempt=$((detach_attempt + 1))
    sleep 2
done
if [ "${detach_attempt}" -eq 5 ]; then
    echo "error: could not unmount the disk image after saving its layout" >&2
    exit 1
fi
MOUNT=""

hdiutil convert "${RW_IMAGE}" -format UDZO -ov -o "${DMG}"
hdiutil verify "${DMG}"

MOUNT="$(mktemp -d "${TMPDIR:-/tmp}/vitreo-dmg-mount.XXXXXX")"
hdiutil attach "${DMG}" -nobrowse -readonly -mountpoint "${MOUNT}"
test -x "${MOUNT}/Vitreo.app/Contents/MacOS/Vitreo"
test -x "${MOUNT}/Vitreo.app/Contents/Helpers/vitreo"
test -f "${MOUNT}/Vitreo.app/Contents/Resources/LICENSE"
test -d "${MOUNT}/Vitreo.app/Contents/Resources/third-party-notices"
test -e "${MOUNT}/Applications"
test ! -L "${MOUNT}/Applications"
test -s "${MOUNT}/.DS_Store"
test -f "${MOUNT}/.background.png"
test ! -d "${MOUNT}/.background"
test -f "${MOUNT}/.VolumeIcon.icns"
for notice in "${ROOT}/third_party/notices/runtime/"*; do
    [ -f "${notice}" ] || continue
    name="$(basename "${notice}")"
    test -f "${MOUNT}/Vitreo.app/Contents/Resources/third-party-notices/${name}"
done
hdiutil detach "${MOUNT}" >/dev/null
rmdir "${MOUNT}"
MOUNT=""

shasum -a 256 "${DMG}" | tee "${SUM}"
swift "${ROOT}/manager/dmg-volume-icon.swift" --apply "${VOLUME_ICON}" "${DMG}"
echo "DMG: ${DMG}"
