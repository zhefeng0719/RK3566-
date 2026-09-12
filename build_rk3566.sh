#!/bin/sh
set -e

cd "$(dirname "$0")"

make clean
make
file bin/onvif_yolo_lcd
