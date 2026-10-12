#!/usr/bin/env bash
# CI: push the generated ciderdarling-bin recipe to the AUR. Needs the
# maintainer's SSH key in AUR_SSH_PRIVATE_KEY.
set -euo pipefail
dist=${1:?usage: publish-aur.sh DIST}
: "${AUR_SSH_PRIVATE_KEY:?AUR_SSH_PRIVATE_KEY is not set}"

home=$(mktemp -d)
trap 'rm -rf -- "$home"' EXIT
install -m 700 -d "$home/.ssh"
printf '%s\n' "$AUR_SSH_PRIVATE_KEY" > "$home/.ssh/aur"
chmod 600 "$home/.ssh/aur"
ssh-keyscan -t ed25519,rsa aur.archlinux.org > "$home/.ssh/known_hosts" 2>/dev/null
export GIT_SSH_COMMAND="ssh -i $home/.ssh/aur -o UserKnownHostsFile=$home/.ssh/known_hosts -o IdentitiesOnly=yes"

git clone ssh://aur@aur.archlinux.org/ciderdarling-bin.git "$home/aur"
cp -- "$dist/ciderdarling-bin.PKGBUILD" "$home/aur/PKGBUILD"
cp -- "$dist/ciderdarling-bin.SRCINFO" "$home/aur/.SRCINFO"
cp -- "$dist/ciderdarling-bin.install" "$home/aur/ciderdarling-bin.install"
cd "$home/aur"
git add PKGBUILD .SRCINFO ciderdarling-bin.install
if git diff --cached --quiet; then
  echo "The AUR recipe is already current."
  exit 0
fi
version=$(sed -n 's/^\tpkgver = //p' .SRCINFO)-$(sed -n 's/^\tpkgrel = //p' .SRCINFO)
git -c user.name="github-actions[bot]" -c user.email="41898282+github-actions[bot]@users.noreply.github.com" commit -m "Update to $version"
git push origin HEAD:master
