#!/usr/bin/env bash
# Build the Digimon World APK.
#
#   android/build_apk.sh                  # arm64 phone build -> android/out/DigimonWorld.apk
#   ABIS="arm64-v8a x86_64" android/build_apk.sh   # also include the emulator ABI
#
# Steps: compile the runtime for Android with the NDK (libmain.so + libSDL3.so),
# compile SDL's Java glue + the app's activities, and pack both with the
# support files the app installs on first launch (game.toml, open-source BIOS,
# mods, the headers the translated game is compiled against).
#
# The APK contains nothing from the game: no disc image and no recompiled code.
# The app asks for the player's own disc, translates it on the phone with the
# bundled recompiler, and compiles that with the bundled TinyCC at startup.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
RECOMP="$ROOT/digimon-recomp"
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="${ANDROID_NDK:-$SDK/ndk/28.2.13676358}"
BUILD_TOOLS="$SDK/build-tools/35.0.1"
ANDROID_JAR="$SDK/platforms/android-35/android.jar"
CMAKE="$SDK/cmake/3.31.6/bin/cmake"
NINJA="$SDK/cmake/3.31.6/bin/ninja"
GLSLC="$NDK/shader-tools/linux-x86_64/glslc"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
ABIS="${ABIS:-arm64-v8a}"
MIN_SDK=31
TARGET_SDK=35
# ON adds the runtime's TCP debug server (port 4370, reach it with
# "adb forward tcp:4370 tcp:4370") -- for testing, not for playing.
DEBUG_TOOLS="${DEBUG_TOOLS:-OFF}"
OUT="$HERE/out"
APK="${APK:-$OUT/DigimonWorld.apk}"

if [[ ! -f "$RECOMP/psxrecomp/generated/OpenBIOS_full.c" ]]; then
    echo "error: the OpenBIOS backend has not been generated yet (see README.md)" >&2
    exit 1
fi

# ---- 1. native libraries ---------------------------------------------------
for ABI in $ABIS; do
    BUILD="$RECOMP/build-android-${ABI%%-*}"   # arm64-v8a -> build-android-arm64
    # Re-running the configure step is cheap and keeps options like
    # DEBUG_TOOLS in step with this invocation.
    "$CMAKE" -S "$RECOMP" -B "$BUILD" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$MIN_SDK" \
        -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release \
        -DPSX_RECOMP_UI=OFF \
        -DPSX_DEBUG_TOOLS="$DEBUG_TOOLS" \
        -DPSX_DYNAMIC_GAME=ON \
        -DPSX_UI_OVERLAY_DIR="$HERE/native" \
        -DGLSLC_EXE="$GLSLC" >/dev/null
    "$CMAKE" --build "$BUILD" --target psx-runtime -j"$(nproc)"
done

# ---- 2. Java: SDL's activity glue + DigimonActivity -------------------------
SDL_SRC="$(find "$RECOMP" -maxdepth 4 -type d -path '*/_deps/sdl3-src' | head -n1)"
if [[ -z "$SDL_SRC" ]]; then
    echo "error: SDL3 source not found under $RECOMP/build-*/_deps" >&2
    exit 1
fi
rm -rf "$OUT/java" && mkdir -p "$OUT/java/classes" "$OUT/java/dex" "$OUT/java/gen"

# ---- resources + manifest (also generates R.java for step 2b) ---------------
rm -rf "$OUT/res" && mkdir -p "$OUT/res"
"$BUILD_TOOLS/aapt2" compile --dir "$HERE/res" -o "$OUT/res/compiled.zip"
VERSION_CODE="${VERSION_CODE:-$(( $(date +%s) / 60 - 29000000 ))}"
VERSION_NAME="${VERSION_NAME:-1.1.$VERSION_CODE}"
PERMS=""
if [[ "$DEBUG_TOOLS" == "ON" ]]; then
    PERMS='<uses-permission android:name="android.permission.INTERNET" />'
fi
sed "s|<!--@DEBUG_PERMISSIONS@-->|$PERMS|" "$HERE/AndroidManifest.xml" > "$OUT/res/AndroidManifest.xml"
"$BUILD_TOOLS/aapt2" link -o "$OUT/res/base.apk" -I "$ANDROID_JAR" \
    --manifest "$OUT/res/AndroidManifest.xml" \
    --min-sdk-version "$MIN_SDK" --target-sdk-version "$TARGET_SDK" \
    --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
    --java "$OUT/java/gen" \
    "$OUT/res/compiled.zip"

