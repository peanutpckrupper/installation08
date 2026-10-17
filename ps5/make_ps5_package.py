#!/usr/bin/env python3
"""Turn the prefix tools/pw_install.py made from installation08.yml into a
ready-to-upload package, so players need nothing but an FTP client.

The package mirrors /data/prospero-win on the console, exactly as
tools/pw_prefix.py push would leave it there:
  prospero-win/prefixes/installation08/   the prefix: Wine's own files, DXVK,
                                          installation08.exe and Microsoft's
                                          ucrtbase.dll (no game files)
  prospero-win/profiles/installation08.profile   graphics = opengl
  prospero-win/mcc/                        where players put their MCC folders
Links become the console's .pw-symlinks tables, the registry gets the
console's WoW64 CPU, and wowprospero.dll goes in system32, as a push does.

Usage (from prospero-win's source folder, so pw_prefix.py can be imported):
    python3 make_ps5_package.py --prospero . --library ~/prospero-library \
        --cpu-dll <prospero-win app>/win/wine/lib/wine/x86_64-windows/wowprospero.dll \
        --readme README-PS5.txt --out ~/Installation08-PS5
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import zipfile
from pathlib import Path

SLUG = "installation08"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prospero", required=True, type=Path, help="prospero-win's source folder")
    parser.add_argument("--library", required=True, type=Path, help="pw_install.py's --library")
    parser.add_argument("--cpu-dll", required=True, type=Path, help="prospero-win's wowprospero.dll")
    parser.add_argument("--readme", required=True, type=Path, help="the players' README to put at the top")
    parser.add_argument("--ucrtbase", type=Path,
                        help="Microsoft's x64 ucrtbase.dll (Windows SDK Redist\\...\\ucrt\\DLLs\\x64), if the prefix lacks it")
    parser.add_argument("--out", required=True, type=Path, help="folder to write (replaced); a .zip beside it too")
    args = parser.parse_args()

    sys.path.insert(0, str((args.prospero / "tools").resolve()))
    import pw_prefix  # the push's own rules: links, registry, CPU DLL

    prefix = args.library / "prefixes" / SLUG
    profile = args.library / "profiles" / f"{SLUG}.profile"
    if not (prefix / "system.reg").is_file() or not profile.is_file():
        parser.error(f"no {SLUG} prefix and profile in {args.library}: run pw_install.py with installation08.yml first")
    if not args.cpu_dll.is_file():
        parser.error(f"{args.cpu_dll} not found")

    out = args.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    root = out / "prospero-win"
    target = root / "prefixes" / SLUG

    files, dirs, links = pw_prefix.local_tree(prefix)
    for directory in sorted(dirs):
        (target / directory).mkdir(parents=True, exist_ok=True)
    for key, source in files.items():
        destination = target / key
        destination.parent.mkdir(parents=True, exist_ok=True)
        data_key = key if key in pw_prefix.REGISTRY else None
        if data_key:
            destination.write_bytes(pw_prefix.to_console(key, source.read_bytes()))
        else:
            shutil.copyfile(source, destination)
    for directory, table in links.items():
        text = "".join(f"{name}\t{link}\n" for name, link in sorted(table.items()))
        (target / directory / pw_prefix.LINK_TABLE).write_text(text)
    user = target / "drive_c" / "users" / pw_prefix.CONSOLE_USER
    for folder in pw_prefix.CONSOLE_USER_DIRS:
        (user / folder).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.cpu_dll, target / pw_prefix.CPU_DLL)

    # The profile says ucrtbase=n, so Installation 08 doesn't start without
    # Microsoft's ucrtbase.dll next to it: never write a package without it.
    games = target / "drive_c" / "Games" / "Installation08"
    if args.ucrtbase:
        if not args.ucrtbase.is_file():
            parser.error(f"{args.ucrtbase} not found")
        shutil.copyfile(args.ucrtbase, games / "ucrtbase.dll")
    if not (games / "ucrtbase.dll").is_file():
        parser.error("no ucrtbase.dll in the prefix: pass --ucrtbase <Microsoft's x64 ucrtbase.dll>")

    text = profile.read_text()
    text = text.replace("graphics = dxvk\n", "graphics = opengl\n")
    if "ucrtbase=n" not in text.replace(" ", ""):
        parser.error(f"{profile} doesn't use ucrtbase=n: make the prefix with the 1.3 installation08.yml")
    (root / "profiles").mkdir(parents=True)
    (root / "profiles" / f"{SLUG}.profile").write_text(text)
    (root / "mcc").mkdir()
    (root / "mcc" / "PUT YOUR MCC FOLDERS HERE.txt").write_text(
        "Copy halo1, halo2, halo3, halo3odst, haloreach, halo4 and groundhog (H2A multiplayer)\n"
        "(the ones you want) here,\n"
        "and from the mcc folder only mcc\\binaries\\win64, keeping its path (mcc/binaries/win64).\n")
    shutil.copyfile(args.readme, out / "README.txt")

    archive = out.with_suffix(".zip")
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
        for path in sorted(out.rglob("*")):
            zipped.write(path, Path(out.name) / path.relative_to(out))
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"make_ps5_package: {out} ({size / (1 << 20):.0f} MB), {archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
