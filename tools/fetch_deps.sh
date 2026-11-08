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

# A dependency comes from vendor/<name>-<commit, 7 digits>.tar.gz when that archive exists (the source
# release holds one for each, D42), else from a clone. The archive is the commit's tree without .git,
# submodules included.
fetch() {
	name="$1"; url="$2"; commit="$3"; patch="$4"
	dir="third_party/$name"
	archive="vendor/$name-$(echo "$commit" | cut -c1-7).tar.gz"
	if [ -e "$dir" ]; then
		check_existing "$dir" "$commit" "$patch"
		return
	fi
	# Only a complete copy gets the final name, so an interrupted one can't pass as existing.
	tmp="$dir.tmp"
	rm -rf "$tmp"
	if [ -f "$archive" ]; then
		echo "$dir: unpacking $archive"
		mkdir -p "$tmp"
		# BlastEm's android/ holds symlinks Windows refuses, and the plugin does not use it.
		tar -xzf "$archive" -C "$tmp" --exclude=android
		git -C "$tmp" init -q
		git -C "$tmp" config core.autocrlf false
		git -C "$tmp" add -A
		git -C "$tmp" -c user.name=fetch_deps -c user.email=fetch_deps@localhost commit -q -m "$archive"
		touch "$tmp/.vendored"
	else
		echo "$dir: cloning $url"
		git clone -q -c core.autocrlf=false "$url" "$tmp"
		git -C "$tmp" checkout -q "$commit"
		git -C "$tmp" -c core.autocrlf=false submodule --quiet update --init --recursive
	fi
	[ -z "$patch" ] || git -C "$tmp" apply --index "$ROOT/$patch"
	mv "$tmp" "$dir"
}

# BlastEm comes from a personal fork, so the repository keeps its archive in vendor/.
fetch blastem "$BLASTEM_URL" "$BLASTEM_COMMIT" patches/0001-mdmi-everdrive-bridge.patch
fetch clap "$CLAP_URL" "$CLAP_COMMIT"
fetch clap-wrapper "$CLAP_WRAPPER_URL" "$CLAP_WRAPPER_COMMIT" patches/0002-clap-wrapper.patch
fetch vst3sdk "$VST3SDK_URL" "$VST3SDK_COMMIT"
fetch mdmi "$MDMI_URL" "$MDMI_COMMIT" patches/0003-mdmi-psg-envelopes.patch

# The ROM's samples, from SAMPLES_ARCHIVE when it exists (the source release), else downloaded.
fetch_samples() {
	dir=third_party/mdmi/res/samples
	[ -e "$dir" ] && return
	zip="$SAMPLES_ARCHIVE"
	if [ ! -f "$zip" ]; then
		zip=third_party/samples.zip
		echo "$dir: downloading $SAMPLES_URL"
		curl -fsSL -o "$zip" "$SAMPLES_URL"
	fi
	echo "$SAMPLES_SHA256 *$zip" | sha256sum -c --quiet - || die "$zip does not match deps.lock"
	rm -rf "$dir.tmp"
	unzip -q "$zip" -d "$dir.tmp"
	mv "$dir.tmp" "$dir"
}
fetch_samples

echo "$ROM_SHA256 *roms/mdmi-everdrive-v1.34.bin" | sha256sum -c --quiet - || die "roms/ does not match deps.lock"
echo "All dependencies match tools/deps.lock"
