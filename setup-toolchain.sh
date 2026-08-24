#!/bin/sh
# Downloads the prebuilt PSn00bSDK MIPS toolchain + SDK (Linux x86_64).
# macOS: prebuilt toolchains are not published for macOS — build GCC from
# source using the instructions in the PSn00bSDK README
# (https://github.com/Lameguy64/PSn00bSDK), or run this script inside a
# Linux container and point PSN00BSDK_LIBS at the mounted toolchain.
set -e
cd "$(dirname "$0")"
mkdir -p third_party
cd third_party

curl -sLO https://github.com/Lameguy64/PSn00bSDK/releases/download/v0.24/gcc-mipsel-none-elf-12.3.0-linux.zip
curl -sLO https://github.com/Lameguy64/PSn00bSDK/releases/download/v0.24/PSn00bSDK-0.24-Linux.zip

unzip -q gcc-mipsel-none-elf-12.3.0-linux.zip -d tc_extract
unzip -q PSn00bSDK-0.24-Linux.zip -d sdk_extract
cd ..
mv third_party/tc_extract toolchain/gcc-mipsel-none-elf
mv third_party/sdk_extract/PSn00bSDK-0.24-Linux toolchain/psn00bsdksdk
rm -rf third_party/tc_extract third_party/sdk_extract
echo "Toolchain ready. Build with:"
echo "  export PSN00BSDK_LIBS=\$PWD/toolchain/psn00bsdksdk/lib/libpsn00b"
