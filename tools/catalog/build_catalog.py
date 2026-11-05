#!/usr/bin/env python3
"""
Builds the Buy-mode catalog index used by the in-game catalog browser (F7 panel).

  ts2catalog.bin  - records (GUID, price, category, names EN/PT, thumbnail index)
  ts2thumbs.bin   - 64x64 RGB565 thumbnails

Both files are written next to the game executable (TSBin) and read by ijl15.dll at runtime.

Sources
  * every  <pack>/TSData/Res/Objects/objects.package   (OBJD = GUID/price/type, CTSS = catalog names)
  * every  ObjectThumbnails.package found in the usual Documents / Wine prefix locations
    (the game caches the thumbnails it renders while you browse the real Buy catalog; objects you
    never browsed simply get a coloured placeholder tile)

Usage:  build_catalog.py [--game "/path/to/The Sims 2"] [--out TSBin] [--thumbs extra.package ...]
"""
import argparse, glob, io, os, struct, sys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbpf

try:
    from PIL import Image
except ImportError:
    Image = None

T_OBJD, T_CTSS, T_THUMB = 0x4F424A44, 0x43545353, 0xAC2950C1
THUMB = 64

# ---------------------------------------------------------------------------------------------
# categories (id -> (EN, PT)); order = order in the UI
# ---------------------------------------------------------------------------------------------
CATS = [
    ("All", "Tudo"),
    ("Seating", "Assentos"),
    ("Beds", "Camas"),
    ("Surfaces", "Superfícies"),
    ("Appliances", "Eletrodomésticos"),
    ("Electronics", "Eletrônicos"),
    ("Plumbing", "Hidráulica"),
    ("Lighting", "Iluminação"),
    ("Decorative", "Decoração"),
    ("Hobbies & Fun", "Hobbies e diversão"),
    ("Outdoors", "Exterior"),
    ("Pets", "Pets"),
    ("Structure", "Estrutura"),
    ("Other", "Outros"),
]
CAT = {n[0]: i for i, n in enumerate(CATS)}

RULES = [  # first match wins; keywords are matched on lower-case words/substrings
    ("Pets",        ["pet ", "dog", "cat ", "litter", "kennel", "bird cage", "birdcage", "hamster", "scratch", "aquarium", "leash"]),
    ("Beds",        ["bed", "crib", "bunk", "cot ", "hammock", "futon"]),
    ("Seating",     ["booth", "chair", "sofa", "loveseat", "couch", "stool", "bench", "recliner", "ottoman", "beanbag", "bean bag", "armchair", "settee", "pew", "throne", "seat"]),
    ("Plumbing",    ["toilet", "sink", "tub", "shower", "urinal", "bidet", "faucet", "washbasin", "hot tub", "hottub", "spa ", "outhouse"]),
    ("Appliances",  ["ceiling fan", "radiator", "solar panel", "hot chocolate", "fridge", "refrigerator", "stove", "oven", "microwave", "dishwasher", "washer", "dryer", "toaster", "blender", "espresso", "coffee", "grill", "freezer", "vending", "mixer", "juicer", "fryer", "range", "food processor", "trash", "compactor", "heater", "air condition", "ice maker", "kettle", "cooker"]),
    ("Electronics", ["tv", "television", "stereo", "computer", "phone", "radio", "camera", "video", "console", "boombox", "jukebox", "speaker", "karaoke", "robot", "alarm", "projector", "game ", "handheld", "laptop", "walkie", "intercom", "answering", "fax", "printer", "monitor"]),
    ("Lighting",    ["lamp", "chandelier", "light", "sconce", "torch", "lantern", "candle", "lava", "spotlight", "bulb", "lantern"]),
    ("Hobbies & Fun", ["rink", "soccer net", "pirate ship", "axe throwing", "log rolling", "spring rider", "easel", "piano", "guitar", "drum", "chess", "pool table", "billiard", "exercise", "treadmill", "weight", "telescope", "chemistry", "workbench", "loom", "sewing", "dart", "foosball", "pinball", "arcade", "trampoline", "swing", "toy", "sandbox", "playhouse", "dollhouse", "bookshelf", "bookcase", "violin", "cello", "sax", "organ", "keyboard", "xylophone", "bongo", "microscope", "ouija", "crystal ball", "magic", "fishing", "puppet", "tent", "see-saw", "seesaw", "slide", "monkey bars", "jungle", "punching", "hot air", "ping", "table tennis", "air hockey", "bar -", "dj ", "mixing", "poker", "slot machine", "roulette", "casino", "craps", "kitchen set", "play", "karate", "yoga", "dance", "stage", "podium", "lectern", "juggl", "painting kit", "pottery", "wheel", "forge", "kiln", "sculpt"]),
    ("Surfaces",    ["table", "counter", "desk", "dresser", "shelf", "bar", "island", "cabinet", "nightstand", "end table", "vanity", "sideboard", "hutch", "cupboard", "buffet", "wardrobe", "armoire", "bureau", "chest", "stand", "rack", "podium", "workstation", "tray", "cart"]),
    ("Decorative",  ["coffin", "perfume", "stuffed animal", "centerpiece", "painting", "by ", "picture", "sculpture", "plant", "flower", "rug", "mirror", "vase", "statue", "curtain", "fountain", "wreath", "poster", "clock", "fireplace", "mantel", "banner", "mask", "tapestry", "ornament", "decor", "frame", "globe", "display", "bust", "pillar", "sign", "tree", "garland", "trophy", "mobile", "screen", "divider", "bowl", "basket", "plaque", "candelabra", "urn", "hearth", "stocking", "wall hanging", "wallhanging", "pumpkin", "snow", "mat"]),
    ("Outdoors",    ["shrub", "flamingo", "gnome", "sprinkler", "campfire", "bonfire", "fire jet", "bush", "hedge", "garden", "barbecue", "bbq", "picnic", "playground", "mailbox", "birdbath", "planter", "hydrant", "dumpster", "pond", "gazebo", "umbrella", "pergola", "arbor", "pool ", "diving", "ladder", "fence", "gate", "driveway", "lawn", "scarecrow", "windmill", "waterfall", "rock", "boulder", "cart"]),
    ("Structure",   ["column", "pillar", "elevator", "stair", "railing", "beam", "arch", "awning", "foundation"]),
]


