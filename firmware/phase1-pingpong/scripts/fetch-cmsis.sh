#!/usr/bin/env bash
# Fetch the only external code this firmware uses: ST's CMSIS device headers for the STM32U5
# and the Arm CMSIS core headers. Pinned to the commits this project was built and checked with.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party

fetch() {
    local url=$1 dir=$2 rev=$3
    if [ ! -d "third_party/$dir/.git" ]; then
        git clone --quiet "$url" "third_party/$dir"
    fi
    git -C "third_party/$dir" fetch --quiet origin
    git -C "third_party/$dir" checkout --quiet "$rev"
    echo "third_party/$dir @ $(git -C "third_party/$dir" rev-parse --short HEAD)"
}

fetch https://github.com/STMicroelectronics/cmsis-device-u5 cmsis-device-u5 624374fa1e21ca195d6f2102ac0caaa50d0ea4c8
fetch https://github.com/STMicroelectronics/cmsis_core      cmsis_core      01d18147527e30ed69ace1cb03b818de78bc7062
