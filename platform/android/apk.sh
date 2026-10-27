#!/bin/sh
# Pack classes.dex, libyomu.so, and the manifest into a v1-signed APK.
# Android 5.1 verifies JAR signatures. v2 is ignored there.
set -eu
build=${1:?build dir}
dest=${2:?dist dir}
so=${3:?libyomu.so}
sdk=${ANDROID_SDK:-}

if [ -z "$sdk" ] || [ ! -d "$sdk" ]; then
  echo "android: SDK not found. Set ANDROID_SDK_ROOT or install commandlinetools." >&2
  exit 1
fi

jar=$(ls -d "$sdk"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1 || true)
tools=$(ls -d "$sdk"/build-tools/* 2>/dev/null | sort -V | tail -1 || true)
if [ -z "$jar" ] || [ ! -f "$jar" ] || [ -z "$tools" ]; then
  echo "android: install platforms;android-34 and build-tools;35.0.0 with sdkmanager." >&2
  exit 1
fi

d8=$tools/d8
if [ ! -x "$d8" ]; then
  d8=$(command -v d8 || true)
fi
aapt=$tools/aapt
zipalign=$tools/zipalign
apksigner=$tools/apksigner
for tool in "$d8" "$aapt" "$zipalign" "$apksigner"; do
  if [ ! -x "$tool" ]; then
    echo "android: missing $tool" >&2
    exit 1
  fi
done

root=$(pwd)
classes=$build/classes
dex=$build/dex
stage=$build/apk
rm -rf "$classes" "$dex" "$stage"
mkdir -p "$classes" "$dex" "$stage/lib/armeabi-v7a" "$dest"

javac --release 8 -classpath "$jar" -d "$classes" \
  "$root/platform/android/src/dev/yomu/YomuActivity.java" \
  "$root/platform/android/src/dev/yomu/YomuNative.java" \
  "$root/platform/android/src/dev/yomu/YomuView.java"

"$d8" --min-api 22 --lib "$jar" --output "$dex" "$classes"/dev/yomu/*.class
"$aapt" package -f -M "$root/platform/android/AndroidManifest.xml" -I "$jar" -F "$build/yomu-unsigned.apk"

cp "$dex/classes.dex" "$stage/classes.dex"
cp "$so" "$stage/lib/armeabi-v7a/libyomu.so"
(
  cd "$stage"
  zip -r -n .so "$root/$build/yomu-unsigned.apk" classes.dex lib
)

"$zipalign" -f 4 "$build/yomu-unsigned.apk" "$build/yomu-aligned.apk"

ks=$build/debug.keystore
if [ ! -f "$ks" ]; then
  keytool -genkeypair -keystore "$ks" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=yomu Debug, O=yomu, C=US"
fi

"$apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android \
  --v1-signing-enabled true --v2-signing-enabled false \
  --v3-signing-enabled false --v4-signing-enabled false \
  --out "$dest/yomu.apk" "$build/yomu-aligned.apk"
cp "$root/platform/android/INSTALL.txt" "$dest/INSTALL.txt"
