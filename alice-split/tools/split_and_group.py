#!/usr/bin/env python3
"""
แยก alice-merged.bin กลับเป็น flash image ย่อย + แตก generated_assets.bin
+ จัดกลุ่ม source code ที่ถูก compile จริง (อ้างอิง build/compile_commands.json)

usage:  python3 alice-split/tools/split_and_group.py [path/to/merged.bin]
        ถ้าไม่ระบุ จะใช้ alice-split/00_merged/alice-merged.bin
"""
import hashlib, json, os, re, shutil, struct, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(REPO, "alice-split")
BUILD = os.path.join(REPO, "build")
MAIN = os.path.join(REPO, "main")


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


# ---------------------------------------------------------------- 1. split merged
def esp_image_len(buf, off):
    """ความยาวจริงของ ESP app/bootloader image (header + segments + checksum + SHA256)"""
    if buf[off] != 0xE9:
        raise ValueError(f"no ESP image magic at 0x{off:x}")
    nseg = buf[off + 1]
    hash_appended = buf[off + 23]
    p = off + 24
    for _ in range(nseg):
        _load, ln = struct.unpack_from("<II", buf, p)
        p += 8 + ln
    p += 1                       # checksum byte
    p = (p + 15) & ~15           # pad to 16
    if hash_appended == 1:
        p += 32
    return p - off


def parse_partition_table(buf, off=0x8000):
    parts = []
    for i in range(0, 0xC00, 32):
        e = buf[off + i: off + i + 32]
        if e[:2] != b"\xAA\x50":
            break
        ptype, sub, poff, size = struct.unpack_from("<BBII", e, 2)
        name = e[12:28].split(b"\0")[0].decode()
        parts.append(dict(name=name, type=ptype, subtype=sub, offset=poff, size=size))
    return parts


def assets_len(buf, off):
    nfiles, _cks, ln = struct.unpack_from("<III", buf, off)
    if not (0 < nfiles < 4096) or off + 12 + ln > len(buf):
        raise ValueError(f"no assets blob at 0x{off:x}")
    return 12 + ln


def split_merged(merged_path, dst):
    buf = open(merged_path, "rb").read()
    os.makedirs(dst, exist_ok=True)
    parts = parse_partition_table(buf)
    by = {p["name"]: p for p in parts}
    plan = [
        ("0x000000_bootloader.bin", 0x0, esp_image_len(buf, 0x0)),
        ("0x008000_partition-table.bin", 0x8000, 0xC00),
        ("0x00d000_ota_data_initial.bin", by["otadata"]["offset"], by["otadata"]["size"]),
        ("0x020000_xiaozhi.bin", by["ota_0"]["offset"], esp_image_len(buf, by["ota_0"]["offset"])),
        ("0x800000_generated_assets.bin", by["assets"]["offset"], assets_len(buf, by["assets"]["offset"])),
    ]
    out = []
    for name, off, ln in plan:
        p = os.path.join(dst, name)
        open(p, "wb").write(buf[off: off + ln])
        out.append(dict(file=name, offset=hex(off), size=ln, sha256=sha(p)))
    # nvs / phy_init / ota_1 อยู่ใน merged เป็น 0xFF ล้วน — ตรวจยืนยัน
    blank = {}
    for n in ("nvs", "phy_init", "ota_1"):
        if n in by:
            seg = buf[by[n]["offset"]: by[n]["offset"] + by[n]["size"]]
            blank[n] = seg.count(0xFF) == len(seg) if seg else "outside image"
    return parts, out, blank


# ---------------------------------------------------------------- 2. unpack assets
def unpack_assets(assets_bin, dst):
    buf = open(assets_bin, "rb").read()
    nfiles, cks, ln = struct.unpack_from("<III", buf, 0)
    body = buf[12: 12 + ln]
    assert (sum(body) & 0xFFFF) == cks, "assets checksum mismatch"
    table_len = nfiles * 44
    data = body[table_len:]
    os.makedirs(dst, exist_ok=True)
    files = []
    for i in range(nfiles):
        e = body[i * 44:(i + 1) * 44]
        name = e[:32].split(b"\0")[0].decode()
        size, off, w, h = struct.unpack_from("<IIHH", e, 32)
        assert data[off:off + 2] == b"\x5A\x5A"
        open(os.path.join(dst, name), "wb").write(data[off + 2: off + 2 + size])
        files.append(dict(name=name, size=size))
    return files


# ---------------------------------------------------------------- 3. group sources
GROUPS = [
    ("01_Core", "main loop, state machine, MCP, OTA, assets loader, settings"),
    ("02_Board", "board ของ Waveshare AMOLED-2.06 + boards/common + LED"),
    ("03_Audio", "audio_service, codec, AFE/wake word, ogg demuxer"),
    ("04_Protocol", "websocket / mqtt protocol"),
    ("05_Display", "LCD/LVGL display, theme, font, emoji, gif, jpeg"),
    ("06_Apps", "radar / CSI / flight radar / SD card"),
    ("07_Embedded_ogg", "เสียงระบบที่ฝังใน xiaozhi.bin (th-TH + common)"),
    ("08_Components", "local_components (vendored) + รายการ managed_components"),
    ("09_Build_config", "partition, sdkconfig, CMake, flasher args"),
]


def group_of(rel):  # rel = path relative to main/
    if rel.startswith("boards/") or rel.startswith("led/"):
        return "02_Board"
    if rel.startswith("audio/"):
        return "03_Audio"
    if rel.startswith("protocols/"):
        return "04_Protocol"
    if rel.startswith("display/"):
        return "05_Display"
    if rel.startswith("apps/"):
        return "06_Apps"
    return "01_Core"


INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
_all_main = None


def resolve_include(name, from_file):
    global _all_main
    cand = os.path.normpath(os.path.join(os.path.dirname(from_file), name))
    if os.path.isfile(cand):
        return cand
    cand = os.path.join(MAIN, name)
    if os.path.isfile(cand):
        return cand
    if _all_main is None:
        _all_main = [os.path.join(d, f) for d, _, fs in os.walk(MAIN) for f in fs]
    hits = [p for p in _all_main if p.endswith("/" + name)]
    return hits[0] if len(hits) == 1 else None


def group_sources(dst):
    cc = json.load(open(os.path.join(BUILD, "compile_commands.json")))
    compiled = sorted({x["file"] for x in cc if x["file"].startswith(MAIN + "/")})
    todo, seen = list(compiled), set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        base, _ = os.path.splitext(f)
        if os.path.isfile(base + ".h") and base + ".h" not in seen:
            todo.append(base + ".h")
        try:
            txt = open(f, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for inc in INC.findall(txt):
            r = resolve_include(inc, f)
            if r and r.startswith(MAIN + "/") and r not in seen:
                todo.append(r)
    manifest = []
    for f in sorted(seen):
        rel = os.path.relpath(f, MAIN)
        g = group_of(rel)
        target = os.path.join(dst, g, "main", rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(f, target)
        manifest.append(dict(group=g, path="main/" + rel, compiled=f in compiled, sha256=sha(f)))

    # 07 embedded ogg — ที่มาอ่านจากบรรทัดแรกของ build/*.ogg.S
    for s in sorted(os.listdir(BUILD)):
        if not s.endswith(".ogg.S"):
            continue
        first = open(os.path.join(BUILD, s)).readline()
        m = re.search(r"converted from (\S+)", first)
        if not m:
            continue
        src = m.group(1)
        rel = os.path.relpath(src, REPO)
        target = os.path.join(dst, "07_Embedded_ogg", rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(src, target)
        manifest.append(dict(group="07_Embedded_ogg", path=rel, compiled=True, sha256=sha(src)))

    # 08 components
    lc = os.path.join(REPO, "local_components")
    shutil.copytree(lc, os.path.join(dst, "08_Components", "local_components"), dirs_exist_ok=True)
    desc = json.load(open(os.path.join(BUILD, "project_description.json")))
    lock = open(os.path.join(REPO, "dependencies.lock")).read()
    vers = dict(re.findall(r"^  ([\w./-]+):\n(?:    .*\n)*?    version: (\S+)", lock, re.M))
    comps = []
    for c in desc["build_components"]:
        if not c:
            continue
        info = desc.get("build_component_info", {}).get(c, {})
        d = info.get("dir", "")
        key = c.replace("__", "/", 1)
        comps.append(dict(name=c, version=vers.get(key, "idf" if "/esp-idf/" in d or "esp-idf" in d else ""),
                          dir=os.path.relpath(d, REPO) if d.startswith(REPO) else d))
    json.dump(comps, open(os.path.join(dst, "08_Components", "linked_components.json"), "w"),
              indent=1, ensure_ascii=False)

    # 09 build config
    cfg = ["CMakeLists.txt", "main/CMakeLists.txt", "main/Kconfig.projbuild", "partitions/v2/16m.csv",
           "sdkconfig", "sdkconfig.defaults", "sdkconfig.defaults.esp32s3", "sdkconfig.defaults.thai",
           "dependencies.lock", "main/idf_component.yml", "scripts/build_default_assets.py",
           "build/flasher_args.json", "build/flash_args"]
    for c in cfg:
        p = os.path.join(REPO, c)
        if os.path.isfile(p):
            t = os.path.join(dst, "09_Build_config", c)
            os.makedirs(os.path.dirname(t), exist_ok=True)
            shutil.copy2(p, t)
            manifest.append(dict(group="09_Build_config", path=c, compiled=False, sha256=sha(p)))
    return manifest, comps


def main():
    merged = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OUT, "00_merged", "alice-merged.bin")
    parts, images, blank = split_merged(merged, os.path.join(OUT, "01_flash_images"))
    assets = unpack_assets(os.path.join(OUT, "01_flash_images", "0x800000_generated_assets.bin"),
                           os.path.join(OUT, "02_assets_unpacked"))
    src_dir = os.path.join(OUT, "03_source_by_group")
    if os.path.isdir(src_dir):
        shutil.rmtree(src_dir)
    manifest, comps = group_sources(src_dir)
    json.dump(dict(merged=os.path.relpath(merged, REPO), merged_sha256=sha(merged),
                   partition_table=parts, images=images, blank_regions_all_0xFF=blank,
                   assets=assets, sources=manifest),
              open(os.path.join(OUT, "MANIFEST.json"), "w"), indent=1, ensure_ascii=False)
    print("images:")
    for i in images:
        print(f"  {i['offset']:>9} {i['size']:>10,}  {i['file']}")
    print("blank (0xFF) regions:", blank)
    print(f"assets unpacked: {len(assets)} files")
    counts = {}
    for m in manifest:
        counts[m["group"]] = counts.get(m["group"], 0) + 1
    for g, _ in GROUPS:
        print(f"  {g:<18} {counts.get(g, 0):>4} files")
    print(f"linked components: {len(comps)}")


if __name__ == "__main__":
    main()
