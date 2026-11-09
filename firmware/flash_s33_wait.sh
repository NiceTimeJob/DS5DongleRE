#!/bin/bash
# 等 ISP（按住 BOOT 插入，VID_349B&PID_6160）出现就烧 S33；不发 --reset（这块板发了也回 ISP，还会卡死 CDC）。
FLASH='E:\BL616_Project\ds5dongle-bl618-opensource-3.18\bouffalo_sdk\tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe'
cd /e/BL616_Project/DongleRE/firmware || exit 9
echo "[$(date +%T)] waiting for ISP port (max 1200 s)..."
for i in $(seq 1 1200); do
  COM=$(powershell -NoProfile -Command "\$d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { \$_.InstanceId -like '*VID_349B&PID_6160*' } | Select-Object -First 1; if (\$d) { \$p = Get-PnpDeviceProperty -InstanceId \$d.InstanceId -KeyName 'DEVPKEY_Device_FriendlyName' -ErrorAction SilentlyContinue; if (\$p -and \$p.Data -match '\((COM\d+)\)') { \$Matches[1] } }" 2>/dev/null | tr -d '\r\n')
  if [ -n "$COM" ]; then
    echo "[$(date +%T)] ISP on $COM, flashing..."
    sleep 1
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$FLASH" --interface=uart --baudrate=2000000 --port=$COM --chipname=bl616 --cpu_id= --config=flash_prog_cfg.ini 2>&1 | tee flash_pkg_out.txt | tail -25
    if grep -q "All programming completed successfully" flash_pkg_out.txt; then
      echo "[$(date +%T)] FLASH_OK on $COM"; exit 0
    else
      echo "[$(date +%T)] FLASH_FAIL on $COM"; exit 3
    fi
  fi
  sleep 1
done
echo "[$(date +%T)] TIMEOUT: no ISP port appeared"; exit 2
