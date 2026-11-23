"""Small helper to generate KiCad 7 schematics (label-connected) and PCBs from a parts list.

Each part: ref, lib_id ("Lib:Symbol"), value, footprint ("Lib:Footprint"), conns {pin number or name: net or None (no connect)}.
"""
import math, os, re, subprocess, uuid, json

SYMDIR = "/usr/share/kicad/symbols"
FPDIR = "/usr/share/kicad/footprints"

# ---------------------------------------------------------------- s-expressions
def parse(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack = [[]]
    for t in tok:
        if t == "(":
            stack.append([])
        elif t == ")":
            x = stack.pop(); stack[-1].append(x)
        else:
            stack[-1].append(t)
    return stack[0][0]

def dump(x, ind=0):
    if isinstance(x, list):
        inner = " ".join(dump(i, ind + 1) for i in x)
        return "(" + inner + ")"
    return x

def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'

def uid():
    return str(uuid.uuid4())

_libcache = {}
def lib(libname):
    if libname not in _libcache:
        t = open(f"{SYMDIR}/{libname}.kicad_sym", encoding="utf8").read()
        root = parse(t)
        _libcache[libname] = {unq(s[1]): s for s in root if isinstance(s, list) and s and s[0] == "symbol"}
    return _libcache[libname]

def unq(s):
    return s[1:-1] if s.startswith('"') else s

def flat_symbol(libname, name):
    """Return the symbol s-expr flattened (extends resolved), named libname:name."""
    syms = lib(libname)
    s = syms[name]
    ext = [i for i in s if isinstance(i, list) and i and i[0] == "extends"]
    if ext:
        parent = flat_symbol(libname, unq(ext[0][1]))
        pname = unq(parent[1]).split(":", 1)[1]
        child_props = {unq(i[1]): i for i in s if isinstance(i, list) and i and i[0] == "property"}
        out = ["symbol", q(f"{libname}:{name}")]
        for i in parent[2:]:
            if isinstance(i, list) and i and i[0] == "property" and unq(i[1]) in child_props:
                out.append(child_props.pop(unq(i[1])))
            elif isinstance(i, list) and i and i[0] == "symbol":
                sub = list(i); sub[1] = q(unq(sub[1]).replace(pname, name, 1)); out.append(sub)
            else:
                out.append(i)
        for p in child_props.values():
            out.insert(2, p)
        return out
    out = list(s); out[1] = q(f"{libname}:{name}")
    return out

def pins_of(flat):
    """[(number, name, x, y, angle, length)] for unit 0/1 pins."""
    res = []
    for sub in flat:
        if isinstance(sub, list) and sub and sub[0] == "symbol":
            nm = unq(sub[1]); unit = int(nm.rsplit("_", 2)[1])
            if unit not in (0, 1):
                continue
            for p in sub:
                if isinstance(p, list) and p and p[0] == "pin":
                    at = [i for i in p if isinstance(i, list) and i[0] == "at"][0]
                    ln = [i for i in p if isinstance(i, list) and i[0] == "length"][0]
                    name = [i for i in p if isinstance(i, list) and i[0] == "name"][0]
                    num = [i for i in p if isinstance(i, list) and i[0] == "number"][0]
                    res.append((unq(num[1]), unq(name[1]), float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0, float(ln[1])))
    return res

# ---------------------------------------------------------------- schematic
class Part:
    def __init__(self, ref, lib_id, value, footprint, conns, mpn="", lcsc=""):
        self.ref, self.lib_id, self.value, self.footprint, self.conns = ref, lib_id, value, footprint, conns
        self.mpn, self.lcsc = mpn, lcsc
        libname, name = lib_id.split(":")
        self.flat = flat_symbol(libname, name)
        self.pins = pins_of(self.flat)

    def pin_net(self):
        """number -> net (None = no connect). Conns keyed by number or name."""
        out = {}
        bynum = {p[0]: p for p in self.pins}
        for k, net in self.conns.items():
            hits = [p for p in self.pins if p[0] == k] or [p for p in self.pins if p[1] == k]
            if not hits:
                raise KeyError(f"{self.ref}: no pin {k!r}; pins are {[(p[0], p[1]) for p in self.pins]}")
            for p in hits:
                out[p[0]] = net
        missing = [p for p in self.pins if p[0] not in out]
        if missing:
            raise KeyError(f"{self.ref}: unassigned pins {[(p[0], p[1]) for p in missing]}")
        return out

    def bbox(self):
        xs = [p[2] for p in self.pins] or [0]; ys = [p[3] for p in self.pins] or [0]
        return min(xs), min(ys), max(xs), max(ys)


def snap(v, g=1.27):
    return round(v / g) * g


def write_schematic(path, title, parts, page="A3"):
    proj = os.path.splitext(os.path.basename(path))[0]
    sch_uuid = uid()
    libsyms = {}
    for p in parts:
        libsyms[p.lib_id] = p.flat
    items = []
    # layout: rows, width from bbox + label room
    W = {"A4": 280, "A3": 400, "A2": 560}[page]
    x, y, rowh = 30.48, 40.64, 0
    for p in parts:
        x0, y0, x1, y1 = p.bbox()
        maxlab = max([len(n or "") for n in p.pin_net().values() if n] or [4])
        pad = 7.62 + maxlab * 1.27
        w = (x1 - x0) + 2 * pad; h = (y1 - y0) + 15.24
        if x + w > W:
            x = 30.48; y += rowh + 10.16; rowh = 0
        p.at = (snap(x + pad - x0, 2.54), snap(y + y1 + 5.08, 2.54))
        x += w + 5.08; rowh = max(rowh, h)
    for p in parts:
        X, Y = p.at
        pn = p.pin_net()
        pinuuids = " ".join(f'(pin {q(n)} (uuid {uid()}))' for n in sorted({pp[0] for pp in p.pins}))
        props = [("Reference", p.ref, 0, -1), ("Value", p.value, 0, 1), ("Footprint", p.footprint, 0, 2), ("Datasheet", "", 0, 3)]
        x0, y0, x1, y1 = p.bbox()
        prop_s = ""
        for i, (k, v, _, off) in enumerate(props):
            py = Y - y1 - 2.54 if k == "Reference" else (Y - y0 + 2.54 if k == "Value" else Y - y0 + 5.08)
            hide = " hide" if k in ("Footprint", "Datasheet") else ""
            prop_s += f'(property {q(k)} {q(v)} (at {X:.2f} {py:.2f} 0) (effects (font (size 1.27 1.27)){hide}))'
        if p.mpn:
            prop_s += f'(property "MPN" {q(p.mpn)} (at {X:.2f} {Y:.2f} 0) (effects (font (size 1.27 1.27)) hide))'
        if p.lcsc:
            prop_s += f'(property "LCSC" {q(p.lcsc)} (at {X:.2f} {Y:.2f} 0) (effects (font (size 1.27 1.27)) hide))'
        items.append(f'(symbol (lib_id {q(p.lib_id)}) (at {X:.2f} {Y:.2f} 0) (unit 1) (in_bom yes) (on_board yes) (dnp no) (uuid {uid()}) {prop_s} {pinuuids} '
                     f'(instances (project {q(proj)} (path "/{sch_uuid}" (reference {q(p.ref)}) (unit 1)))))')
        done = set()
        for num, name, px, py, ang, ln in p.pins:
            wx, wy = X + px, Y - py
            key = (round(wx, 2), round(wy, 2))
            net = pn[num]
            if key in done:
                continue
            done.add(key)
            if net is None:
                items.append(f'(no_connect (at {wx:.2f} {wy:.2f}) (uuid {uid()}))')
                continue
            a = math.radians(ang + 180)
            dx, dy = round(math.cos(a)), -round(math.sin(a))
            ex, ey = wx + dx * 2.54, wy + dy * 2.54
            items.append(f'(wire (pts (xy {wx:.2f} {wy:.2f}) (xy {ex:.2f} {ey:.2f})) (stroke (width 0) (type default)) (uuid {uid()}))')
            lang = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[(dx, dy)]
            just = "left bottom" if lang in (0, 90) else "right bottom"
            items.append(f'(label {q(net)} (at {ex:.2f} {ey:.2f} {lang}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {just})) (uuid {uid()}))')
    tb = f'(title_block (title {q(title)}) (company "ANCHOR v2") (comment 1 "Generated from anchor-v2/pcb/gen; nets are joined by labels with the same name"))'
    text = (f'(kicad_sch (version 20230121) (generator eeschema) (uuid {sch_uuid}) (paper {q(page)}) {tb} '
            f'(lib_symbols {" ".join(dump(s) for s in libsyms.values())}) {" ".join(items)} '
            f'(sheet_instances (path "/" (page "1"))))')
    with open(path, "w", encoding="utf8") as f:
        f.write(text)
    pro = os.path.splitext(path)[0] + ".kicad_pro"
    if not os.path.exists(pro):
        json.dump({"meta": {"filename": os.path.basename(pro), "version": 1}}, open(pro, "w"))


def export_netlist(sch, out):
    subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", out, sch], check=True, capture_output=True)
    root = parse(open(out, encoding="utf8").read())
    nets = {}
    for sec in root:
        if isinstance(sec, list) and sec[0] == "nets":
            for n in sec[1:]:
                name = unq([i for i in n if isinstance(i, list) and i[0] == "name"][0][1])
                nodes = [(unq(i[1][1]), unq(i[2][1])) for i in n if isinstance(i, list) and i[0] == "node"]
                nets[name] = nodes
    return nets


def check_netlist(parts, nets):
    """Compare intended connections with what KiCad read back from the schematic."""
    want = {}
    for p in parts:
        for num, net in p.pin_net().items():
            if net:
                want.setdefault(net, set()).add((p.ref, num))
    got = {}
    for name, nodes in nets.items():
        clean = name.lstrip("/")
        got[clean] = set(nodes)
    problems = []
    for net, nodes in want.items():
        g = got.get(net)
        if g is None:
            problems.append(f"net {net} missing"); continue
        if nodes != g:
            problems.append(f"net {net}: want {sorted(nodes - g)} extra {sorted(g - nodes)}")
        if len(nodes) < 2:
            problems.append(f"net {net} has only one pin {sorted(nodes)}")
    return problems
