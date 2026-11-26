#!/usr/bin/env bash
# joyride's Linux release, built in Ubuntu 22.04 (glibc 2.35, podman): the
# AppImage runs there and on anything newer. tools/appimage.sh, run in the
# container this folder's Containerfile makes (once), with the checkout.
#
#   tools/release-linux/build.sh      -> build-release-linux/joyride-<version>-x86_64.AppImage
#
# The shaders are compiled to C headers here first: Ubuntu 22.04 has no
# glslc, and the headers are the same wherever they are made.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
image=joyride-release-linux
out="$root/build-release-linux"

podman image exists "$image" || podman build -t "$image" -f "$root/tools/release-linux/Containerfile" "$root/tools"

mkdir -p "$out/shaders"
for s in fullscreen.vert texfilter.frag hwquad.vert hwquad.frag composite.frag map.frag; do
    glslc -O -mfmt=c -o "$out/shaders/$s.h" "$root/shaders/$s"
done

podman run --rm -v "$root":/src:Z -w /src \
    -e JOYRIDE_PREBUILT_SHADERS=/src/build-release-linux/shaders \
    -e JOYRIDE_PYTHON_HOME=/opt/python \
    -e QMAKE=/opt/qt/6.8.3/gcc_64/bin/qmake \
    -e LINUXDEPLOY=/opt/linuxdeploy/linuxdeploy-x86_64.AppImage \
    "$image" tools/appimage.sh /src/build-release-linux
