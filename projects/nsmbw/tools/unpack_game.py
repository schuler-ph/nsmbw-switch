"""Prepare the translator's inputs from New Super Mario Bros. Wii: a disc image or an extracted disc.

    python unpack_game.py <disc image or extracted game folder> [<output folder>] [--variant <v>]
                          [--extract-to <folder>] [--check]

A disc image (.iso, .wbfs, .rvz, .wia, .gcz, .ciso) is first checked (game ID and revision) and then
extracted into a folder of its own, the same folder Dolphin's Extract Entire Disc makes: by default
disc-<game ID> next to this repository (disc-SMNE01), or --extract-to. That folder is the game data
the port runs from (build-linux.sh --game, sdmc:/switch/nsmbw/game/); an existing one is reused.
.iso and .wbfs are read by WiiDiscExtract (the game's own Open ISO code, built here the first time,
about a minute: needs cmake and a C++ compiler), compressed images by nodtool (put it on PATH or set
NODTOOL). An extracted folder is what Dolphin's Extract Entire Disc makes (files/ and sys/, or a DATA
folder holding them).

This copies sys/main.dol and unpacks the four boot RELs (files/rels/*.rel.LZ, LZ77 type 0x11) into the
output folder: by default the variant's folder next to this repository ("game" for SMNE01 rev 1,
where recomp.yml looks for them). Then run prelink_rels.py. Standard library only.

--variant picks the game version (projects/nsmbw/variants/variants.toml; default E1, SMNE01 rev 1):
the disc's ID, revision and main.dol checksum must be that version's. --check only checks the disc
and unpacks the RELs in memory: it keeps nothing (an image is extracted into a temporary folder).
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RELS = ("d_profileNP", "d_basesNP", "d_enemiesNP", "d_en_bossNP")
TOOLS = Path(__file__).resolve().parent
NODTOOL_RELEASE = "https://github.com/encounter/nod/releases/tag/v2.0.0-alpha.10"
COMPRESSED = {b"RVZ\x01": ".rvz", b"WIA\x01": ".wia", b"\x01\xC0\x0B\xB1": ".gcz", b"CISO": ".ciso"}


def lz11(data: bytes) -> bytes:
    if data[0] != 0x11:
        raise ValueError("not LZ77 type 0x11 data")
    size = data[1] | data[2] << 8 | data[3] << 16
    pos = 4
    if size == 0:
        size = struct.unpack_from("<I", data, 4)[0]
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[pos])
                pos += 1
                continue
            b0 = data[pos]
            if b0 >> 4 == 0:
                b1, b2 = data[pos + 1], data[pos + 2]
                length = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11
                disp = ((b1 & 0xF) << 8 | b2) + 1
                pos += 3
            elif b0 >> 4 == 1:
                b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]
                length = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
                disp = ((b2 & 0xF) << 8 | b3) + 1
                pos += 4
            else:
                b1 = data[pos + 1]
                length = (b0 >> 4) + 1
                disp = ((b0 & 0xF) << 8 | b1) + 1
                pos += 2
            start = len(out) - disp
            for i in range(length):  # the source may overlap what is being written
                out.append(out[start + i])
    return bytes(out[:size])


def find_root(path: Path) -> Path:
    for candidate in (path, path / "DATA"):
        if (candidate / "files").is_dir() and (candidate / "sys" / "main.dol").is_file():
            return candidate
    sys.exit(f"{path} is not an extracted game: it needs files/ and sys/ (Dolphin's Extract Entire Disc)")


def check_id(game_id: str, revision: int, variant: dict, known: dict) -> None:
    """Exits unless the disc header's game ID and revision are the variant's."""
    want = f"{variant['game_id']} revision {variant['revision']}"
    if game_id != variant["game_id"] or revision != variant["revision"]:
        other = next((v["name"] for v in known.values()
                      if v["game_id"] == game_id and v["revision"] == revision), None)
        hint = f" (that is variant {other}: pass --variant {other})" if other else ""
        sys.exit(f"this is {game_id} revision {revision}{hint}; "
                 f"the port needs New Super Mario Bros. Wii {want} "
                 f"(Dolphin shows both in the game's Properties, Info tab)")


def check_disc(root: Path, variant: dict, known: dict) -> bytes:
    """Exits unless the extract is the variant's clean disc; returns its main.dol."""
    header = (root / "sys" / "boot.bin").read_bytes()[:8]
    check_id(header[:6].decode("ascii", "replace"), header[7], variant, known)
    want = f"{variant['game_id']} revision {variant['revision']}"
    dol = (root / "sys" / "main.dol").read_bytes()
    accepted = (variant["main_dol_sha256"], *variant.get("main_dol_sha256_also", ()))
    if hashlib.sha256(dol).hexdigest() not in accepted:
        sys.exit(f"sys/main.dol differs from the clean {want} executable (modified game?)")
    fst = struct.unpack_from(">I", (root / "sys" / "boot.bin").read_bytes(), 0x428)[0] * 4
    if "fst_size" in variant and fst != variant["fst_size"]:
        sys.exit(f"the disc's FST is 0x{fst:X} bytes, not {want}'s 0x{variant['fst_size']:X} (modified game?)")
    return dol


