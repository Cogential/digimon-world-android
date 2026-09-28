#!/usr/bin/env python3
"""Assemble the unsigned APK from aapt2's resource APK, dex, native libs and
the game payload.

Native libraries and the disc image are STORED (not deflated): the manifest
sets extractNativeLibs="false", so Android maps the .so files straight out of
the APK (zipalign -P 16 page-aligns them afterwards), and the disc image is
copied out on first launch, which is much faster from a stored entry.

payload/manifest.txt lists every payload file with its size so the app knows
what to unpack; its version line is a digest of the payload, so an update that
changes nothing in the payload does not re-unpack anything.
"""
import argparse
import hashlib
import os
import zipfile


def payload_files(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel == "manifest.txt":
                continue
            out.append((rel, full))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--dex-dir", required=True)
    ap.add_argument("--lib-dir", required=True)
    ap.add_argument("--payload", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    files = payload_files(a.payload)
    digest = hashlib.sha1()
    lines = []
    for rel, full in files:
        size = os.path.getsize(full)
        lines.append("%d\t%s" % (size, rel))
        digest.update(("%s\0%d\0" % (rel, size)).encode())
        if not rel.startswith("disc/"):  # the disc is pinned by its size
            with open(full, "rb") as f:
                digest.update(f.read())
    manifest = "version %s\n%s\n" % (digest.hexdigest()[:16], "\n".join(lines))

    with zipfile.ZipFile(a.base) as base, \
            zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as z:
        for info in base.infolist():
            z.writestr(info, base.read(info.filename), compress_type=info.compress_type)
        for name in sorted(os.listdir(a.dex_dir)):
            if name.endswith(".dex"):
                z.write(os.path.join(a.dex_dir, name), name)
        for abi in sorted(os.listdir(a.lib_dir)):
            for so in sorted(os.listdir(os.path.join(a.lib_dir, abi))):
                z.write(os.path.join(a.lib_dir, abi, so), "lib/%s/%s" % (abi, so),
                        compress_type=zipfile.ZIP_STORED)
        z.writestr("assets/payload/manifest.txt", manifest)
        for rel, full in files:
            stored = rel.startswith("disc/")
            z.write(full, "assets/payload/" + rel,
                    compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)
    print("payload: %d files, version %s" % (len(files), manifest.split()[1]))


if __name__ == "__main__":
    main()