# things that are priced/typed like furniture but are inventory items, scripts or test leftovers
JUNK_PREFIX = ("token", "npc", "controller", "jobdata", "memory", "reaction", "social", "test", "script", "uskel", "aspiration",
               "daterewar", "potion", "reagent", "fish", "magazine", "book", "souvenir", "map", "award", "food", "accessory",
               "shopping", "paul", "bonfire fx", "stack", "emitter", "career", "phone call", "learned", "spell", "witch")


def categorize(name):
    n = " " + name.lower() + " "
    for cat, kws in RULES:
        for k in kws:
            if k.endswith(" ") or k.endswith("-"):
                hit = (" " + k) in n or n.startswith(k)
            else:
                hit = (" " + k) in n or (k in n and len(k) >= 5)
            if hit:
                return CAT[cat]
    return CAT["Other"]


# OBJD +0x9E = the game's own Buy-mode sub-category id (10000+); map them onto our categories
def _r(a, b):
    return range(a, b + 1)

SUBMAP = {}
for cat, ids in [
    ("Seating", [*_r(10000, 10004), 10069]),
    ("Beds", [*_r(10006, 10008), 10094]),
    ("Surfaces", [*_r(10009, 10013), 10043, 10047]),
    ("Decorative", [*_r(10014, 10018), 10049, 10062, 10064, 10099]),
    ("Plumbing", [*_r(10019, 10022), 10096]),
    ("Appliances", [*_r(10023, 10032), 10090, 10112, 10113]),
    ("Electronics", [*_r(10033, 10040)]),
    ("Hobbies & Fun", [*_r(10041, 10042), *_r(10044, 10046), 10048, *_r(10054, 10060), 10065, 10068, *_r(10070, 10076),
                       10097, 10100, 10101, 10103, 10104, 10105, 10106, 10107, 10111]),
    ("Outdoors", [*_r(10050, 10053), *_r(10087, 10089), *_r(10091, 10093), 10095]),
    ("Pets", [*_r(10077, 10086), 10109]),
    ("Other", [10061, 10102, 10108]),
]:
    for i in ids:
        SUBMAP[i] = CAT[cat]


def categorize_item(fname, en, subid=0):
    if subid in SUBMAP:
        return SUBMAP[subid]
    c = categorize(fname)                       # EA file names follow "Type - Kind - Style"
    return c if c != CAT["Other"] else categorize(en)


def parse_ctss(b):
    """-> {lang: [rows]} where rows alternate name, description"""
    if len(b) < 68:
        return {}
    cnt = struct.unpack_from('<H', b, 66)[0]
    p = 68
    rows = collections.defaultdict(list)
    for _ in range(cnt):
        if p >= len(b):
            break
        lang = b[p]; p += 1
        try:
            e1 = b.index(b'\0', p)
            t = b[p:e1].decode('utf-8', 'replace'); p = e1 + 1
            e2 = b.index(b'\0', p)
            p = e2 + 1
        except ValueError:
            break
        rows[lang].append(t)
    return rows


