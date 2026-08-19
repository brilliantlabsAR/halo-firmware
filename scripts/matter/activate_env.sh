#!/bin/bash

PWD_ORG=`pwd`

cd ../modules/lib/matter
# Activate Matter virtual Environment
source scripts/activate.sh

echo "Build a Host tools"

# Build without wifi interface. This is because of lack of Global Ipv6 at wifi interface.
gn gen out/host --args='chip_enable_wifi=false'
ninja -C out/host

HOST_PATH=`pwd`
HOST_PATH=$HOST_PATH/out/host

echo "Add to host tools to path:"
echo $HOST_PATH
export PATH=$HOST_PATH:$PATH

# go back original folder
cd $PWD_ORG
