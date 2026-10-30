#!/bin/bash
cd /mnt/c/Users/ycx/Desktop/Nikon/third-party-20261029/fenestra-z8 || exit 1
rm -rf build/hello-z8
python3 host/build_sdk_module.py examples/hello/module.c --id hello --target z8-c311 --output-dir build/hello-z8 2>&1 | tail -5 | cut -c1-300
ls -la build/hello-z8 | cut -c20-
