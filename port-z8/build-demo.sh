#!/bin/bash
cd /mnt/c/Users/ycx/Desktop/Nikon/third-party-20261029/fenestra-z8 || exit 1
rm -rf build/demo-z8
python3 host/build_sdk_module.py modules/demo/sdk.c --id demo --target z8-c311 --output-dir build/demo-z8 2>&1 | tail -30 | grep -vE '^\s+"(src|include|host|modules)/' | cut -c1-240 | head -20
ls -la build/demo-z8/*.wlm | cut -c20-
