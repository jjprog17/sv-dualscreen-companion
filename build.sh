#!/bin/sh
# Builds both companions into dist/:
#   dist/Escarlata-v<version>.dsmod.zip   (Pokémon Scarlet, 0100A3D008C5C000)
#   dist/Purpura-v<version>.dsmod.zip     (Pokémon Violet,  01008F6008C5E000)
# Needs clang, lld, llvm-strip and Python 3 (standard library only).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=${1:-$(cat "$HERE/VERSION")}
cd "$HERE"
python3 tools/gen_manifest.py packages art >/dev/null
sh native/build.sh native/out >/dev/null
N=native/out
P=tools/third_party/build_dualscreen_package.py
mkdir -p dist
python3 $P --package packages/PokemonScarlet --output dist --title-id 0100A3D008C5C000 --version "$VERSION" \
  --module android-arm64-v8a=$N/android-arm64-v8a/0100A3D008C5C000.so --module linux-x86_64=$N/linux-x86_64/0100A3D008C5C000.so \
  --build-id C01884F294C053C4 --build-id 421C5411B487EB4D --min-runtime 17 >/dev/null
mv dist/0100A3D008C5C000.dsmod.zip "dist/Escarlata-v$VERSION.dsmod.zip"
python3 $P --package packages/PokemonViolet --output dist --title-id 01008F6008C5E000 --version "$VERSION" \
  --module android-arm64-v8a=$N/android-arm64-v8a/01008F6008C5E000.so --module linux-x86_64=$N/linux-x86_64/01008F6008C5E000.so \
  --build-id E335DE2BF4DCD951 --build-id 709BFD6611529864 --min-runtime 17 >/dev/null
mv dist/01008F6008C5E000.dsmod.zip "dist/Purpura-v$VERSION.dsmod.zip"
ls -l dist
