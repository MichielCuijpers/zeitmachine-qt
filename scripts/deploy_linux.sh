#!/bin/bash

SCRIPTS=$PWD
BUILD_DIR="$SCRIPTS/../../build-zeitmachine-Desktop_Qt_5_6_0_GCC_64bit-Debug/"
QT_LIB_DIR="/home/simonrepp/Qt/5.6/gcc_64/lib"
FFMPEG_LIB_DIR="/home/simonrepp/zeitdice/zeitmachine/dependencies/installed/ffmpeg-3.0.1/lib"
X264_LIB_DIR="/home/simonrepp/zeitdice/zeitmachine/dependencies/installed/x264-snapshot-20141218-2245-stable/lib"

cp $SCRIPTS/zeitmachine.sh $BUILD_DIR

cp /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.21 $BUILD_DIR
cp /lib/x86_64-linux-gnu/libgcc_s.so.1 $BUILD_DIR
cp /lib/x86_64-linux-gnu/libc.so.6 $BUILD_DIR

cp $QT_LIB_DIR/libQt5OpenGL.so.5 $BUILD_DIR
cp $QT_LIB_DIR/libQt5Widgets.so.5 $BUILD_DIR
cp $QT_LIB_DIR/libQt5Gui.so.5 $BUILD_DIR
cp $QT_LIB_DIR/libQt5Core.so.5 $BUILD_DIR

cp $QT_LIB_DIR/libicui18n.so.56 $BUILD_DIR
cp $QT_LIB_DIR/libicuuc.so.56 $BUILD_DIR
cp $QT_LIB_DIR/libicudata.so.56 $BUILD_DIR

cp $FFMPEG_LIB_DIR/libavfilter.so.6 $BUILD_DIR
cp $FFMPEG_LIB_DIR/libavformat.so.57 $BUILD_DIR
cp $FFMPEG_LIB_DIR/libavcodec.so.57 $BUILD_DIR
cp $FFMPEG_LIB_DIR/libswscale.so.4 $BUILD_DIR
cp $FFMPEG_LIB_DIR/libavutil.so.55 $BUILD_DIR

cp $X264_LIB_DIR/libx264.so.142 $BUILD_DIR
