#!/bin/bash
# install_dependencies.sh

if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root" 
   exit 1
fi

echo "Installing build dependencies for Raspberry Pi OS (Trixie)..."

apt update
apt install -y \
    build-essential \
    pkg-config \
    libsdl3-dev \
    libsdl3-image-dev \
    libsdl3-ttf-dev \
    liblo-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    fonts-dejavu-core

echo "Dependencies installed."