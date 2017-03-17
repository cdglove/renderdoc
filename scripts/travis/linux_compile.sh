#!/bin/sh

# ignore "error" codes in the env script below
set +e

. /opt/qt56/bin/qt56-env.sh 

set -e

if [ $CC == "gcc" ]; then
	export CC=gcc-6;
	export CXX=g++-6;
else
	export CC=clang-3.5;
	export CXX=clang++-3.5;
fi

mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
