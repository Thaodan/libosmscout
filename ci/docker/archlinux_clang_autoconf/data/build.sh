#!/bin/sh

if [ $# -ge 1 ] ; then
  REPO="$1"
else
  REPO="git://git.code.sf.net/p/libosmscout/code"
fi

if [ $# -ge 2 ] ; then
  BRANCH="$2"
else
  BRANCH="master"
fi

git clone -b "$BRANCH" "$REPO" libosmscout

cd libosmscout
. ./setupAutoconf.sh
export CC=clang
export CXX=clang++
env
make full

