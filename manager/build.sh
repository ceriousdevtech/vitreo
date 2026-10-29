#!/bin/bash
# Build script for Vitreo (fallback when xcodegen is not available)
# Usage: ./build.sh [debug|release]

set -euo pipefail
cd "$(dirname "$0")"

CONFIG="${1:-debug}"
BUILD_DIR="build/${CONFIG}"
APP_NAME="Vitreo"
APP_BUNDLE="${BUILD_DIR}/${APP_NAME}.app"

# Collect all Swift source files
SOURCES=$(find VitreoManager -name "*.swift" -type f)

if [ -z "$SOURCES" ]; then
    echo "Error: No Swift source files found"
    exit 1
fi

echo "=== Building Vitreo (${CONFIG}) ==="

# Create app bundle structure
mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"

# Compile
SWIFT_FLAGS="-O"
if [ "$CONFIG" = "debug" ]; then
    SWIFT_FLAGS="-Onone -g"
fi

echo "Compiling ${APP_NAME}..."
swiftc \
    ${SWIFT_FLAGS} \
    -target arm64-apple-macos14.0 \
    -sdk "$(xcrun --show-sdk-path --sdk macosx)" \
    -module-name "${APP_NAME}" \
    -emit-executable \
    -o "${APP_BUNDLE}/Contents/MacOS/${APP_NAME}" \
    ${SOURCES}

echo "Compiling native launcher helper..."
swiftc \
    -O \
    -target arm64-apple-macos14.0 \
    -sdk "$(xcrun --show-sdk-path --sdk macosx)" \
    -o "${APP_BUNDLE}/Contents/Resources/VitreoLauncher" \
    LauncherHelper/main.swift
chmod 755 "${APP_BUNDLE}/Contents/Resources/VitreoLauncher"

# Create Info.plist
cat > "${APP_BUNDLE}/Contents/Info.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>Vitreo</string>
    <key>CFBundleExecutable</key>
    <string>Vitreo</string>
    <key>CFBundleIdentifier</key>
    <string>dev.vitreo.manager</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>Vitreo</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0 Beta</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>Windows applications run by Vitreo record from the microphone when they ask for audio input.</string>
    <key>NSMainStoryboardFile</key>
    <string></string>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.utilities</string>
    <!-- Keep this file-association metadata equivalent to
         VitreoManager/Info.plist: both build paths must register the same
         Finder behavior. -->
    <key>UTImportedTypeDeclarations</key>
    <array>
        <dict>
            <key>UTTypeIdentifier</key>
            <string>com.microsoft.windows-executable</string>
            <key>UTTypeDescription</key>
            <string>Windows executable</string>
            <key>UTTypeConformsTo</key>
            <array>
                <string>public.data</string>
                <string>public.executable</string>
            </array>
            <key>UTTypeTagSpecification</key>
            <dict>
                <key>public.filename-extension</key>
                <array><string>exe</string></array>
                <key>public.mime-type</key>
                <array><string>application/x-msdownload</string></array>
            </dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.windows-installer-package</string>
            <key>UTTypeDescription</key><string>Windows Installer package</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict>
                <key>public.filename-extension</key><array><string>msi</string></array>
                <key>public.mime-type</key><array><string>application/x-msi</string></array>
            </dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.windows-installer-patch</string>
            <key>UTTypeDescription</key><string>Windows Installer patch</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>msp</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.windows-installer-transform</string>
            <key>UTTypeDescription</key><string>Windows Installer transform</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>mst</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.windows-installer-merge-module</string>
            <key>UTTypeDescription</key><string>Windows Installer merge module</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>msm</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.msix-package</string>
            <key>UTTypeDescription</key><string>MSIX package</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>msix</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.msix-bundle</string>
            <key>UTTypeDescription</key><string>MSIX bundle</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>msixbundle</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.appx-package</string>
            <key>UTTypeDescription</key><string>AppX package</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>appx</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.appx-bundle</string>
            <key>UTTypeDescription</key><string>AppX bundle</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>appxbundle</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.appinstaller</string>
            <key>UTTypeDescription</key><string>App Installer file</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>appinstaller</string></array></dict>
        </dict>
        <dict>
            <key>UTTypeIdentifier</key><string>com.microsoft.windows-update-package</string>
            <key>UTTypeDescription</key><string>Windows Update package</string>
            <key>UTTypeConformsTo</key><array><string>public.data</string></array>
            <key>UTTypeTagSpecification</key>
            <dict><key>public.filename-extension</key><array><string>msu</string></array></dict>
        </dict>
    </array>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>Windows executable</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.windows-executable</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>Windows Installer package</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.windows-installer-package</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>Windows Installer patch</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.windows-installer-patch</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>Windows Installer transform</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.windows-installer-transform</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>Windows Installer merge module</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.windows-installer-merge-module</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>MSIX package</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.msix-package</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>MSIX bundle</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.msix-bundle</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>AppX package</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.appx-package</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>AppX bundle</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.appx-bundle</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>App Installer file</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.appinstaller</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>Windows Update package</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.microsoft.windows-update-package</string></array>
        </dict>
    </array>
    <key>LSSupportsOpeningDocumentsInPlace</key>
    <true/>
