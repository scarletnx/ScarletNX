#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# An iOS IPA (docs/hosts/apple.md) signed with a developer account's own certificate, and installed on an iPhone, from a Mac
# with Xcode and no sideloading tool. Run it from Terminal.app on the Mac's own screen: over ssh the keychain is
# locked and codesign fails with errSecInternalComponent.
#
#   scripts/apple-resign.sh IPA --team TEAM --bundle ID --identity IDENTITY [--device UDID] [--out DIR]
#
# TEAM is the account's team ID (10 characters) and ID the app's bundle ID. An app already installed is updated in place,
# its folders kept, only with its own bundle ID (a sideloading tool adds the team: org.scarletnx.TEAM). IDENTITY is the
# name or SHA-1 of an "Apple Development" certificate of that team (security find-identity -v -p codesigning). With
# --device (xcrun devicectl list devices) the app is installed. DIR (default: here) gets the signed ScarletNX.app.
set -eu
usage() { sed -n '8p' "$0" | sed 's/^#   //'; exit 2; }
ipa=${1:-}
[ -n "$ipa" ] && [ -f "$ipa" ] || usage
shift
team= bundle= identity= device= out=$(pwd)
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || usage
    case $1 in
    --team) team=$2 ;;
    --bundle) bundle=$2 ;;
    --identity) identity=$2 ;;
    --device) device=$2 ;;
    --out) out=$2 ;;
    *) usage ;;
    esac
    shift 2
done
[ -n "$team" ] && [ -n "$bundle" ] && [ -n "$identity" ] || usage
ipa=$(cd "$(dirname "$ipa")" && pwd)/$(basename "$ipa")
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# 1. The provisioning profile, which Xcode makes for an app of this bundle ID and team: a stub that builds with
#    -allowProvisioningUpdates, so the profile is made in the account Xcode is signed in to. Its entitlements are what
#    the real app may claim.
stub=$work/stub
mkdir -p "$stub/Stub"
cat >"$stub/Stub/StubApp.swift" <<'EOF'
import SwiftUI

@main
struct StubApp: App {
    var body: some Scene { WindowGroup { Text("stub") } }
}
EOF
mkdir "$stub/Stub.xcodeproj"
cat >"$stub/Stub.xcodeproj/project.pbxproj" <<'EOF'
// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {};
	objectVersion = 56;
	objects = {
		E1 = {isa = PBXBuildFile; fileRef = E2; };
		E2 = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = StubApp.swift; sourceTree = "<group>"; };
		E3 = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = Stub.app; sourceTree = BUILT_PRODUCTS_DIR; };
		A4 = {isa = PBXGroup; children = (E5, A6); sourceTree = "<group>"; };
		E5 = {isa = PBXGroup; children = (E2); path = Stub; sourceTree = "<group>"; };
		A6 = {isa = PBXGroup; children = (E3); name = Products; sourceTree = "<group>"; };
		E6 = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (E1); runOnlyForDeploymentPostprocessing = 0; };
		B3 = {isa = PBXNativeTarget; buildConfigurationList = C4; buildPhases = (E6); buildRules = (); dependencies = (); name = Stub; productName = Stub; productReference = E3; productType = "com.apple.product-type.application"; };
		B2 = {isa = PBXProject; attributes = {LastUpgradeCheck = 2700; }; buildConfigurationList = C1; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en, Base); mainGroup = A4; productRefGroup = A6; projectDirPath = ""; projectRoot = ""; targets = (B3); };
		C1 = {isa = XCConfigurationList; buildConfigurations = (D1); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
		C4 = {isa = XCConfigurationList; buildConfigurations = (D5); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
		D1 = {isa = XCBuildConfiguration; name = Debug; buildSettings = {SDKROOT = iphoneos; IPHONEOS_DEPLOYMENT_TARGET = 16.0; }; };
		D5 = {isa = XCBuildConfiguration; name = Debug; buildSettings = {PRODUCT_NAME = "$(TARGET_NAME)"; PRODUCT_BUNDLE_IDENTIFIER = "@BUNDLE@"; SWIFT_VERSION = 5.0; GENERATE_INFOPLIST_FILE = YES; INFOPLIST_KEY_UILaunchScreen_Generation = YES; CODE_SIGN_STYLE = Automatic; DEVELOPMENT_TEAM = @TEAM@; TARGETED_DEVICE_FAMILY = 1; }; };
	};
	rootObject = B2;
}
EOF
sed -i '' -e "s/@BUNDLE@/$bundle/" -e "s/@TEAM@/$team/" "$stub/Stub.xcodeproj/project.pbxproj"
echo "making the provisioning profile for $bundle (team $team)"
xcodebuild build -project "$stub/Stub.xcodeproj" -scheme Stub -destination generic/platform=iOS \
    -derivedDataPath "$work/derived" -allowProvisioningUpdates >"$work/xcodebuild.log" 2>&1 || {
    tail -n 20 "$work/xcodebuild.log" >&2
    echo "apple-resign: Xcode could not make the profile (is the Apple ID of team $team signed in to Xcode?)" >&2
    exit 1
}
profile=$work/derived/Build/Products/Debug-iphoneos/Stub.app/embedded.mobileprovision
security cms -D -i "$profile" >"$work/profile.plist"
/usr/libexec/PlistBuddy -x -c 'Print :Entitlements' "$work/profile.plist" >"$work/entitlements.plist"

# 2. The app: the profile in it, the bundle ID, then the frameworks and the app signed with the profile's entitlements
unzip -q "$ipa" -d "$work/ipa"
app=$(ls -d "$work"/ipa/Payload/*.app | head -n 1)
cp "$profile" "$app/embedded.mobileprovision"
plutil -replace CFBundleIdentifier -string "$bundle" "$app/Info.plist"
rm -rf "$app/_CodeSignature"
for part in "$app"/Frameworks/*.framework "$app"/Frameworks/*.dylib; do
    if [ -e "$part" ]; then codesign --force --sign "$identity" --timestamp=none "$part"; fi
done
codesign --force --sign "$identity" --entitlements "$work/entitlements.plist" --timestamp=none "$app"
codesign --verify --strict "$app"
rm -rf "$out/$(basename "$app")"
cp -R "$app" "$out/"
echo "signed: $out/$(basename "$app")"

# 3. The install, over the app of the same bundle ID: its dumps, keys and games stay
if [ -n "$device" ]; then
    xcrun devicectl device install app --device "$device" "$out/$(basename "$app")"
fi
