#!/usr/bin/env bash

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates curl xz-utils cmake make file binutils pkg-config ragel \
    git gcc g++ libc6-dev libasound2-dev libssl-dev libglib2.0-dev \
    libpango1.0-dev libgl1-mesa-dev libevent-dev libgtk2.0-dev libxrandr-dev \
    libxrender-dev libxcursor-dev libv4l-dev libgles2-mesa-dev libva-dev \
    libvdpau-dev libdrm-dev libpulse-dev libjack-jackd2-dev libsoxr-dev \
    libavcodec-dev

mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make
mkdir ../artifact
cp libfreshwrapper-flashplayer.so ../artifact/

cd ..
