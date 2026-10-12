#!/usr/bin/env bash
# CI: publish the built archives as the GitHub release bin-<revision prefix>.
# An existing release is only replaced with REPLACE=true: its archives may
# already be pinned by checksum (ciderdarling-bin, Cider).
set -euo pipefail
dist=${1:?usage: publish-release.sh DIST}
revision=${GITHUB_SHA:?}
tag=bin-${revision:0:12}
shopt -s nullglob
assets=("$dist"/*)
((${#assets[@]})) || { echo "Nothing to publish in $dist" >&2; exit 1; }

if gh release view "$tag" >/dev/null 2>&1; then
  if [[ ${REPLACE:-false} != true ]]; then
    echo "::notice::Release $tag already exists; kept (rerun with replace to overwrite)."
    exit 0
  fi
  gh release upload "$tag" "${assets[@]}" --clobber
  exit 0
fi

notes=$(mktemp)
{
  echo "Prebuilt CiderDarling for \`$revision\`, built by CI from this commit."
  echo
  echo "| Flavor | Archive | Built on | Needs glibc |"
  echo "| --- | --- | --- | --- |"
  for manifest in "$dist"/ciderdarling-*-x86_64-*.json; do
    python3 - "$manifest" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
print(f"| {m['flavor']} | `{m['archive']}` ({m['archive_bytes'] / 1048576:.0f} MiB) | {m['build_host']} | {m['min_glibc']}+ |")
PY
  done
  echo
  echo "Each archive unpacks to \`/opt/ciderdarling/$revision\`, where Cider finds it:"
  echo
  echo '```sh'
  echo "python3 tools/cider/install-binary.py ciderdarling-$revision-x86_64-<flavor>.tar.xz"
  echo '```'
  echo
  echo "Keep the \`.json\` manifest next to the archive. Darling's bridges to host libraries name the"
  echo "build host's library versions, so pick the flavor of your distribution family. On Arch Linux,"
  echo "install \`ciderdarling-bin\` from the AUR or \`pacman -U\` the attached package."
} > "$notes"
gh release create "$tag" "${assets[@]}" --target "$revision" \
  --title "CiderDarling ${revision:0:12} binaries" --notes-file "$notes"
