#!/bin/bash

## Before running this build script, you have to pull the tianocore/edk2 repository first
## pull all submodules
## Edit MdeModulePkg.dsc, add MdeModulePkg/Application/BootLoader/BootLoader.inf under [Component] section

## Check whether env car EDK2_DIR exists
if [[ -n "$EDK2_DIR" ]]; then
    echo "EDK2_DIR: $MY_VAR"
else
    echo "EDK2_DIR is required"
    exit 1
fi

## Make directory
mkdir -p $EDK2_DIR/MdeModulePkg/Application/BootLoader

## Copy the source file to EDK2 
cp ./bootloader/BootLoader.c $EDK2_DIR/MdeModulePkg/Application/BootLoader/BootLoader.c
cp ./bootloader/BootLoader.inf $EDK2_DIR/MdeModulePkg/Application/BootLoader/BootLoader.inf

## Run . edksetup.sh 
cd $EDK2_DIR
pwd
. ./edksetup.sh

## Run build
build