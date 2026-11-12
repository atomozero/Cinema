#!/bin/sh
# Copyright 2026, Andrea Bernardi
# SPDX-License-Identifier: MIT
#
# build-hpkg.sh: builds Cinema and makes its package, on Haiku or RenkuOS.
# The package goes to the current folder.
set -e
VERSION="1.5.0"
REVISION="1"
ARCH=$(getarch)
TOP=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(pwd)
WORKDIR=$(mktemp -d /tmp/cinema-package.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

cd "$TOP"
make clean
make

# yt-dlp as a Python module where HaikuPorts has it; elsewhere the single
# file yt-dlp in the PATH works too (see README.md). FFmpeg 6, which live TV
# is linked with: a newer one has other libraries.
REQUIRES=""
case $ARCH in
	x86_64)	REQUIRES="yt_dlp_python3.14 ffmpeg6" ;;
	# the libraries themselves on ARM: the ffmpeg package built for it
	# does not provide its own name
	arm)	REQUIRES="lib:libavformat lib:libavcodec lib:libswscale \
			lib:libswresample lib:libavutil" ;;
esac

STAGE=$WORKDIR/stage
mkdir -p "$STAGE/apps/Cinema" \
	"$STAGE/data/locale/catalogs/x-vnd.atomozero-Cinema" \
	"$STAGE/data/deskbar/menu/Applications"
cp objects/Cinema helper/cinema-helper.py "$STAGE/apps/Cinema/"
cp objects/catalogs/*.catalog \
	"$STAGE/data/locale/catalogs/x-vnd.atomozero-Cinema/"
ln -s ../../../../apps/Cinema/Cinema \
	"$STAGE/data/deskbar/menu/Applications/Cinema"

awk -v version="$VERSION" -v revision="$REVISION" -v arch="$ARCH" -v requires="$REQUIRES" '
	/^@REQUIRES@$/ { n = split(requires, list, " ")
		for (i = 1; i <= n; i++) print "\t" list[i]; next }
	{ gsub("@VERSION@", version); gsub("@REVISION@", revision)
		gsub("@ARCH@", arch); print }
	' packaging/Cinema.PackageInfo.in > "$WORKDIR/.PackageInfo"

package create -C "$STAGE" -i "$WORKDIR/.PackageInfo" \
	"$OUT/cinema-$VERSION-$REVISION-$ARCH.hpkg"
echo "$OUT/cinema-$VERSION-$REVISION-$ARCH.hpkg"
