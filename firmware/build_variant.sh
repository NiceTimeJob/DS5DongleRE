#!/bin/bash
# 编一个变体并归档成可直接烧的完整包：_archive/<版本>_<变体>_<sha8>/（boot2 + partition + app）。
# 用法：./build_variant.sh sdk232     上游 v2.3.32 + 新蓝牙控制器库 1.6.199（R1.14 起 0xF8 不带后缀：用户 2026-10-15 把 sdk232 后缀去掉了，这条就是发布线）
#       ./build_variant.sh sdk2328    现用 SDK 2.3.28 fork（控制器库 1.6.182）
# 烧：./flash_pkg.sh _archive/<包目录>
set -e
cd /e/BL616_Project/DongleRE/firmware
VAR="$1"
VER=$(sed -n 's/^#define FW_VERSION *"\([^"]*\)".*/\1/p' src/version.h)
case "$VAR" in
  sdk232)  SDK='E:\BL616_Project\sdk_v2.3.32'; TAG= ;;
  sdk2328) SDK='E:\BL616_Project\ds5dongle-bl618-opensource-3.18\bouffalo_sdk'; TAG= ;;
  *) echo "usage: $0 sdk232|sdk2328"; exit 9 ;;
esac
[ -n "$VER" ] || { echo "version.h 里读不到 FW_VERSION"; exit 9; }
LOG="build_${VER}_${VAR}.log"
rm -rf build
env -u SHELL MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' FW_SDK_TAG="$TAG" BL_SDK_BASE="$SDK" \
    TOOLCHAIN_PATH='E:\BL616_Project\ds5dongle-bl618-opensource-3.18\toolchain_gcc_t-head_windows' \
    PATH='/e/BL616_Project/tools_bin:/c/Windows/System32:/c/Windows' \
    cmd.exe /C 'E:\BL616_Project\DongleRE\firmware\build_windows.bat rebuild' > "$LOG" 2>&1 || true
if ! grep -q "Built target combine" "$LOG" || [ ! -f build/build_out/ds5donglere_bl618_bl616.bin ]; then
    echo "BUILD FAILED ($LOG):"
    grep -nE "error|Error|undefined reference" "$LOG" | head -20
    exit 1
fi
grep -nE "warning:" "$LOG" | grep -E "main\.c|bt_hid_host\.c|perf\.c|ds5map\.c|gyro\.c" | head -10 || true
APP=build/build_out/ds5donglere_bl618_bl616.bin
SHA=$(sha256sum "$APP" | cut -c1-8)
PKG="_archive/${VER}_${VAR}_${SHA}"
mkdir -p "$PKG"
cp "$APP" build/build_out/boot2_*.bin build/build_out/partition*.bin "$PKG"/
VSTR=$(grep -aoE "(LCT616|BL616)-DongleRE R[0-9]+(\.[0-9]+)?( HS)?( sdk[0-9]+)?" "$APP" | head -1)
echo "BUILD OK: $PKG  size=$(stat -c %s "$APP")  version='$VSTR'"
