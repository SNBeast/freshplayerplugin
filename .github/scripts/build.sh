#!/usr/bin/env bash

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates curl xz-utils cmake make file binutils pkg-config ragel \
    gcc g++ libc6-dev libasound2-dev libssl-dev libglib2.0-dev \
    libpango1.0-dev libgl1-mesa-dev libevent-dev libgtk2.0-dev libxrandr-dev \
    libxrender-dev libxcursor-dev libv4l-dev libgles2-mesa-dev libva-dev \
    libvdpau-dev libdrm-dev libpulse-dev libjack-jackd2-dev libsoxr-dev

mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_MAIN_LIB=ON -DBUILD_FFMPEG_SHIM=OFF \
    ..
make
mkdir ../artifact
cp libfreshwrapper-flashplayer.so ../artifact/

for spec in \
    "4.4.5:58:56" \
    "5.1.6:59:57" \
    "6.1.2:60:58" \
    "7.1.1:61:59" \
    "8.0:62:60"; do

    IFS=":" read -r FF_VER CODEC_VER UTIL_VER <<< "$spec"
    SDK_DIR="/tmp/sdk_$CODEC_VER"
    BUILD_DIR="build_so_$CODEC_VER"

    ../.github/scripts/build_ffmpeg_shim.sh \
        "$SDK_DIR" \
        "$FF_VER" \
        "libavcodec.so.$CODEC_VER" \
        "libavutil.so.$UTIL_VER"

    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DBUILD_MAIN_LIB=OFF -DBUILD_FFMPEG_SHIM=ON \
        -DAVCODEC_MAJOR="$CODEC_VER" -DFFMPEG_SDK_DIR="$SDK_DIR" \
        ..
    make
    cp "libfreshffmpegwrapper_$CODEC_VER.so" ../artifact/
done

cp /tmp/sdk_62/lib/libpp_interface_resource_stub.so ../artifact/

cd ..
