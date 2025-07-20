#!/usr/bin/env bash

set -e

# install the necessary dependencies
sudo apt-get update
sudo apt-get install -y \
    linux-libc-dev \
    build-essential \
    yasm zlib1g-dev \
    libpng-dev

./configure --enable-static --yasm='' \
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
