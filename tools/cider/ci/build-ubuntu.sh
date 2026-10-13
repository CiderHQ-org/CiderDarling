#!/usr/bin/env bash
# CI: build the Ubuntu 24.04 flavor on the runner itself (Debian-family
# hosts with glibc 2.39 or newer). CCACHE_DIR comes from the workflow.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y --no-install-recommends devscripts equivs ccache ninja-build cmake python3 xz-utils \
  xvfb xauth libgl1-mesa-dri libegl1 libgl1
# mk-build-deps leaves files in its working directory: keep the checkout clean.
(cd "${RUNNER_TEMP:-/tmp}" &&
  sudo mk-build-deps -i -r -t "apt-get --no-install-recommends -y" "$GITHUB_WORKSPACE/debian/control")

export CCACHE_BASEDIR=$PWD CCACHE_MAXSIZE=4G CCACHE_COMPILERCHECK=content
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
ccache --zero-stats
python3 tools/cider/package.py --flavor ubuntu24.04 --jobs "$(nproc)" --output dist
ccache --show-stats
# Ubuntu's runner image restricts user namespaces through AppArmor. This
# change is confined to the disposable CI VM; tests still map one build user.
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a --server-args='-screen 0 1280x1024x24 -nolisten tcp -ac' python3 validation/binary-smoke.py \
  --manifest "$(ls dist/ciderdarling-*-x86_64-ubuntu24.04.json)" \
  --output dist/runtime-smoke-ubuntu24.04.json
