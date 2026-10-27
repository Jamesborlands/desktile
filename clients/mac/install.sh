#!/bin/sh
# Builds the SkyTile menu bar app and installs it in ~/Applications, then launches it.
set -e
cd "$(dirname "$0")"
xcodegen -q
xcodebuild -project SkyTileBar.xcodeproj -scheme SkyTileBar -configuration Release \
  -derivedDataPath build -quiet build
pkill -x SkyTile 2>/dev/null || true
mkdir -p ~/Applications
rm -rf ~/Applications/SkyTile.app
cp -R build/Build/Products/Release/SkyTile.app ~/Applications/
open ~/Applications/SkyTile.app
echo "Installed ~/Applications/SkyTile.app"