# ---- 2b. compile ---------------------------------------------------------------
# javac argument files split on whitespace, so quote each path (this tree
# lives under "Digimon World APK").
find "$SDL_SRC/android-project/app/src/main/java" "$HERE/src" "$OUT/java/gen" -name '*.java' \
    | sed 's/.*/"&"/' > "$OUT/java/sources.txt"
javac -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 \
    -bootclasspath "$ANDROID_JAR" -d "$OUT/java/classes" @"$OUT/java/sources.txt"
jar cf "$OUT/java/classes.jar" -C "$OUT/java/classes" .
"$BUILD_TOOLS/d8" --release --min-api "$MIN_SDK" --lib "$ANDROID_JAR" \
    --output "$OUT/java/dex" "$OUT/java/classes.jar"

# ---- 4. support files (installed by PayloadInstaller on first launch) --------
FIRST_ABI="${ABIS%% *}"
FIRST_BUILD="$RECOMP/build-android-${FIRST_ABI%%-*}"
PAYLOAD="$OUT/payload"
rm -rf "$PAYLOAD" && mkdir -p "$PAYLOAD/bios" "$PAYLOAD/seeds"
cp "$HERE/game.android.toml" "$PAYLOAD/game.toml"
cp "$RECOMP/cheats.txt" "$PAYLOAD/cheats.txt"
cp "$FIRST_BUILD/psx_game_version.txt" "$PAYLOAD/"
cp "$FIRST_BUILD/bios/openbios.bin" "$FIRST_BUILD/bios/OpenBIOS.LICENSE" "$PAYLOAD/bios/"
cp "$RECOMP/psxrecomp/bios/SCPH1001.toml" "$PAYLOAD/bios/"    # recompiler's BIOS profile
cp "$RECOMP/seeds/ghidra_funcs.txt" "$PAYLOAD/seeds/"          # function entry seeds
cp -r "$FIRST_BUILD/mods" "$PAYLOAD/mods"
cp "$HERE/mods.state.default.toml" "$PAYLOAD/mods/state.toml"   # installed once, then the player's
cp -r "$FIRST_BUILD/tcc" "$PAYLOAD/tcc"                         # headers for the startup compile

# ---- 5. native libs, stripped ---------------------------------------------------
rm -rf "$OUT/lib"
for ABI in $ABIS; do
    BUILD="$RECOMP/build-android-${ABI%%-*}"   # arm64-v8a -> build-android-arm64
    mkdir -p "$OUT/lib/$ABI"
    "$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/libmain.so" "$BUILD/libmain.so"
    "$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/libSDL3.so" "$BUILD/_deps/sdl3-build/libSDL3.so"
done

# ---- 6. assemble, align, sign ---------------------------------------------------
python3 "$HERE/tools/assemble_apk.py" \
    --base "$OUT/res/base.apk" --dex-dir "$OUT/java/dex" \
    --lib-dir "$OUT/lib" --payload "$PAYLOAD" --out "$OUT/unsigned.apk"
# Nothing from the game may ship: no disc image, no recompiled code.
if unzip -l "$OUT/unsigned.apk" | grep -qiE '\.(bin|cue|chd)$|generated/|SLUS_'; then
    if unzip -l "$OUT/unsigned.apk" | grep -iE '\.(bin|cue|chd)$|generated/|SLUS_' \
            | grep -vE 'bios/openbios\.bin$'; then
        echo "error: game data found in the APK (listed above)" >&2
        exit 1
    fi
fi
for ABI in $ABIS; do
    if "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D --defined-only \
            "$OUT/lib/$ABI/libmain.so" | grep -q ' func_8'; then
        echo "error: libmain.so ($ABI) contains recompiled game functions" >&2
        exit 1
    fi
done
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

KEYSTORE="$HERE/keystore/digimon-debug.keystore"
if [[ ! -f "$KEYSTORE" ]]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias digimon -keyalg RSA -keysize 2048 -validity 36500 \
        -dname "CN=Digimon World Recompiled, O=personal build" >/dev/null
fi
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
    --key-pass pass:android --ks-key-alias digimon --out "$APK" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" verify "$APK"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo
echo "APK: $APK ($(du -h "$APK" | cut -f1), versionCode $VERSION_CODE, ABIs: $ABIS)"
