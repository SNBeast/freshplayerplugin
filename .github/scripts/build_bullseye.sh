#!/usr/bin/env bash

set -euo pipefail

rm -f /etc/apt/sources.list.d/*

cat << 'EOF' > /etc/apt/sources.list
deb http://archive.debian.org/debian bullseye main contrib non-free
deb http://archive.debian.org/debian-security bullseye-security main contrib non-free
deb http://archive.debian.org/debian bullseye-updates main contrib non-free
EOF

cat << 'EOF' > /etc/apt/apt.conf.d/99archive
Acquire::Check-Valid-Until "false";
Acquire::AllowInsecureRepositories "true";
APT::Get::AllowUnauthenticated "true";
EOF

./.github/scripts/build.sh
