#!/bin/sh
# Fetch and stage the Debian base root (X server, apps, shell) for the
# Debian namespace. The staged tree is gitignored; it is material, not
# source.
# SUITE selects the Debian release (default: trixie).
set -eu
suite="${SUITE:-trixie}"
root="$(dirname "$0")/.."
tmp="$(mktemp -d)"
echo "staging Debian $suite i386 base"
curl -sL -o "$tmp/Packages.xz" "http://deb.debian.org/debian/dists/$suite/main/binary-i386/Packages.xz"
xz -d "$tmp/Packages.xz"
python3 - "$tmp" <<'PY'
import os, re, subprocess, sys
tmp = sys.argv[1]
ROOTS = ["xvfb", "xterm", "x11-apps",
         # Text Editor launches nano inside xterm; trixie has no xedit
         "nano",
         "fonts-dejavu-core",
         # Xorg compiles its keymap by running xkbcomp through /bin/sh
         "dash"]
pkg, provides = {}, {}
for b in open(tmp + "/Packages").read().split("\n\n"):
    f, key = {}, None
    for line in b.splitlines():
        if line.startswith(" ") and key:
            f[key] += "\n" + line
            continue
        key, _, v = line.partition(": ")
        f[key] = v
    n = f.get("Package")
    if not n or n in pkg or f.get("Architecture") not in ("i386", "all"):
        continue
    pkg[n] = f
    for p in (f.get("Provides") or "").split(","):
        p = p.strip().split(" ")[0]
        if p:
            provides.setdefault(p, n)
seen, q = set(), list(ROOTS)
while q:
    p = q.pop(0)
    r = p if p in pkg else provides.get(p)
    if not r or r in seen:
        continue
    seen.add(r)
    for d in (pkg[r].get("Depends") or "").split(","):
        d = d.split("|")[0].strip().split(" ")[0].split(":")[0]
        if d and d not in seen:
            q.append(d)
os.makedirs(tmp + "/root", exist_ok=True)
for n in sorted(seen):
    url = "http://deb.debian.org/debian/" + pkg[n]["Filename"]
    out = tmp + "/" + os.path.basename(pkg[n]["Filename"])
    subprocess.run(["curl", "-sL", "-o", out, url], check=True)
    x = tmp + "/x." + n
    os.makedirs(x, exist_ok=True)
    subprocess.run(["ar", "x", out], cwd=x, check=True)
    data = [c for c in os.listdir(x) if c.startswith("data.tar")][0]
    subprocess.run(["tar", "xf", data, "-C", tmp + "/root"], cwd=x, check=True)
subprocess.run(["cp", "-R", tmp + "/root/.", "debian/rootfs/"], check=True)
# dash ships only /bin/dash; the /bin/sh link is made by the package
# scripts on real Debian, so provide it here
os.makedirs("debian/rootfs/bin", exist_ok=True)
if os.path.lexists("debian/rootfs/bin/sh"):
    os.remove("debian/rootfs/bin/sh")
os.symlink("dash", "debian/rootfs/bin/sh")
print("staged", len(seen), "packages into debian/rootfs")
PY
rm -rf "$tmp"
