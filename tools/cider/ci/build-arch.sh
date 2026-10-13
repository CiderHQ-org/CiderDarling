#!/usr/bin/env bash
# CI: build the Arch Linux flavor and its ciderdarling-bin recipe inside an
# archlinux:base-devel container (as root), with the checkout at /src and a
# compiler cache at /ccache. HOST_UID/HOST_GID get the outputs back.
set -euo pipefail
cd /src

pacman -Syu --noconfirm --needed \
  cmake ninja clang llvm lld flex bison icu git python pkgconf xz ccache binutils \
  fontconfig freetype2 cairo libtiff giflib libjpeg-turbo libpng mesa glu libglvnd \
  libbsd libelf libxml2 libcap dbus libpulse ffmpeg fuse2 \
  libx11 libxext libxkbfile libxcursor libxkbcommon libxrandr vulkan-headers vulkan-icd-loader \
  xorg-server-xvfb xorg-xauth
git config --global --add safe.directory '*'

export CCACHE_DIR=/ccache CCACHE_BASEDIR=/src CCACHE_MAXSIZE=4G CCACHE_COMPILERCHECK=content
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
ccache --zero-stats
python3 tools/cider/package.py --flavor arch --jobs "$(nproc)" --output dist
ccache --show-stats

# The AUR recipe, checked by building the package from the local archive.
manifest=$(ls dist/ciderdarling-*-x86_64-arch.json)
archive=dist/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["archive"])' "$manifest")
recipe=dist/aur
python3 tools/cider/aur-package.py --manifest "$manifest" --output "$recipe" --resolve-depends
cp -- "$archive" "$recipe/"
id builder >/dev/null 2>&1 || useradd -m builder
chown -R builder: "$recipe"
su builder -c "cd '$recipe' && makepkg --printsrcinfo > .SRCINFO && makepkg -f --nodeps --noconfirm"
package=$(ls "$recipe"/ciderdarling-bin-*.pkg.tar.zst)
bsdtar -tvf "$package" | grep -E '^-rwsr-xr-x .* opt/ciderdarling/[0-9a-f]{40}/bin/darling$' >/dev/null ||
  { echo "The package does not keep bin/darling setuid root" >&2; exit 1; }

# Release assets need unique, visible names.
mv -- "$recipe/PKGBUILD" dist/ciderdarling-bin.PKGBUILD
mv -- "$recipe/.SRCINFO" dist/ciderdarling-bin.SRCINFO
mv -- "$recipe/ciderdarling-bin.install" dist/ciderdarling-bin.install
mv -- "$package" dist/
rm -rf -- "$recipe"
chown -R builder: dist
su builder -c 'cd /src && LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a --server-args="-screen 0 1280x1024x24 -nolisten tcp -ac" python3 validation/binary-smoke.py --manifest "$(ls dist/ciderdarling-*-x86_64-arch.json)" --output dist/runtime-smoke-arch.json'
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" dist /ccache
