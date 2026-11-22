#!/usr/bin/env bash

set -euo pipefail

apt-get update
apt-get install -y cmake gcc g++ pkg-config ragel libasound2-dev libssl-dev \
    libglib2.0-dev libpango1.0-dev libgl1-mesa-dev libevent-dev libgtk2.0-dev \
    libxrandr-dev libxrender-dev libxcursor-dev libv4l-dev libgles2-mesa-dev \
    libavcodec-dev libva-dev libvdpau-dev libdrm-dev libicu-dev libpulse-dev \
    libjack-jackd2-dev libsoxr-dev
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make
mkdir ../artifact
cp libfreshwrapper-flashplayer.so ../artifact/
cd ..
