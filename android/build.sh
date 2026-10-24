#!/usr/bin/env bash
# Builds rng-lens.apk without Gradle: aapt -> javac -> dx -> aapt -> zipalign -> apksigner.
# Needs: JDK, android-23 platform jar, aapt, dx (dalvik-exchange), zipalign, apksigner.
# Ubuntu: apt install android-sdk-platform-23 aapt dalvik-exchange zipalign apksigner
set -euo pipefail
cd "$(dirname "$0")"

ANDROID_JAR="${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}"
DX="${DX:-$(command -v dalvik-exchange || command -v dx)}"
OUT=build
KEYSTORE="${KEYSTORE:-$PWD/rnglens.keystore}"

rm -rf "$OUT"
mkdir -p "$OUT/gen" "$OUT/obj"

aapt package -f -m -J "$OUT/gen" -M AndroidManifest.xml -S res -I "$ANDROID_JAR"

javac -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 \
  -bootclasspath "$ANDROID_JAR" -d "$OUT/obj" \
  $(find src "$OUT/gen" -name '*.java')

"$DX" --dex --min-sdk-version=23 --output="$OUT/classes.dex" "$OUT/obj"

aapt package -f -M AndroidManifest.xml -S res -I "$ANDROID_JAR" -F "$OUT/unsigned.apk"
(cd "$OUT" && aapt add unsigned.apk classes.dex >/dev/null)

zipalign -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass rnglens -keypass rnglens -alias rnglens \
    -keyalg RSA -keysize 2048 -validity 36500 -dname "CN=rng-lens, O=rengi.dev" >/dev/null 2>&1
fi
apksigner sign --ks "$KEYSTORE" --ks-pass pass:rnglens --key-pass pass:rnglens \
  --out "$OUT/rng-lens.apk" "$OUT/aligned.apk"
apksigner verify "$OUT/rng-lens.apk"
echo "built $OUT/rng-lens.apk ($(du -h "$OUT/rng-lens.apk" | cut -f1))"
