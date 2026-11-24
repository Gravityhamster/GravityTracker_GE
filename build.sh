#!/bin/bash
set -e

CXX=g++
CXXFLAGS="-std=c++20 -O2 -Wall -I./Gravitytracker -I./Gravitytracker/SDL/include -I./Gravitytracker/SDL_TTF/include"
SRCS="Gravitytracker/Gravitytracker.cpp Gravitytracker/TrackerFunctionDefinitions.cpp"
OUT="./Gravitytracker/linux_out/gravitytracker"
LIBS="-lSDL3 -lSDL3_ttf -static-libstdc++ -static-libgcc"

$CXX $CXXFLAGS $SRCS $LIBS -o $OUT
echo "Build complete: $OUT"   