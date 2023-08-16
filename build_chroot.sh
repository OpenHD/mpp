#!/bin/bash
#This file is the install instruction for the CHROOT build
#We're using cloudsmith-cli to upload the file in CHROOT

sudo apt install -y python3-pip
sudo pip3 install --upgrade cloudsmith-cli
ls -a
sudo apt install -y git ruby-dev make cmake gcc g++ wget
gem install fpm
cd build/linux/aarch64
./make-Makefiles.bash
make -j$(nproc)
make DESTDIR=mpp-package -j4 install
cd mpp-package/usr/local
mv lib ../
cd ../
mkdir aarch64-linux-gnu
cd lib
mv * ../aarch64-linux-gnu/
cd ../../../
fpm -a arm64 -s dir -t deb -n mpp -v 0.99 -C mpp-package -p mpp_VERSION_ARCH.deb
echo "copied deb file"
echo "push to cloudsmith"
git describe --exact-match HEAD >/dev/null 2>&1
echo "Pushing the package to OpenHD 2.3 repository"
ls -a
ls -a ../../../
API_KEY=$(cat ../../../cloudsmith_api_key.txt)
DISTRO=$(cat ../../../distro.txt)
FLAVOR=$(cat ../../../flavor.txt)
cloudsmith push deb --api-key "$API_KEY" openhd/openhd-2-3-evo/${DISTRO}/${FLAVOR} *.deb || exit 1

