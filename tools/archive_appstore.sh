#!/bin/sh
# Release archives for App Store Connect: build/appstore/ios/*.ipa and build/appstore/macos/*.pkg.
# Signing is automatic (team SZ4F4WP934); nothing is uploaded.
set -e
cd "$(dirname "$0")/.."
OUT=build/appstore
rm -rf "$OUT"; mkdir -p "$OUT"
cat > "$OUT/ExportOptions.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>app-store-connect</string>
  <key>destination</key><string>export</string>
  <key>teamID</key><string>SZ4F4WP934</string>
  <key>signingStyle</key><string>automatic</string>
  <key>uploadSymbols</key><true/>
  <key>manageAppVersionAndBuildNumber</key><false/>
</dict></plist>
P
for p in iOS macOS; do
  dest="generic/platform=$p"
  xcodebuild -project StateOfTheArt.xcodeproj -scheme "StateOfTheArt-$p" -configuration Release \
    -destination "$dest" -archivePath "$OUT/$p.xcarchive" -allowProvisioningUpdates archive > "$OUT/archive_$p.log" 2>&1 \
    || { tail -20 "$OUT/archive_$p.log"; exit 1; }
  xcodebuild -exportArchive -archivePath "$OUT/$p.xcarchive" -exportPath "$OUT/$(echo $p | tr A-Z a-z)" \
    -exportOptionsPlist "$OUT/ExportOptions.plist" -allowProvisioningUpdates > "$OUT/export_$p.log" 2>&1 \
    || { tail -20 "$OUT/export_$p.log"; exit 1; }
  echo "$p: $(ls "$OUT/$(echo $p | tr A-Z a-z)" | grep -E '\.(ipa|pkg)$')"
done
