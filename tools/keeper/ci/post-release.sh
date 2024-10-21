#!/bin/bash
set -e
ci=$(realpath $(dirname $0))
v=$1
if [ "$v" = "" ]; then
  echo "$0 <version>"
  exit 1
fi

newv=$(awk -F. '/[0-9]+\./{$NF+=1;print}' OFS=. <<<"$v")
tee version/version.go <<EOF
/* This file is autogenerated by GitHub Actions, do not change it manually. */
package version

var Version = "$newv-alpha"
EOF

git config user.name github-actions
git config user.email github-actions@github.com
git add version/version.go
git commit -m "chore: start next dev iteration $newv-alpha"
git push