</dict>
</plist>
PLIST

# Compile the asset catalog, including the app icon. actool only emits
# AppIcon.icns when --app-icon names the set.
if command -v actool &>/dev/null; then
    ACTOOL_PLIST="$(mktemp)"
    ACTOOL_PARTIAL="${APP_BUNDLE}/Contents/Info-actool.plist"
    actool --compile "${APP_BUNDLE}/Contents/Resources" \
           --platform macosx \
           --minimum-deployment-target 14.0 \
           --app-icon AppIcon \
           --output-partial-info-plist "${ACTOOL_PARTIAL}" \
           VitreoManager/Assets.xcassets > "${ACTOOL_PLIST}"
    if plutil -extract com.apple.actool.errors raw "${ACTOOL_PLIST}" >/dev/null 2>&1; then
        echo "error: actool failed" >&2
        plutil -p "${ACTOOL_PLIST}" >&2
        exit 1
    fi
    for key in CFBundleIconFile CFBundleIconName; do
        value="$(plutil -extract "${key}" raw "${ACTOOL_PARTIAL}" 2>/dev/null || true)"
        if [ -n "${value}" ]; then
            /usr/libexec/PlistBuddy -c "Delete :${key}" \
                "${APP_BUNDLE}/Contents/Info.plist" >/dev/null 2>&1 || true
            /usr/libexec/PlistBuddy -c "Add :${key} string ${value}" \
                "${APP_BUNDLE}/Contents/Info.plist"
        fi
    done
    rm -f "${ACTOOL_PLIST}" "${ACTOOL_PARTIAL}"
fi

# The management app and the engine ship together. The engine cannot be named
# Vitreo inside Contents/MacOS: that is the app executable, and the volume is
# case-insensitive. Contents/Helpers is the auxiliary executable location.
# VITREO_ENGINE overrides the default ../build/src/vitreo (an absolute path).
if [ -n "${VITREO_ENGINE:-}" ]; then
    if [ ! -x "${VITREO_ENGINE}" ]; then
        echo "error: VITREO_ENGINE is not an executable: ${VITREO_ENGINE}" >&2
        exit 1
    fi
    ENGINE_BINARY="${VITREO_ENGINE}"
elif [ -x "../build/src/vitreo" ]; then
    ENGINE_BINARY="../build/src/vitreo"
else
    ENGINE_BINARY=""
fi
mkdir -p "${APP_BUNDLE}/Contents/Helpers" "${APP_BUNDLE}/Contents/Resources"
rm -f "${APP_BUNDLE}/Contents/Resources/vitreo"
# Helpers is nested code. Loose license files there fail codesign --verify --strict.
rm -rf "${APP_BUNDLE}/Contents/Helpers/third-party-notices" \
       "${APP_BUNDLE}/Contents/Resources/third-party-notices"
rm -f "${APP_BUNDLE}/Contents/Helpers/LICENSE" \
      "${APP_BUNDLE}/Contents/Resources/LICENSE"
if [ -n "${ENGINE_BINARY}" ]; then
    ENGINE_DIR="$(cd "$(dirname "${ENGINE_BINARY}")" && pwd)"
    # CMake places these beside the loose vitreo binary. Inside the app they
    # ship as resources, because Helpers may contain only signed executables.
    if [ ! -d "${ENGINE_DIR}/third-party-notices" ] || [ ! -f "${ENGINE_DIR}/LICENSE" ]; then
        echo "error: ${ENGINE_DIR} is missing third-party-notices or LICENSE" >&2
        echo "Build the vitreo target so the license files are copied next to the engine." >&2
        exit 1
    fi
    cp "${ENGINE_BINARY}" "${APP_BUNDLE}/Contents/Helpers/vitreo"
    chmod 755 "${APP_BUNDLE}/Contents/Helpers/vitreo"
    cp -R "${ENGINE_DIR}/third-party-notices" "${APP_BUNDLE}/Contents/Resources/third-party-notices"
    cp "${ENGINE_DIR}/LICENSE" "${APP_BUNDLE}/Contents/Resources/LICENSE"
else
    echo "Warning: Vitreo engine was not found; the app will require an engine path in Settings."
fi

echo "=== Build complete: ${APP_BUNDLE} ==="
echo "Run with: open ${APP_BUNDLE}"