def pick_name(rows, langs):
    for l in langs:
        r = rows.get(l)
        if r and r[0].strip():
            return r[0].strip()
    return ""


def find_thumb_packages(extra):
    home = os.path.expanduser("~")
    pats = [
        f"{home}/Documentos/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
        f"{home}/Documents/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
        f"{home}/Faugus/*/drive_c/users/*/Documents/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
        f"{home}/.local/share/Steam/steamapps/compatdata/*/pfx/drive_c/users/*/Documents/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
        f"{home}/.wine*/drive_c/users/*/Documents/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
        f"{home}/.cache/ts2coop/*/prefix/drive_c/users/*/Documents/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
        f"{home}/Games/*/drive_c/users/*/Documents/EA Games/The Sims 2/Thumbnails/ObjectThumbnails.package",
    ]
    out = []
    for p in pats:
        out += glob.glob(p)
    out += extra
    seen, res = set(), []
    for p in out:
        rp = os.path.realpath(p)
        if rp not in seen and os.path.exists(rp):
            seen.add(rp); res.append(rp)
    return res


def find_download_dirs(extra):
    """Custom content: every .../The Sims 2/Downloads folder we can find (Documents of any prefix) + --mods dirs."""
    home = os.path.expanduser("~")
    pats = [
        f"{home}/Documentos/EA Games/The Sims 2/Downloads",
        f"{home}/Documents/EA Games/The Sims 2/Downloads",
        f"{home}/Faugus/*/drive_c/users/*/Documents/EA Games/The Sims 2/Downloads",
        f"{home}/.local/share/Steam/steamapps/compatdata/*/pfx/drive_c/users/*/Documents/EA Games/The Sims 2/Downloads",
        f"{home}/.wine*/drive_c/users/*/Documents/EA Games/The Sims 2/Downloads",
        f"{home}/Games/*/drive_c/users/*/Documents/EA Games/The Sims 2/Downloads",
    ]
    out = []
    for p in pats:
        out += glob.glob(p)
    out += extra
    seen, res = set(), []
    for p in out:
        rp = os.path.realpath(p)
        if rp not in seen and os.path.isdir(rp):
            seen.add(rp); res.append(rp)
    return res


