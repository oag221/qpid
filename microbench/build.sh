#!/bin/bash

if [ ! -d "build" ]; then
    echo "'build' directory does not exist. Creating it now..."
    # Create the directory (including parent directories if needed)
    mkdir -p "build"
    echo "Directory created successfully."
fi

cd build

if [[ -n "$1" && "$1" == "-f" ]]; then
    rm -rf CMakeCache.txt CMakeFiles/
fi

# generate build files
cmake -DCMAKE_CXX_COMPILER=g++ ..

# compile
cmake --build .
