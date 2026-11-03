#!/usr/bin/env bash
# Copies the cross build into the airCal test VM and (re)starts it there,
# pointed at tools/fake-cloud.py on the host. airCal's output goes to
# /boot/home/aircal-test/aircal.log. Extra arguments are environment
# settings, e.g. AIRCAL_SETTINGS_DIR=/boot/home/aircal-test/data2
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
VM="$ROOT/tools/vm/haiku.sh"
"$VM" 'hey airCal quit > /dev/null 2>&1; for i in 1 2 3 4 5 6 7 8 9 10; do ps | grep -q "[a]irCal" || break; sleep 0.5; done; kill -9 airCal 2>/dev/null; mkdir -p /boot/home/aircal-test'
"$VM" 'cat > /boot/home/aircal-test/airCal' < "$ROOT/build-x86_64/airCal"
"$VM" "cd /boot/home/aircal-test && chmod 755 airCal && mimeset -f airCal && (env AIRCAL_CLOUD_BASE=http://10.0.2.2:18480 AIRCAL_SETTINGS_DIR=/boot/home/aircal-test/data $* ./airCal > aircal.log 2>&1 &) ; sleep 4; cat aircal.log"
