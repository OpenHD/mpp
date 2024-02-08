#!/bin/bash
# This file is the install instruction for the CHROOT build
# We're using cloudsmith-cli to upload the file in CHROOT
sudo apt install -y python3-pip
sudo pip3 install --upgrade cloudsmith-cli
curl -1sLf 'https://dl.cloudsmith.io/public/openhd/release/setup.deb.sh'| sudo -E bash
apt update
sudo apt install -y git ruby-dev curl make cmake gcc g++ wget libdrm-dev mlocate openhd qopenhd-rk3566 apt-transport-https apt-utils open-hd-web-ui
gem install fpm
cd build/linux/aarch64
./make-Makefiles.bash
make -j$(nproc)
make DESTDIR=mpp-package -j4 install
cd mpp-package/usr/local
mv lib ../
cd ../
mkdir -p aarch64-linux-gnu
cd lib
mv * ../aarch64-linux-gnu/
mv ../aarch64-linux-gnu ../lib/aarch64-linux-gnu
mkdir -p /opt/additionalFiles/build/linux/aarch64/mpp-package/etc/system/systemd
mv /opt/additionalFiles/h264_decode.service /opt/additionalFiles/build/linux/aarch64/mpp-package/etc/system/systemd/
mkdir -p /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin
mv /opt/additionalFiles/Header.h264 /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin/
cd ../../../
ls -a
exit 1
VERSION="1.1-$(date +'%m/%d/%Y')"
fpm -a arm64 -s dir -t deb -n mpp-rk3566 -v "$VERSION" -C /opt/additionalFiles/build/linux/aarch64/mpp-package/ -p mpp-rk3566_VERSION_ARCH.debecho "copied deb file"
echo "push to cloudsmith"
git describe --exact-match HEAD >/dev/null 2>&1
echo "Pushing the package to OpenHD 2.3 repository"
ls -a
ls -a ../../../
API_KEY=$(cat ../../../cloudsmith_api_key.txt)
DISTRO=$(cat ../../../distro.txt)
FLAVOR=$(cat ../../../flavor.txt)
BOARD=$(cat ../../../board.txt)

if [ "$BOARD" = "rk3588" ]; then
    for file in *.deb; do
        mv "$file" "${file%.deb}-rk3588.deb"
    done
    cloudsmith push deb --api-key "$API_KEY" openhd/dev-release/${DISTRO}/${FLAVOR} *.deb || exit 1
else
for file in *.deb; do
        mv "$file" "${file%.deb}-rk3566.deb"
    done
    cloudsmith push deb --api-key "$API_KEY" openhd/dev-release/${DISTRO}/${FLAVOR} *.deb || exit 1
fi
