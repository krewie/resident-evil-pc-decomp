#!/usr/bin/env bash
set -e

cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build/linux -j
