#!/usr/bin/env bash

set -e

# install the necessary dependencies
sudo apt-get update
sudo apt-get install -y \
    linux-libc-dev \
    build-essential \
    yasm zlib1g-dev \
    libpng-dev

_ARCH=$(arch)

if [ "$_ARCH" = "x86_64" ]; then
    export CFLAGS="-march=x86-64 -mtune=generic"
fi

./configure --enable-static --yasm='' \
  --disable-runtime-cpudetection \
  --disable-x11 --disable-alsa --disable-arts \
  --disable-sdl --disable-vidix --disable-mga --disable-gl \
  --disable-directfb --disable-aa --disable-caca \
  --disable-lirc --disable-tv --disable-radio \
  --disable-dvdread --disable-dvdnav --disable-mencoder \
  --disable-live --enable-fbdev


make -j$(nproc)


# move the built binary to a specific directory
mkdir -p static
cp mplayer static/mplayer
