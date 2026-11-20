#!/bin/bash
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=$HOME/Desktop/edk2/Build/OvmfX64/DEBUG_GCC/FV/OVMF.fd \
  -drive file=fat:rw:$HOME/Desktop/makeos/usb_root,format=raw \
  -m 512M \
  -vnc :0,password=on \
  -monitor stdio

## (qemu) change vnc password 123456