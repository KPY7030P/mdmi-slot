#!/bin/sh
# Copies the built plugins to the install folder, verifies the copies and records the build there.
#
#   install.sh [folder]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-D:/ALL/VST/CHIPTUNE/MDMI}"
OUT="$ROOT/build/cmake/out"
LABEL="$(cat "$ROOT/build/label.txt")" || exit 1
failed=0

hash_of() {
	sha256sum "$1" | cut -d' ' -f1
}

install_one() {
	source="$OUT/$1"; dest="$TARGET/$2"
	if [ ! -f "$source" ]; then
		echo "$2: NOT BUILT, $source is missing"
		failed=1
	elif ! cp "$source" "$dest"; then
		echo "$2: NOT INSTALLED. If the error above says busy or denied, close Ableton Live and run this again."
		failed=1
	elif [ "$(hash_of "$source")" != "$(hash_of "$dest")" ]; then
		echo "$2: NOT INSTALLED, the copy differs from the build"
		failed=1
	else
		echo "$2: installed, sha256 $(hash_of "$dest")"
	fi
}

echo "Build $LABEL into $TARGET"
install_one "VST3/MDMI Mega Drive.vst3" "MDMI Mega Drive.vst3"
install_one "CLAP/MDMI Mega Drive.clap" "MDMI Mega Drive.clap"

if [ "$failed" != 0 ]; then
	echo "INSTALL FAILED: the folder may still hold an older build."
	rm -f "$TARGET/BUILD.txt"
	exit 1
fi
{
	echo "$LABEL"
	echo "VST3 sha256 $(hash_of "$TARGET/MDMI Mega Drive.vst3")"
	echo "CLAP sha256 $(hash_of "$TARGET/MDMI Mega Drive.clap")"
} > "$TARGET/BUILD.txt" || { echo "INSTALL FAILED: could not write BUILD.txt"; exit 1; }
echo "OK: $TARGET holds build $LABEL"
