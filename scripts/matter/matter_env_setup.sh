#!/bin/bash

PWD_ORG=`pwd`
cd ../modules/lib/matter

# Clone just necessary submodules
echo "Update needed Submodules for matter."
git submodule update --init third_party/nlunit-test/repo
git submodule update --init third_party/nlio/repo
git submodule update --init third_party/nlassert/repo
git submodule update --init third_party/jsoncpp/repo
git submodule update --init third_party/perfetto/repo
git submodule update --init third_party/libwebsockets/repo
git submodule update --init third_party/editline/repo

# Create a Matter virtual environment and tools
source scripts/bootstrap.sh

# Upgrade PIP to Matter own virtual env
pip install --upgrade pip
# Install Requirement PIP packets
pip install -r $PWD_ORG/scripts/requirements-matter.txt

# Build a chip host tools
echo "Build a Host tools"
# Clean old build system
rm -rf out/host
gn gen out/host --args='chip_enable_wifi=false'
ninja -C out/host

# Add a host tool to Path
echo "Add to host tools to path:"
HOST_PATH=`pwd`
HOST_PATH=$HOST_PATH/out/host
echo $HOST_PATH
export PATH=$HOST_PATH:$PATH

cd $PWD_ORG
