#!/bin/bash

## Check whether env car EDK2_DIR exists
if [[ -n "$EDK2_DIR" ]]; then
    echo "EDK2_DIR: $EDK2_DIR"
else
    echo "EDK2_DIR is required"
    exit 1
fi

cp $EDK2_DIR/Build/MdeModule/DEBUG_GCC/X64/MdeModulePkg/Application/BootLoader/BootLoader/DEBUG/BootLoader.efi ./usb_root/EFI/BOOT/BootLoader.efi