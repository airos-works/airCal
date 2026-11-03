#!/usr/bin/env bash
# Starts a throwaway Haiku test VM for airCal (VNC 127.0.0.1:5914, SSH 2235).
#
# It boots TasAmp's Haiku R1/beta6 disk (apps/tasamp/.vm/work.qcow2, override
# with AIRCAL_VM_BASE) read-only through a qcow2 overlay in AIRCAL_VM_DIR
# (default /mnt/HaikuWork/tmp/aircal-vm) and uses TasAmp's SSH key. The host
# is 10.0.2.2 inside the VM: tools/fake-cloud.py (port 18480) and
# tools/caldav-server.sh (port 5232) are reachable there.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
TASAMP=$(cd "$ROOT/.." && pwd)/tasamp/.vm
BASE=${AIRCAL_VM_BASE:-$TASAMP/work.qcow2}
DIR=${AIRCAL_VM_DIR:-/mnt/HaikuWork/tmp/aircal-vm}
OVERLAY=$DIR/overlay.qcow2
STAMP=$DIR/base.stamp
mkdir -p "$DIR"
if [[ -f $DIR/qemu.pid ]] && kill -0 "$(cat "$DIR/qemu.pid")" 2>/dev/null; then
	echo 'The airCal test VM is already running.'
	exit 0
fi
stamp=$(stat -c '%s %Y' "$BASE")
if [[ -f $OVERLAY && "$(cat "$STAMP" 2>/dev/null)" != "$stamp" ]]; then
	echo 'Base disk changed; starting from a fresh overlay.'
	rm -f "$OVERLAY"
fi
if [[ ! -f $OVERLAY ]]; then
	qemu-img create -q -f qcow2 -b "$BASE" -F qcow2 "$OVERLAY"
	echo "$stamp" > "$STAMP"
fi
[[ -f $DIR/id_ed25519 ]] || cp "$TASAMP/id_ed25519" "$DIR/id_ed25519"
chmod 600 "$DIR/id_ed25519"
rm -f "$DIR/qmp.sock"
exec qemu-system-x86_64 -enable-kvm -cpu host -m "${AIRCAL_VM_MEMORY:-4096}" \
	-smp "${AIRCAL_VM_CPUS:-4}" \
	-drive file="$OVERLAY",format=qcow2,if=ide,index=0 \
	-nic user,model=e1000,hostfwd=tcp:127.0.0.1:2235-:22 \
	-device qemu-xhci -device usb-tablet -vga std -display none \
	-vnc 127.0.0.1:14 -qmp unix:"$DIR/qmp.sock",server=on,wait=off \
	-pidfile "$DIR/qemu.pid" -serial file:"$DIR/serial.log" -daemonize
