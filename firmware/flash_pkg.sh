#!/bin/bash
# 把 _archive 里的一个完整包（boot2 + partition + app）放回 build/build_out，再等 ISP 口出现就烧。
# 用法：./flash_pkg.sh _archive/R1.3_sdk232_c0fdef3e
set -e
PKG="$1"
[ -f "$PKG/ds5donglere_bl618_bl616.bin" ] || { echo "no package: $PKG"; exit 9; }
cd /e/BL616_Project/DongleRE/firmware
mkdir -p build/build_out
rm -f build/build_out/ds5donglere_bl618_bl616.bin build/build_out/boot2_*.bin build/build_out/partition*.bin
cp "$PKG"/* build/build_out/
echo "[$(date +%T)] package $(basename "$PKG") staged: $(sha256sum build/build_out/ds5donglere_bl618_bl616.bin | cut -c1-16)"
sed -i "s/flash_[a-z0-9]*_out.txt/flash_pkg_out.txt/g" flash_s33_wait.sh
./flash_s33_wait.sh 2>&1 | tail -3
echo "verifications=$(grep -c 'Verification succeeded' flash_pkg_out.txt 2>/dev/null)"
grep "SHA caled by dev" flash_pkg_out.txt | tail -1 | sed 's/.*dev: //' | cut -c1-16
