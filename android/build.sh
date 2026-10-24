#!/usr/bin/env bash
# Builds rng-lens.apk without Gradle: aapt -> javac -> dx -> aapt -> zipalign -> apksigner.
# Needs: JDK, android-23 platform jar, aapt, dx (dalvik-exchange), zipalign, apksigner.
# Ubuntu: apt install android-sdk-platform-23 aapt dalvik-exchange zipalign apksigner
set -euo pipefail
export KEYSTORE_PASS KEY_PASS 2>/dev/null || true
cd "$(dirname "$0")"

ANDROID_JAR="${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}"
DX="${DX:-$(command -v dalvik-exchange || command -v dx)}"
OUT=build
# Release builds: KEYSTORE, KEYSTORE_PASS, KEY_ALIAS, KEY_PASS (CI passes them from secrets).
# Without them a local debug key is created and used.
KEYSTORE="${KEYSTORE:-$PWD/rnglens.keystore}"
KEYSTORE_PASS="${KEYSTORE_PASS:-rnglens}"
KEY_ALIAS="${KEY_ALIAS:-rnglens}"
KEY_PASS="${KEY_PASS:-$KEYSTORE_PASS}"
# Optional: VERSION_NAME=1.2.3 VERSION_CODE=10203 (CI derives them from the git tag).

rm -rf "$OUT"
mkdir -p "$OUT/gen" "$OUT/obj"

MANIFEST="$OUT/AndroidManifest.xml"
cp AndroidManifest.xml "$MANIFEST"
[ -n "${VERSION_NAME:-}" ] && sed -i "s/android:versionName=\"[^\"]*\"/android:versionName=\"$VERSION_NAME\"/" "$MANIFEST"
[ -n "${VERSION_CODE:-}" ] && sed -i "s/android:versionCode=\"[^\"]*\"/android:versionCode=\"$VERSION_CODE\"/" "$MANIFEST"

aapt package -f -m -J "$OUT/gen" -M "$MANIFEST" -S res -I "$ANDROID_JAR"

javac -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 \
  -bootclasspath "$ANDROID_JAR" -d "$OUT/obj" \
  $(find src "$OUT/gen" -name '*.java')

"$DX" --dex --min-sdk-version=23 --output="$OUT/classes.dex" "$OUT/obj"

aapt package -f -M "$MANIFEST" -S res -I "$ANDROID_JAR" -F "$OUT/unsigned.apk"
(cd "$OUT" && aapt add unsigned.apk classes.dex >/dev/null)

zipalign -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

if [ ! -f "$KEYSTORE" ]; then
  echo "no keystore: creating a local DEBUG key ($KEYSTORE) - do not publish APKs signed with it"
  keytool -genkeypair -keystore "$KEYSTORE" -storepass "$KEYSTORE_PASS" -keypass "$KEY_PASS" -alias "$KEY_ALIAS" \
    -keyalg RSA -keysize 2048 -validity 36500 -dname "CN=rng-lens, O=rengi.dev" >/dev/null 2>&1
fi
apksigner sign --ks "$KEYSTORE" --ks-pass env:KEYSTORE_PASS --key-pass env:KEY_PASS --ks-key-alias "$KEY_ALIAS" \
  --out "$OUT/rng-lens.apk" "$OUT/aligned.apk"
apksigner verify "$OUT/rng-lens.apk"
echo "built $OUT/rng-lens.apk ($(du -h "$OUT/rng-lens.apk" | cut -f1))"