def run(cmd: list, what: str) -> str:
    """Runs a tool; exits with the tool's own error message (what it printed to stderr) when it fails."""
    try:
        done = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, errors="replace")
    except OSError as e:
        sys.exit(f"couldn't run {cmd[0]}: {e}")
    if done.returncode != 0:
        def clean(line):
            line = re.sub(r"\x1b\[[0-9;]*m", "", line).strip()
            return re.sub(r"^(error|Error|Failed|Caused by):\s*", "", line)
        errors = [clean(line) for line in done.stderr.splitlines() if line.strip()]
        output = [clean(line) for line in done.stdout.splitlines() if line.strip()]
        why = "; ".join(errors) or (output[-1] if output else f"exit code {done.returncode}")
        sys.exit(f"{what}: {why}")
    return done.stdout


def wiidisc_extract(repo: Path) -> Path:
    """WiiDiscExtract: $WIIDISC_EXTRACT, or built (once, then kept up to date) in ../build-disc-extract."""
    if os.environ.get("WIIDISC_EXTRACT"):
        return Path(os.environ["WIIDISC_EXTRACT"])
    build = repo.parent / "build-disc-extract"
    exe = build / ("WiiDiscExtract.exe" if os.name == "nt" else "WiiDiscExtract")
    if shutil.which("cmake") is None:
        sys.exit("reading a disc image needs cmake and a C++ compiler (to build WiiDiscExtract once); "
                 "install them, or pass a folder made by Dolphin's Extract Entire Disc")
    if not (build / "CMakeCache.txt").is_file():
        print(f"building WiiDiscExtract into {build} (once, about a minute)")
        run(["cmake", "-S", TOOLS / "disc-extract", "-B", build, "-DCMAKE_BUILD_TYPE=Release"],
            "couldn't set up the WiiDiscExtract build")
    run(["cmake", "--build", build, "--parallel", str(os.cpu_count() or 2)], "couldn't build WiiDiscExtract")
    if not exe.is_file():
        sys.exit(f"the WiiDiscExtract build made no {exe}")
    return exe


def nodtool(repo: Path, kind: str) -> Path:
    """nodtool for compressed images: $NODTOOL, PATH, or the Launcher's copy."""
    for candidate in (os.environ.get("NODTOOL"), shutil.which("nodtool"),
                      repo / "Launcher" / "artifacts" / ("nodtool.exe" if os.name == "nt" else "nodtool")):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    sys.exit(f"that's a compressed disc image ({kind}): unpack_game.py reads those with nodtool "
             f"(MIT or Apache-2.0, {NODTOOL_RELEASE}). Put it on PATH or set NODTOOL=<path>. "
             "Or let Dolphin turn it into an .iso: right-click the game, Convert File..., Format: ISO")


def image_kind(image: Path) -> str:
    """'' for an .iso or .wbfs (WiiDiscExtract reads those), else the compressed format's name."""
    with image.open("rb") as fh:
        magic = fh.read(4)
        fh.seek(0x200)
        nkit = fh.read(4)
    if magic in COMPRESSED:
        return COMPRESSED[magic]
    return "NKit" if nkit == b"NKIT" else ""


