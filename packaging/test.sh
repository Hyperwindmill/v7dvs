#!/bin/sh
# Install each package in a fresh container of its own distribution and run
# what can run without hardware. This is what proves the dependency names in
# nfpm.yaml: a wrong one fails here, in apt, dnf or pacman, not on a user.
set -e
cd "$(dirname "$0")/.."

check='echo "-- $(. /etc/os-release; echo $PRETTY_NAME)"
v7dvs --version
v7d --version
pw-jack v7dvs --version
v7dvs-launcher --version
v7dvs-launcher --selftest
python3 /usr/lib/v7dvs/usb/v7audio.py
python3 -c "import usb1, numpy, tkinter; print(\"python deps import ok\")"
test -f /usr/lib/udev/rules.d/99-numark-v7.rules
test -f /usr/lib/modules-load.d/v7dvs.conf
test -f /usr/share/wireplumber/wireplumber.conf.d/50-v7dvs-loopback.conf
test -f /usr/share/applications/v7dvs.desktop
echo "-- ok"'

# One script, set -e first: the install and the checks are one sequence, and a
# failed install must stop it. (An `install && checks` chain would bind only
# the first check line to the &&, and run the rest against nothing.)
run() { docker run --rm -v "$PWD/dist:/dist:ro" "$1" sh -c "set -e
$2
$check"; }

run debian:bookworm 'apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y /dist/v7dvs_*.deb >/dev/null'
run fedora:latest   'dnf -q install -y /dist/v7dvs-*.rpm'
run archlinux:latest 'pacman -Syu --noconfirm >/dev/null && pacman -U --noconfirm /dist/v7dvs-*.pkg.tar.zst >/dev/null'
echo "all packages install and pass"
