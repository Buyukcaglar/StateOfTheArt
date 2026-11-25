#!/bin/sh
# Builds the recompiled demo for Apple platforms.
#   tools/build_apple.sh [--recover]   # --recover: re-run the coverage pass (about 1 minute)
# Steps: host tools -> coverage (13000 frames = the whole demo) -> lift to build/gen -> xcodegen.
set -e
cd "$(dirname "$0")/.."
make build/sota_run build/lift
if [ "$1" = "--recover" ] || [ ! -f build/coverage.txt ]; then
    rm -f build/coverage.txt
    ./build/sota_run --frames 13000 --coverage build/coverage.txt --quiet
fi
make gen
mkdir -p build/xcode
xcodegen generate --spec project.yml
echo "Open StateOfTheArt.xcodeproj, or build from the command line with xcodebuild."
