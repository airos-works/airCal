#!/usr/bin/env bash
# Cross-compiles airCal on Linux with an air/OS cross SDK (airos-ci
# sdk/build-sdk.sh) and the third-party dependency prefix (libcurl), the way
# the build server does. Produces build-ARCH/airCal.
#
#   tools/build-cross.sh [x86_64|arm64] [make target]
#
# AIROS_SDK defaults to /data1/airos/sdk, AIROS_DEPS to /data1/airos/deps.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ARCH=${1:-x86_64}
SDK=${AIROS_SDK:-/data1/airos/sdk}/$ARCH
DEPS=${AIROS_DEPS:-/data1/airos/deps}/$ARCH/boot/system
[[ -f $SDK/env.sh ]] || { echo "no air/OS SDK at $SDK" >&2; exit 1; }
# shellcheck disable=SC1091
. "$SDK/env.sh"
# The SDK's host tools live in HOST_TOOLS/bin on newer SDKs.
RCBIN=$TOOLS/rc/rc; XRESBIN=$TOOLS/xres; MIMESETBIN=$TOOLS/mimeset
[[ -x $RCBIN ]] || RCBIN=${HOST_TOOLS:-$TOOLS}/bin/rc
[[ -x $XRESBIN ]] || XRESBIN=${HOST_TOOLS:-$TOOLS}/bin/xres
[[ -x $MIMESETBIN ]] || MIMESETBIN=${HOST_TOOLS:-$TOOLS}/bin/mimeset

# A private farm with just libcurl, so -lcurl is unambiguous.
FARM=$ROOT/build-$ARCH/.deps
mkdir -p "$FARM/lib" "$FARM/include"
ln -sf "$(ls "$DEPS"/lib/libcurl.so.4 "$DEPS"/develop/lib/libcurl.so.4 2>/dev/null | head -1)" "$FARM/lib/libcurl.so"
ln -sfn "$DEPS/develop/headers/curl" "$FARM/include/curl"

cd "$ROOT"
exec make BUILD="build-$ARCH" -j"$(nproc)" \
	CXX="${CROSS}g++ --sysroot=$SYSROOT -specs=$UNWIND_SPECS" \
	APP_CPPFLAGS="-I$FARM/include" \
	APP_LDFLAGS="-L$FARM/lib -Wl,-rpath-link,$FARM/lib -Wl,-rpath-link,$DEPS/lib" \
	RC="$RCBIN" XRES="$XRESBIN" MIMESET="$MIMESETBIN --mimedb $MIMEDB" \
	"${2:-all}"
