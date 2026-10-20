#!/bin/bash
# CI: build and smoke-test zchangelog inside a Debian 10 (kernel 4.19)
# VM, exercising the inotify backend on the oldest supported kernels.
set -e
WS="$PWD"
cd /home/runner/ktest 2>/dev/null || { mkdir -p /home/runner/ktest; cd /home/runner/ktest; }
echo "=== image ==="
[ -s d10.qcow2 ] || curl -fsSL -o d10.qcow2 https://cloud.debian.org/images/cloud/buster/latest/debian-10-genericcloud-amd64.qcow2
echo "=== seed ==="
rm -f /tmp/cikey
ssh-keygen -q -t ed25519 -N '' -f /tmp/cikey
PUB=$(cat /tmp/cikey.pub)
cat > /tmp/ci-user-data <<EOFUD
#cloud-config
users:
  - default
  - name: ci
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: true
    ssh_authorized_keys:
      - $PUB
EOFUD
printf "instance-id: zclci01\nlocal-hostname: zclci\n" > /tmp/ci-meta-data
cloud-localds /tmp/ci-seed.iso /tmp/ci-user-data /tmp/ci-meta-data
echo "=== boot ==="
pkill -f "drive file=d10.qcow2" 2>/dev/null || true
sleep 1
qemu-system-x86_64 -name zclci -m 1024 -smp 2 -machine accel=tcg \
  -drive file=d10.qcow2,if=virtio,format=qcow2 \
  -drive file=/tmp/ci-seed.iso,if=virtio,media=cdrom \
  -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 \
  -display none -serial file:serial.log -daemonize -pidfile /tmp/zclci.pid
trap 'kill $(cat /tmp/zclci.pid) 2>/dev/null || true' EXIT
up=0
# TCG boots slowly on the two core runners; allow up to 25 minutes.
for i in $(seq 1 75); do
  if ssh -i /tmp/cikey -o IdentitiesOnly=yes -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 ci@127.0.0.1 true 2>/dev/null; then up=1; break; fi
  if ! pgrep -f "qemu-system.*zclci" > /dev/null; then break; fi
  sleep 20
done
if [ $up -ne 1 ]; then
  echo "ERROR: ssh wait failed; serial console tail:"
  tail -20 serial.log 2>/dev/null || true
  exit 1
fi
echo "=== transfer ==="
git -C "$WS" archive HEAD | gzip > /tmp/zcl-src.tgz
scp -i /tmp/cikey -o IdentitiesOnly=yes -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /tmp/zcl-src.tgz ci@127.0.0.1:/tmp/
echo "=== in-VM: apt over the buster archive ==="
ssh -i /tmp/cikey -o IdentitiesOnly=yes -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ci@127.0.0.1 'sudo bash -c "
echo \"deb [trusted=yes] http://archive.debian.org/debian buster main contrib non-free\" > /etc/apt/sources.list
echo \"deb [trusted=yes] http://archive.debian.org/debian buster-updates main contrib non-free\" >> /etc/apt/sources.list
echo \"deb [trusted=yes] http://archive.debian.org/debian-security buster/updates main contrib non-free\" >> /etc/apt/sources.list
apt-get update -o Acquire::Check-Valid-Until=false > /dev/null 2>&1
for p in libsqlite3-dev gcc make autoconf automake pkg-config ksh; do
  DEBIAN_FRONTEND=noninteractive apt-get install -y -q \$p > /tmp/apt.log 2>&1 || { echo \"APT FAIL: \$p\"; tail -6 /tmp/apt.log; }
done
uname -r
"'
echo "=== in-VM: build ==="
ssh -i /tmp/cikey -o IdentitiesOnly=yes -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ci@127.0.0.1 'cd /tmp && rm -rf zcl && mkdir zcl && tar xzf zcl-src.tgz -C zcl && cd zcl
./autogen.sh > autogen.log 2>&1 || { echo "AUTOGEN FAIL"; tail -15 autogen.log; exit 1; }
./configure > configure.log 2>&1 || { echo "CONFIGURE FAIL"; tail -15 configure.log; exit 1; }
make > make.log 2>&1 || { echo "MAKE FAIL"; tail -15 make.log; exit 1; }
ls -la zchangelog'
echo "=== 빌드 성공 ==="

echo "=== in-VM: inotify backend smoke (kernel 4.19) ==="
ssh -i /tmp/cikey -o IdentitiesOnly=yes -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ci@127.0.0.1 'cd /tmp/zcl
rm -rf wd j.db*; mkdir -p wd/sub
./zchangelog -d j.db register c1 > /dev/null
(./zchangelog -d j.db watch -b inotify $PWD/wd > watch.log 2>&1 &)
sleep 1
touch wd/f1
mkdir wd/d2
touch wd/d2/g
mv wd/f1 wd/f1r
rm wd/f1r
sleep 1
echo "--- watch log ---"; cat watch.log
out=$(./zchangelog -d j.db read c1)
echo "$out"
echo "$out" | grep -q CREATE || { echo "FAIL: no CREATE"; exit 1; }
echo "$out" | grep -q RENAME || { echo "FAIL: no RENAME"; exit 1; }
echo "$out" | grep -q REMOVE || { echo "FAIL: no REMOVE"; exit 1; }
echo "=== 4.19 inotify backend: PASS ==="'
echo "=== done ==="