def to565(im):
    im = im.convert("RGB").resize((THUMB, THUMB), Image.LANCZOS)
    px = im.load()
    out = bytearray()
    for y in range(THUMB):
        for x in range(THUMB):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += struct.pack('<H', v)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default="/mnt/NVME-JOGOS/The Sims 2")
    ap.add_argument("--out", default=None, help="output dir (default: <game>/EP9/TSBin)")
    ap.add_argument("--thumbs", nargs="*", default=[], help="extra ObjectThumbnails.package files")
    ap.add_argument("--mods", nargs="*", default=[], help="extra folders with custom content (.package)")
    ap.add_argument("--no-mods", action="store_true", help="skip custom content")
    a = ap.parse_args()
    out_dir = a.out or os.path.join(a.game, "EP9", "TSBin")

    pkgs = sorted(glob.glob(os.path.join(a.game, "*", "TSData", "Res", "Objects", "objects.package")))
    # later packs (EPs) override earlier ones for the same GUID, like the game does
    order = {"Base": 0}
    pkgs.sort(key=lambda p: (order.get(p.split(os.sep)[-5], 1), p))
    print("packages:", len(pkgs))

    items = {}          # guid -> dict

    def scan(pk, is_mod, src):
        try:
            f, es = dbpf.load(pk)
        except Exception as ex:
            print("skip", pk, ex); return
        if is_mod and not any(e[0] == T_OBJD for e in es):
            return
        ctss = {(e[1], e[2]): e for e in es if e[0] == T_CTSS}
        for e in es:
            if e[0] != T_OBJD:
                continue
            try:
                b = dbpf.get(f, e)
            except Exception:
                continue
            if len(b) < 0x100:
                continue
            fname = b[:64].split(b'\0')[0].decode('latin1')
            typ = struct.unpack_from('<H', b, 0x52)[0]
            price = struct.unpack_from('<H', b, 0x64)[0]
            master = struct.unpack_from('<H', b, 0x54)[0]
            subidx = struct.unpack_from('<H', b, 0x56)[0]
            guid = struct.unpack_from('<I', b, 0x5C)[0]
            ctssId = struct.unpack_from('<H', b, 0x92)[0]
            subid = struct.unpack_from('<H', b, 0x9E)[0]
            if guid == 0 or typ not in (4, 5, 19) or price <= 1:
                continue
            if is_mod and guid in items:
                continue            # default replacement of a game object: keep the game entry
            if master != 0 and subidx != 0xFFFF:
                continue            # secondary tile of a multi-tile object
            if "rack obj" in fname.lower() or fname.lower().startswith(JUNK_PREFIX) or "placeholder" in fname.lower() or "roof" in fname.lower():
                continue
            ce = ctss.get((e[1], ctssId))
            try:
                rows = parse_ctss(dbpf.get(f, ce)) if ce else {}
            except Exception:
                rows = {}
            en = pick_name(rows, [1, 2]) or fname
            pt = pick_name(rows, [35, 14]) or ""
            if pt == en:
                pt = ""
            items[guid] = dict(guid=guid, group=e[1], price=price, en=en, pt=pt, fname=fname, typ=typ,
                               cat=categorize_item(fname, en, subid), mod=is_mod, src=src)

    for pk in pkgs:
        scan(pk, False, "")
    nbase = len(items)
    print("game items:", nbase)
    if not a.no_mods:
        for d in find_download_dirs(a.mods):
            files = glob.glob(os.path.join(d, "**", "*.package"), recursive=True)
            print("mods dir:", d, len(files), "packages")
            for pk in sorted(files):
                scan(pk, True, os.path.relpath(pk, d))
    print("catalog items:", len(items), "(mods:", len(items) - nbase, ")")

    # ---- thumbnails (group -> jpeg) ----------------------------------------------------------
    thumb_src = {}
    if Image is None:
        print("PIL missing: no thumbnails")
    else:
        for tp in find_thumb_packages(a.thumbs):
            try:
                f, es = dbpf.load(tp)
            except Exception as ex:
                print("skip", tp, ex); continue
            n = 0
            for e in es:
                if e[0] == T_THUMB and e[3]:
                    if e[3] not in thumb_src:
                        thumb_src[e[3]] = (f, e); n += 1
            print("thumbs:", tp, "->", n, "new groups")
    blobs, order_list = [], sorted(items.values(), key=lambda it: (it["cat"], it["en"].lower()))
    nth = 0
    for it in order_list:
        it["thumb"] = 0xFFFFFFFF
        src = thumb_src.get(it["group"])
        if not src:
            continue
        try:
            im = Image.open(io.BytesIO(dbpf.get(*src))); im.load()
            blobs.append(to565(im)); it["thumb"] = nth; nth += 1
        except Exception:
            pass
    print("thumbnails:", nth, "of", len(order_list))

    # ---- write -------------------------------------------------------------------------------
    names = bytearray(b'\0')
    def add(s):
        if not s:
            return 0
        off = len(names); names.extend(s.encode('utf-8') + b'\0'); return off
    recs = bytearray()
    for it in order_list:
        recs += struct.pack('<IIHBBIIII', it["guid"], it["group"], min(it["price"], 65535), it["cat"], 1 if it["mod"] else 0,
                            add(it["en"]), add(it["pt"]), it["thumb"], add(os.path.basename(it["src"])) if it["mod"] else 0)
    cats = bytearray()
    for en, pt in CATS:
        cats += struct.pack('<II', add(en), add(pt))
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "ts2catalog.bin"), "wb") as fo:
        # header: magic, version, count, catCount, recSize, offCats, offRecs, offNames, namesLen
        hdr_len = 9 * 4
        offCats = hdr_len
        offRecs = offCats + len(cats)
        offNames = offRecs + len(recs)
        fo.write(struct.pack('<4sIIIIIIII', b'TS2C', 1, len(order_list), len(CATS), 28, offCats, offRecs, offNames, len(names)))
        fo.write(cats); fo.write(recs); fo.write(names)
    with open(os.path.join(out_dir, "ts2thumbs.bin"), "wb") as fo:
        fo.write(struct.pack('<4sIII', b'TS2T', 1, nth, THUMB))
        for bl in blobs:
            fo.write(bl)
    print("wrote", os.path.join(out_dir, "ts2catalog.bin"), os.path.getsize(os.path.join(out_dir, "ts2catalog.bin")), "bytes")
    print("wrote", os.path.join(out_dir, "ts2thumbs.bin"), os.path.getsize(os.path.join(out_dir, "ts2thumbs.bin")), "bytes")
    h = collections.Counter(it["cat"] for it in order_list)
    for i, (en, _) in enumerate(CATS):
        print("  %-16s %d" % (en, h.get(i, 0)))
    oth = collections.Counter(it["fname"].split(" - ")[0].strip() for it in order_list if it["cat"] == CAT["Other"])
    print("Other by file prefix:", oth.most_common(70))


if __name__ == "__main__":
    main()
