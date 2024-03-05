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
mkdir -p /opt/additionalFiles/build/linux/aarch64/mpp-package/etc/systemd/system/
mv /opt/additionalFiles/h264_decode.service /opt/additionalFiles/build/linux/aarch64/mpp-package/etc/systemd/system/
mkdir -p /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin
mv /opt/additionalFiles/Header.h264 /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin/
mv /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin/openhd_vid_test /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin/openhd_vid
chmod +x /opt/additionalFiles/build/linux/aarch64/mpp-package/usr/local/bin/openhd_vid
cd ../../../
VERSION="1.2-$(date -d '+1 hour' +'%m-%d-%Y--%H-%M-%S')"
VERSION=$(echo "$VERSION" | sed 's/\//-/g')
fpm -a arm64 -s dir -t deb -n mpp-rk3566 -v "$VERSION" -C mpp-package -p mpp-rk3566_VERSION_ARCH.deb
echo "push to cloudsmith"
git describe --exact-match HEAD >/dev/null 2>&1
echo "Pushing the package to OpenHD 2.3 repository"
API_KEY=$(cat /opt/additionalFiles/cloudsmith_api_key.txt)
DISTRO=$(cat /opt/additionalFiles/distro.txt)
FLAVOR=$(cat /opt/additionalFiles/flavor.txt)
BOARD=$(cat /opt/additionalFiles/board.txt)

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