def extract_image(image: Path, target: Path, variant: dict, known: dict, repo: Path) -> None:
    """Checks the image's game ID and revision, then extracts it into `target` (sys/ and files/)."""
    kind = image_kind(image)
    tool = nodtool(repo, kind) if kind else wiidisc_extract(repo)
    if kind:
        info = run([tool, "--no-color", "info", image], f"nodtool couldn't read {image.name}")
        found = re.search(r"Game ID: (\S{6})", info), re.search(r"Revision (\d+)", info)
        if not all(found):
            sys.exit(f"{image.name}: nodtool didn't report a game ID and revision")
        game_id, revision, size = found[0][1], int(found[1][1]), 400 << 20
    else:
        info = run([tool, "--info", image], image.name)
        fields = dict(part.split("=", 1) for part in info.split())
        game_id, revision, size = fields["id"], int(fields["revision"]), int(fields["bytes"])
    check_id(game_id, revision, variant, known)
    target.parent.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(target.parent).free
    if free < size + (64 << 20):
        sys.exit(f"{target.parent} has {free >> 20} MiB free; extracting the disc needs {size >> 20} MiB")
    partial = target.with_name(target.name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    print(f"{game_id} revision {revision}: extracting {image.name} into {target}")
    try:
        if kind:
            run([tool, "--no-color", "extract", "-q", image, partial], f"couldn't extract {image.name}")
        else:
            run([tool, image, partial], f"couldn't extract {image.name}")
    except SystemExit:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    partial.rename(target)


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = sys.argv[1:]
    options = {"--variant": "E1", "--extract-to": None}
    for option in options:
        if option in flags:
            at = flags.index(option)
            if at + 1 >= len(flags):
                sys.exit(__doc__)
            options[option] = flags[at + 1]
            args.remove(flags[at + 1])
    variant_name = options["--variant"]
    check_only = "--check" in flags
    if not args:
        sys.exit(__doc__)
    sys.path.insert(0, str(TOOLS))
    import variants  # noqa: E402  (tomllib: Python 3.11+, only needed here)
    known = variants.load_variants()
    if variant_name not in known:
        sys.exit(f"unknown variant {variant_name}; variants.toml has {', '.join(sorted(known))}")
    variant = known[variant_name]
    source = Path(args[0])
    out = Path(args[1]) if len(args) > 1 else variants.default_game_dir(variant)
    temp = None
    if source.is_file():
        if check_only:
            temp = tempfile.TemporaryDirectory(prefix="unpack_game-")
            disc = Path(temp.name) / "disc"
        else:
            disc = Path(options["--extract-to"] or variants.REPO.parent / f"disc-{variant['game_id']}")
        if disc.is_dir() and any(disc.iterdir()):
            print(f"{disc} is already there: using it (delete it to extract the image again)")
        else:
            extract_image(source, disc, variant, known, variants.REPO)
        source = disc
    elif not source.exists():
        sys.exit(f"{source} doesn't exist: pass a disc image or the folder Dolphin's Extract Entire Disc made")
    root = find_root(source)
    check_disc(root, variant, known)
    rels = {name: lz11((root / "files" / "rels" / f"{name}.rel.LZ").read_bytes()) for name in RELS}
    if check_only:
        for name, rel in rels.items():
            print(f"{name}.rel  {len(rel)} bytes")
        print(f"{variant['game_id']} revision {variant['revision']}: clean ({variant_name}); nothing kept")
        if temp:
            temp.cleanup()
        return
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / "sys" / "main.dol", out / "main.dol")
    for name in RELS:
        rel = rels[name]
        (out / f"{name}.rel").write_bytes(rel)
        print(f"{name}.rel  {len(rel)} bytes")
    nxt = "prelink_rels.py" if variant_name == "E1" else f"prelink_rels.py {out} --variant {variant_name}"
    print(f"main.dol and the RELs are in {out}; next: {nxt}")
    if Path(args[0]).is_file():
        print(f"the game data (files/ and sys/) is in {source}: build-linux.sh --game {source}")


if __name__ == "__main__":
    main()
