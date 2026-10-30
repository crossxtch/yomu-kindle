#!/bin/sh
# Publish the current main commit as a GitHub release.
#
# Usage: ./release.sh v1.2.3
#
# The tag is v and one to three numbers. This pushes main, then uploads
# yomu-kindle.zip, yomu.apk, and INSTALL-dpt.txt from dist/. Rebuild and
# commit dist/ first.
# A rebuild here would not match the commit. A dirty tree, a detached
# HEAD, or an existing tag is refused.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: ./release.sh v1.2.3" >&2
  exit 1
fi
tag=$1

root=$(CDPATH= cd "$(dirname "$0")" && pwd) || exit 1
cd "$root"

printf '%s\n' "$tag" | grep -Eq '^v[0-9]+(\.[0-9]+){0,2}$' || {
  echo "release: tag must look like v1, v1.2, or v1.2.3" >&2
  exit 1
}

command -v gh >/dev/null 2>&1 || {
  echo "release: gh is not installed" >&2
  exit 1
}
command -v zip >/dev/null 2>&1 || {
  echo "release: zip is not installed" >&2
  exit 1
}
command -v unzip >/dev/null 2>&1 || {
  echo "release: unzip is not installed" >&2
  exit 1
}
command -v file >/dev/null 2>&1 || {
  echo "release: file is not installed" >&2
  exit 1
}
if ! gh auth status >/dev/null 2>&1; then
  echo "release: gh is not logged in" >&2
  exit 1
fi

branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$branch" != main ]; then
  echo "release: checkout main" >&2
  exit 1
fi
if [ -n "$(git status --porcelain)" ]; then
  echo "release: working tree is dirty" >&2
  exit 1
fi
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
  echo "release: tag $tag already exists" >&2
  exit 1
fi
# A prefix match would treat v1 as the existing tag v1.2.3.
if git ls-remote --tags origin | awk -v tag="$tag" '$2 == "refs/tags/" tag { found = 1 } END { exit found ? 0 : 1 }'; then
  echo "release: tag $tag already exists on origin" >&2
  exit 1
fi

kindle=dist/kindle/yomu/yomu
apk=dist/android/yomu.apk
for f in \
  "$kindle" \
  dist/kindle/yomu/yomu.sh \
  dist/kindle/INSTALL.txt \
  dist/kindle/extensions/yomu/config.xml \
  dist/kindle/extensions/yomu/menu.json \
  "$apk" \
  dist/android/INSTALL.txt
do
  if [ ! -f "$f" ]; then
    echo "release: missing $f" >&2
    exit 1
  fi
done

# 32-bit little-endian ELF, ET_EXEC, EM_ARM. file confirms the ABI.
hdr=$(od -An -t x1 -N 20 "$kindle" | tr -d ' \n') || {
  echo "release: cannot read $kindle" >&2
  exit 1
}
case "$hdr" in
  7f454c460101*) ;;
  *)
    echo "release: $kindle is not a 32-bit little-endian ELF" >&2
    exit 1
    ;;
esac
if [ "$(printf '%s' "$hdr" | cut -c33-40)" != "02002800" ]; then
  echo "release: $kindle is not an ARM executable" >&2
  exit 1
fi
desc=$(file -b "$kindle")
case "$desc" in
  *"EABI5"*"statically linked"*"stripped"*) ;;
  *)
    echo "release: $kindle is not a stripped static EABI5 binary ($desc)" >&2
    exit 1
    ;;
esac
if ! unzip -l "$apk" | grep -q 'classes.dex'; then
  echo "release: $apk has no classes.dex" >&2
  exit 1
fi
if ! unzip -l "$apk" | grep -q 'lib/armeabi-v7a/libyomu.so'; then
  echo "release: $apk has no armeabi-v7a libyomu.so" >&2
  exit 1
fi

stage=$(mktemp -d "${TMPDIR:-/tmp}/yomu-release.XXXXXX")
trap 'rm -rf "$stage"' EXIT
(
  cd dist/kindle
  COPYFILE_DISABLE=1 zip -q -X -r "$stage/yomu-kindle.zip" yomu extensions INSTALL.txt
)
cp "$apk" "$stage/yomu.apk"
cp dist/android/INSTALL.txt "$stage/INSTALL-dpt.txt"

sha=$(git rev-parse HEAD)
echo "release: pushing $branch $sha and publishing $tag"
git push origin "$branch"
gh release create "$tag" \
  "$stage/yomu-kindle.zip#Kindle" \
  "$stage/yomu.apk#DPT-RP1" \
  "$stage/INSTALL-dpt.txt#DPT-RP1 notes" \
  --target "$sha" \
  --generate-notes \
  --fail-on-no-commits \
  --notes "yomu-kindle.zip unpacks at the Kindle USB root. yomu.apk installs on the DPT-RP1 with adb install -r. INSTALL-dpt.txt is the DPT-RP1 note."
git fetch origin "refs/tags/$tag:refs/tags/$tag"
echo "release: $tag"
