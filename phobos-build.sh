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
  # x86-64 need to be generic for old machines
  export CFLAGS="-march=generic64 -mtune=generic64"

  ./configure --enable-static --yasm='' \
    --disable-runtime-cpudetection \
    --disable-x11 --disable-alsa --disable-arts \
    --disable-sdl --disable-vidix --disable-mga --disable-gl \
    --disable-directfb --disable-aa --disable-caca \
    --disable-lirc --disable-tv --disable-radio \
    --disable-dvdread --disable-dvdnav --disable-mencoder \
    --disable-live --enable-fbdev
else
  # normal compile
  ./configure --enable-static --yasm='' \
    --disable-x11 --disable-alsa --disable-arts \
    --disable-sdl --disable-vidix --disable-mga --disable-gl \
    --disable-directfb --disable-aa --disable-caca \
    --disable-lirc --disable-tv --disable-radio \
    --disable-dvdread --disable-dvdnav --disable-mencoder \
    --disable-live --enable-fbdev
fi


make -j$(nproc)


# move the built binary to a specific directory
mkdir -p static
cp mplayer static/mplayer
