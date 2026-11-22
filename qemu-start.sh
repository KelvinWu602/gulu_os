#!/bin/bash

# 1. 先清理殘留嘅 QEMU 進程（加 || true 避免無進程時報錯或中斷）
pkill -f -9 qemu-system-x86_64 2>/dev/null || true

# 2. 解析命令列參數：傳入 -d 或 --debug 先至開啟 GDB
GDB_OPTS=()
for arg in "$@"; do
  case "$arg" in
    -d|--debug)
      GDB_OPTS=(-gdb tcp:0.0.0.0:1234 -S)
      echo "[INFO] Debug mode enabled: listening on 0.0.0.0:1234 (CPU paused)"
      ;;
    -h|--help)
      echo "Usage: $0 [-d|--debug]"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg"
      echo "Usage: $0 [-d|--debug]"
      exit 1
      ;;
  esac
done

# 3. 啟動 QEMU
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=$HOME/Desktop/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF.fd \
  -drive file=fat:rw:$HOME/Desktop/makeos/usb_root,format=raw \
  -m 512M \
  -object secret,id=vncsec,data=123 \
  -vnc :0,password-secret=vncsec \
  -monitor stdio \
  "${GDB_OPTS[@]}"