#!/usr/bin/env python3
"""Assemble the Installation 08 PS5 app (PPSA99908) from:
  --title    tools/build_native.sh's dist/PPSA99908 (eboot, libc.prx, lapy.elf, sce_sys)
  --runtime  a prospero-win app folder (win/, LICENSES, SOURCES.txt, ...)
  --prefix   an installation08 prefix (as on the console)
  --exe      installation08.exe to ship
  --extra    this folder (bundle/profiles, bundle/mcc)
  --readme   the players' README (ps5/README-PS5.txt), shipped as README.txt
--no-32bit leaves out Wine's 32-bit side, which Installation 08 and MCC don't use.
Wine's placeholder files are not shipped: placeholders.txt lists them and the app
makes them from its runtime on first start."""
import argparse, os, shutil, sys, zipfile
from pathlib import Path

PLACEHOLDER = b"Wine builtin DLL"
# Wine's stand-ins for these trip antivirus scanners on PCs and nothing uses them.
DROP_NAMES = {"iexplore.exe", "wmplayer.exe", "wordpad.exe"}
PERSONAL = ["installation08.ini", "installation08.log", "lan-id.txt", "restart.flag", "results", "saves",
            "temp", "lan_game_types", "profiles", "config"]

def pe_arch(path):
    head = path.read_bytes()[:0x400]
    off = int.from_bytes(head[0x3c:0x40], "little")
    machine = head[off + 4:off + 6]
    return {b"\x64\x86": "x86_64-windows", b"\x4c\x01": "i386-windows"}.get(machine)

def main():
    ap = argparse.ArgumentParser()
    for name in ("title", "runtime", "prefix", "exe", "ucrtbase", "extra", "readme", "patch", "out"):
        ap.add_argument("--" + name, type=Path, required=name not in ("ucrtbase", "patch", "readme"))
    ap.add_argument("--no-32bit", action="store_true")
    a = ap.parse_args()
    out = a.out
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(a.title, out)
    for name in ("LICENSE", "LICENSES", "SOURCES.txt", "THIRD_PARTY.md", "mesa-zink-manifest.json"):
        src = a.runtime / name
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, out / name)
    def runtime_filter(directory, names):
        rel = Path(directory).relative_to(a.runtime / "win")
        drop = {n for n in names if n.lower() in DROP_NAMES}
        if a.no_32bit and str(rel) in ("wine/lib/wine", "mesa-zink"):
            drop |= {n for n in names if n == "i386-windows"}
        return drop
    shutil.copytree(a.runtime / "win", out / "win", ignore=runtime_filter)
    runtime_lib = out / "win/wine/lib/wine"

    prefix = out / "bundle/prefixes/installation08"
    shutil.copytree(a.prefix, prefix, ignore=shutil.ignore_patterns(".wineserver"))
    games = prefix / "drive_c/Games/Installation08"
    for name in PERSONAL:
        p = games / name
        shutil.rmtree(p) if p.is_dir() else p.unlink(missing_ok=True)
    shutil.copy2(a.exe, games / "installation08.exe")
    if a.ucrtbase: shutil.copy2(a.ucrtbase, games / "ucrtbase.dll")
    if not (games / "ucrtbase.dll").is_file(): sys.exit("no ucrtbase.dll")

    windows = prefix / "drive_c/windows"
    # Mesa's DLLs are copied into the prefix by the app at every start.
    for dll in ("libgallium_wgl.dll", "opengl32.dll"):
        for d in ("system32", "syswow64"):
            p = windows / d / dll
            if p.exists() and p.read_bytes()[0x40:0x50] != PLACEHOLDER: p.unlink()
    # MCC's XAudio, if Installation 08 already put it in: back to Wine's (a placeholder).
    for d in ("system32", "syswow64"):
        p = windows / d / "xaudio2_9.dll"
        if p.exists() and p.read_bytes()[0x40:0x50] != PLACEHOLDER: p.unlink()
    lines = []
    for p in sorted(prefix.rglob("*")):
        if not p.is_file(): continue
        rel = p.relative_to(out / "bundle").as_posix()
        is32 = "/syswow64/" in rel or "Program Files (x86)" in rel or "/x86_" in rel or "/w32x86" in rel
        if a.no_32bit and (is32 or "/syswow64/" in rel):
            p.unlink(); continue
        if p.read_bytes()[0x40:0x50] != PLACEHOLDER: continue
        arch = pe_arch(p)
        p.unlink()
        if p.name.lower() in DROP_NAMES or not arch or not (runtime_lib / arch / p.name).is_file():
            continue
        if a.no_32bit and arch == "i386-windows": continue
        lines.append(f"{rel}\t{arch}\n")
    (windows / "syswow64").mkdir(exist_ok=True)
    (out / "placeholders.txt").write_text("".join(lines))

    shutil.copytree(a.extra / "bundle", out / "bundle", dirs_exist_ok=True)
    shutil.copy2(a.readme or a.extra / "README.txt", out / "README.txt")
    if a.patch: shutil.copy2(a.patch, out / a.patch.name)
    src = (out / "SOURCES.txt").read_text()
    if "installation08-app.patch" not in src:
        src = src.replace("commit 644e32e400c555e340d507383398c5d21b39b0b0\n",
            "commit 644e32e400c555e340d507383398c5d21b39b0b0,\n  with installation08-app.patch (in this folder),"
            " built as PPSA99908 with -DPW_FIXED_PROFILE=\"installation08\"\n  -DPW_DATA_NAME=\"installation08\""
            " -DPW_APP_NAME=\"installation08-wine64\"\n  -DPW_RESTART_FLAG=\"drive_c/Games/Installation08/restart.flag\"\n", 1)
        src = src.replace("Lapy JB Daemon (lapy.elf)  https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/releases/tag/v0.3.2-experimental  release v0.3.2-experimental",
            "Lapy JB Daemon (lapy.elf)  https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon  tag v0.3.2-experimental,\n"
            "  built for this title: tools/build_owned_daemon.py --elf-helper --title PPSA99908")
        src += ("\nInstallation 08  installation08.exe in bundle/prefixes/installation08/drive_c/Games/Installation08.\n"
                "ucrtbase.dll  Microsoft's redistributable x64 Universal C Runtime (Windows SDK Redist).\n"
                "The prefix in bundle/prefixes/installation08 was made with Wine and DXVK as listed above;\n"
                "Wine's placeholder files are made from win/ on first start (placeholders.txt).\n")
        if a.no_32bit: src += "This package leaves out Wine's 32-bit side (lib/wine/i386-windows, Mesa i386).\n"
        (out / "SOURCES.txt").write_text(src)

    zpath = out.parent / (out.parent.name + ".zip")
    zpath.unlink(missing_ok=True)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(out.rglob("*")):
            info_path = Path(out.name) / p.relative_to(out)
            if p.is_dir():
                z.write(p, info_path.as_posix() + "/")
            else:
                z.write(p, info_path)
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"{out}: {size >> 20} MB, {len(lines)} placeholders; {zpath} {zpath.stat().st_size >> 20} MB")

main()
