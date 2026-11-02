#!/bin/sh
# Clones third_party/ at the commits in tools/deps.lock and applies patches/. Safe to rerun: an
# existing clone is checked and left alone.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. tools/deps.lock

die() {
	echo "fetch_deps: $*" >&2
	exit 1
}

check_existing() {
	dir="$1"; commit="$2"; patch="$3"
	[ -f "$dir/.vendored" ] || [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] \
		|| die "$dir is not at $commit"
	[ -n "$patch" ] || patch=/dev/null
	git -C "$dir" diff HEAD | cmp -s - "$patch" \
		|| die "$dir differs from ${patch}; if the change is meant to stay, run tools/save_patches.sh for a patched clone"
	if git -C "$dir" submodule status --recursive | grep -q '^[-+U]'; then
		die "$dir has submodules out of step; delete it and run this again"
	fi
}

fetch() {
	dir="third_party/$1"; url="$2"; commit="$3"; patch="$4"
	if [ -e "$dir" ]; then
		check_existing "$dir" "$commit" "$patch"
		return
	fi
	echo "$dir: cloning $url"
	# Only a complete clone gets the final name, so an interrupted one can't pass as existing.
	tmp="$dir.tmp"
	rm -rf "$tmp"
	git clone -q -c core.autocrlf=false "$url" "$tmp"
	git -C "$tmp" checkout -q "$commit"
	git -C "$tmp" -c core.autocrlf=false submodule --quiet update --init --recursive
	[ -z "$patch" ] || git -C "$tmp" apply --index "$ROOT/$patch"
	mv "$tmp" "$dir"
}

# BlastEm comes from a personal fork, so a copy of its source is kept in vendor/.
fetch_blastem() {
	dir=third_party/blastem
	patch=patches/0001-mdmi-everdrive-bridge.patch
	if [ -e "$dir" ] || git ls-remote -q "$BLASTEM_URL" > /dev/null 2>&1; then
		fetch blastem "$BLASTEM_URL" "$BLASTEM_COMMIT" "$patch"
		return
	fi
	echo "$dir: $BLASTEM_URL is unreachable, using $BLASTEM_ARCHIVE"
	tmp="$dir.tmp"
	rm -rf "$tmp"
	mkdir -p "$tmp"
	# android/ holds symlinks Windows refuses, and the plugin does not use it.
	tar -xzf "$BLASTEM_ARCHIVE" -C "$tmp" --exclude=android
	git -C "$tmp" init -q
	git -C "$tmp" config core.autocrlf false
	git -C "$tmp" add -A
	git -C "$tmp" commit -q -m "$BLASTEM_ARCHIVE"
	touch "$tmp/.vendored"
	git -C "$tmp" apply --index "$ROOT/$patch"
	mv "$tmp" "$dir"
}

fetch_blastem
fetch clap "$CLAP_URL" "$CLAP_COMMIT"
fetch clap-wrapper "$CLAP_WRAPPER_URL" "$CLAP_WRAPPER_COMMIT" patches/0002-clap-wrapper-hide-program-controls.patch
fetch vst3sdk "$VST3SDK_URL" "$VST3SDK_COMMIT"
fetch mdmi "$MDMI_URL" "$MDMI_COMMIT"

echo "$ROM_SHA256 *roms/mdmi-everdrive-v1.34.bin" | sha256sum -c --quiet - || die "roms/ does not match deps.lock"
echo "All dependencies match tools/deps.lock"
