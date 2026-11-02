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
	if [ -n "$patch" ]; then
		git -C "$dir" diff HEAD | cmp -s - "$patch" \
			|| die "$dir differs from $patch; run tools/save_patches.sh if the change is meant to stay"
	fi
	echo "$dir: ok"
}

fetch() {
	dir="third_party/$1"; url="$2"; commit="$3"; patch="$4"
	if [ -e "$dir" ]; then
		check_existing "$dir" "$commit" "$patch"
		return
	fi
	echo "$dir: cloning $url"
	git clone -q -c core.autocrlf=false "$url" "$dir"
	git -C "$dir" checkout -q "$commit"
	git -C "$dir" -c core.autocrlf=false submodule --quiet update --init --recursive
	[ -z "$patch" ] || git -C "$dir" apply --index "$ROOT/$patch"
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
	mkdir -p "$dir"
	tar -xzf "$BLASTEM_ARCHIVE" -C "$dir"
	git -C "$dir" init -q
	git -C "$dir" config core.autocrlf false
	git -C "$dir" add -A
	git -C "$dir" commit -q -m "$BLASTEM_ARCHIVE"
	touch "$dir/.vendored"
	git -C "$dir" apply --index "$ROOT/$patch"
}

fetch_blastem
fetch clap "$CLAP_URL" "$CLAP_COMMIT"
fetch clap-wrapper "$CLAP_WRAPPER_URL" "$CLAP_WRAPPER_COMMIT" patches/0002-clap-wrapper-hide-program-controls.patch
fetch vst3sdk "$VST3SDK_URL" "$VST3SDK_COMMIT"
fetch mdmi "$MDMI_URL" "$MDMI_COMMIT"

echo "$ROM_SHA256 *roms/mdmi-everdrive-v1.34.bin" | sha256sum -c --quiet - || die "roms/ does not match deps.lock"
echo "All dependencies match tools/deps.lock"
