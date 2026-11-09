#!/bin/sh
# Builds a release from a clean clone of a tag and writes its two zips to dist/ (D42):
#
#   make_release.sh v0.N.M
#
# MDMI-Slot-0.N.M-win64.zip   the plugins, LICENSE, NOTICE, THIRD_PARTY_NOTICES.txt, README.txt
# MDMI-Slot-0.N.M-source.zip  the tag's tree, and in vendor/ every dependency at its pinned commit
#                                   and the ROM's samples, so it builds without network (GPL section 6)
set -e
TAG="${1:?usage: make_release.sh v0.N.M}"
VERSION="${TAG#v}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
WORK="$DIST/$TAG"

git -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG" > /dev/null || { echo "make_release: no tag $TAG"; exit 1; }
mkdir -p "$DIST"
rm -rf "$WORK"
git clone -q -c core.autocrlf=false --branch "$TAG" "$ROOT" "$WORK"

sh "$WORK/tools/build_plugin.sh"
LABEL="$(cat "$WORK/build/label.txt")"
case "$LABEL" in
	*dirty*|*unknown*) echo "make_release: build label $LABEL is not a clean tag"; exit 1 ;;
esac
echo "Build $LABEL"

failures=0
sh "$WORK/tools/test/run_tests.sh" > "$DIST/$TAG-tests.log" 2>&1 || failures=$?
if [ "$failures" != 0 ]; then
	echo "make_release: run_tests.sh failed $failures checks, see $DIST/$TAG-tests.log"
	exit 1
fi
echo "run_tests.sh: 0 failures"

python "$(cygpath -m "$ROOT/tools/pack_release.py")" "$(cygpath -m "$WORK")" "$VERSION" "$(cygpath -m "$DIST")"
