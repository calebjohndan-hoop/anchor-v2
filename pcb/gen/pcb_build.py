"""Build a KiCad 7 PCB from a parts list and a placement, autoroute it with Freerouting, fill pours, run DRC, export fab files."""
import os, subprocess, sys, json
import pcbnew
from pcbnew import FromMM, ToMM, VECTOR2I

FP = "/usr/share/kicad/footprints"
FREEROUTING = "/tmp/claude-0/-home-claude/f1721c77-a1d5-52e9-9a9b-d95c036eef22/scratchpad/fr/fr.jar"
POWER_NETS = {"GND", "+3V3", "VBUS", "VSYS", "VSYS_SW", "VBAT", "SPK_L_P", "SPK_L_N", "SPK_R_P", "SPK_R_N"}


def pt(x, y):
    return VECTOR2I(FromMM(x), FromMM(y))


def rounded_outline(board, w, h, r):
    import math
    segs = []
    def line(a, b):
        s = pcbnew.PCB_SHAPE(board); s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pt(*a)); s.SetEnd(pt(*b)); s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(FromMM(0.1)); board.Add(s)
    def arc(c, start, mid, end):
        s = pcbnew.PCB_SHAPE(board); s.SetShape(pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(pt(*start), pt(*mid), pt(*end)); s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(FromMM(0.1)); board.Add(s)
    k = r * (1 - 1 / math.sqrt(2))
    line((r, 0), (w - r, 0)); line((w, r), (w, h - r)); line((w - r, h), (r, h)); line((0, h - r), (0, r))
    arc(None, (w - r, 0), (w - k, k), (w, r)); arc(None, (w, h - r), (w - k, h - k), (w - r, h))
    arc(None, (r, h), (k, h - k), (0, h - r)); arc(None, (0, r), (k, k), (r, 0))


def add_zone(board, net, layer, w, h, inset=0.3):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer); z.SetNet(net)
    poly = z.Outline(); poly.NewOutline()
    for x, y in [(inset, inset), (w - inset, inset), (w - inset, h - inset), (inset, h - inset)]:
        poly.Append(FromMM(x), FromMM(y))
    z.SetLocalClearance(FromMM(0.25)); z.SetMinThickness(FromMM(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
    z.SetThermalReliefGap(FromMM(0.25)); z.SetThermalReliefSpokeWidth(FromMM(0.3))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    board.Add(z)
    return z


def build(name, outdir, parts, place, size, corner=2.0, drop_rule_areas=(), route=True, passes=None, edge_keepout=0.8):
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, f"{name}.kicad_pcb")
    board = pcbnew.NewBoard(path)
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)
    ns = ds.m_NetSettings
    dflt = ns.m_DefaultNetClass
    dflt.SetTrackWidth(FromMM(0.2)); dflt.SetClearance(FromMM(0.18)); dflt.SetViaDiameter(FromMM(0.6)); dflt.SetViaDrill(FromMM(0.3))
    pw = pcbnew.NETCLASS("Power"); pw.SetTrackWidth(FromMM(0.4)); pw.SetClearance(FromMM(0.2)); pw.SetViaDiameter(FromMM(0.7)); pw.SetViaDrill(FromMM(0.35))
    ns.m_NetClasses["Power"] = pw
    ds.m_TrackMinWidth = FromMM(0.15); ds.m_MinClearance = FromMM(0.15); ds.m_ViasMinSize = FromMM(0.5); ds.m_MinThroughDrill = FromMM(0.2)
    ds.m_HoleToHoleMin = FromMM(0.25); ds.m_CopperEdgeClearance = FromMM(0.3)

    nets = {}
    for p in parts:
        for net in p.pin_net().values():
            if net and net not in nets:
                ni = pcbnew.NETINFO_ITEM(board, net); board.Add(ni); nets[net] = ni
    for net in nets:
        if net in POWER_NETS:
            ns.SetNetclassPatternAssignment(net, "Power") if hasattr(ns, "SetNetclassPatternAssignment") else None
    fps = {}
    for p in parts:
        lib, fpn = p.footprint.split(":")
        fp = pcbnew.FootprintLoad(f"{FP}/{lib}.pretty", fpn)
        fp.SetReference(p.ref); fp.SetValue(p.value)
        ref = fp.Reference(); ref.SetTextSize(VECTOR2I(FromMM(0.8), FromMM(0.8))); ref.SetTextThickness(FromMM(0.12))
        for k in ("MPN", "LCSC"):
            v = getattr(p, k.lower())
            if v:
                fp.SetField(k, v) if hasattr(fp, "SetField") else None
        x, y, rot, side = place[p.ref]
        board.Add(fp)
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        if side == "B":
            fp.Flip(pt(x, y), False)
        if p.ref in drop_rule_areas:
            # radio is not used: drop the antenna keepout and the antenna clearance courtyard, keep a body courtyard
            for z in list(fp.Zones()):
                fp.Remove(z)
            for g in list(fp.GraphicalItems()):
                if "Courtyard" in g.GetLayerName():
                    fp.Remove(g)
            c = pcbnew.FP_SHAPE(fp) if hasattr(pcbnew, "FP_SHAPE") else pcbnew.PCB_SHAPE(fp)
            c.SetShape(pcbnew.SHAPE_T_RECT); c.SetLayer(pcbnew.F_CrtYd); c.SetWidth(FromMM(0.05))
            c.SetStart0(pt(-9.75, -13.05)) if hasattr(c, "SetStart0") else c.SetStart(pt(-9.75, -13.05))
            c.SetEnd0(pt(9.75, 13.45)) if hasattr(c, "SetEnd0") else c.SetEnd(pt(9.75, 13.45))
            fp.Add(c)
        pn = p.pin_net()
        for pad in fp.Pads():
            n = pad.GetNumber()
            if n in pn and pn[n]:
                pad.SetNet(nets[pn[n]])
        if fpn.startswith("InvenSense_ICS"):
            for g in list(fp.GraphicalItems()):
                if g.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu):
                    fp.Remove(g)
        fps[p.ref] = fp
    w, h = size
    rounded_outline(board, w, h, corner)
    # keep tracks and vias 0.8 mm from the edge (Freerouting ignores the edge clearance rule)
    m = edge_keepout
    for x0, y0, x1, y1 in [(0, 0, w, m), (0, h - m, w, h), (0, 0, m, h), (w - m, 0, w, h)]:
        z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(False); z.SetDoNotAllowCopperPour(False)
        ls = pcbnew.LSET(); ls.AddLayer(pcbnew.F_Cu); ls.AddLayer(pcbnew.B_Cu); z.SetLayerSet(ls)
        o = z.Outline(); o.NewOutline()
        for x, y in [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]:
            o.Append(FromMM(x), FromMM(y))
        board.Add(z)
    board.Save(path)
    return path


def add_pours(path, size, both=True):
    board = pcbnew.LoadBoard(path)
    nets = board.GetNetsByName()
    if nets.has_key("GND"):
        w, h = size
        add_zone(board, nets["GND"], pcbnew.F_Cu, w, h)
        if both:
            add_zone(board, nets["GND"], pcbnew.B_Cu, w, h)
    board.Save(path)


def autoroute(path, passes=100, timeout=900):
    board = pcbnew.LoadBoard(path)
    dsn = path.replace(".kicad_pcb", ".dsn"); ses = path.replace(".kicad_pcb", ".ses")
    pcbnew.ExportSpecctraDSN(board, dsn)
    cmd = ["xvfb-run", "-a", "java", "-jar", FREEROUTING, "-de", dsn, "-do", ses, "-mp", str(passes), "-dct", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if not os.path.exists(ses):
        print(r.stdout[-2000:], r.stderr[-2000:]); raise SystemExit("freerouting produced no SES")
    board = pcbnew.LoadBoard(path)
    n_tr, n_via = import_ses(board, ses)
    board.Save(path)
    return r.stdout[-1500:] + f"\nimported {n_tr} track segments, {n_via} vias"


def import_ses(board, ses):
    """Add Freerouting's routes to the board (KiCad 7's ImportSpecctraSES needs the GUI)."""
    import re
    from anchor_eda import parse, unq
    root = parse(open(ses).read())
    def find(node, key):
        return [i for i in node if isinstance(i, list) and i and i[0] == key]
    routes = find(root, "routes")[0]
    res = float(find(routes, "resolution")[0][2])  # units per micrometre
    k = 1.0 / (res * 1000.0)  # -> mm
    layers = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu}
    nets = board.GetNetsByName()
    n_tr = n_via = 0
    have = {(round(ToMM(t.GetPosition().x), 2), round(ToMM(t.GetPosition().y), 2)) for t in board.GetTracks() if t.GetClass() == "PCB_VIA"}
    for net in find(find(routes, "network_out")[0], "net"):
        name = unq(net[1])
        ni = nets[name] if nets.has_key(name) else None
        for w in find(net, "wire"):
            for path in find(w, "path"):
                layer, width = layers[unq(path[1])], float(path[2]) * k
                xy = [float(v) * k for v in path[3:]]
                pts_ = [(xy[i], -xy[i + 1]) for i in range(0, len(xy), 2)]
                for a, b in zip(pts_, pts_[1:]):
                    t = pcbnew.PCB_TRACK(board); t.SetStart(pt(*a)); t.SetEnd(pt(*b)); t.SetWidth(FromMM(width)); t.SetLayer(layer)
                    if ni: t.SetNet(ni)
                    board.Add(t); n_tr += 1
        for v in find(net, "via"):
            m = re.search(r"_(\d+):(\d+)_um", unq(v[1]))
            dia, drill = (int(m.group(1)) / 1000, int(m.group(2)) / 1000) if m else (0.6, 0.3)
            x, y = float(v[2]) * k, -float(v[3]) * k
            if (round(x, 2), round(y, 2)) in have:
                continue
            have.add((round(x, 2), round(y, 2)))
            via = pcbnew.PCB_VIA(board); via.SetPosition(pt(x, y)); via.SetWidth(FromMM(dia)); via.SetDrill(FromMM(drill))
            via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            if ni: via.SetNet(ni)
            board.Add(via); n_via += 1
    return n_tr, n_via


def finish(path):
    board = pcbnew.LoadBoard(path)
    filler = pcbnew.ZONE_FILLER(board); filler.Fill(board.Zones())
    board.Save(path)
    rpt = path.replace(".kicad_pcb", "-drc.rpt")
    pcbnew.WriteDRCReport(board, rpt, pcbnew.EDA_UNITS_MILLIMETRES, True)
    txt = open(rpt).read()
    unrouted = sum(1 for _ in [0]) and board.GetConnectivity().GetUnconnectedCount(False) if hasattr(board.GetConnectivity(), "GetUnconnectedCount") else -1
    return txt, unrouted


def exports(path, outdir):
    fab = os.path.join(outdir, "fab"); os.makedirs(fab, exist_ok=True)
    run = lambda *a: subprocess.run(["kicad-cli", "pcb", "export", *a, path], check=True, capture_output=True)
    run("gerbers", "-o", fab + "/", "--layers", "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts")
    run("drill", "-o", fab + "/")
    run("pos", "-o", os.path.join(fab, "positions.csv"), "--format", "csv", "--units", "mm", "--side", "both")
    run("svg", "-o", os.path.join(outdir, "pcb-front.svg"), "--layers", "F.Cu,F.Silkscreen,F.Mask,Edge.Cuts", "--page-size-mode", "2", "--exclude-drawing-sheet")
    run("svg", "-o", os.path.join(outdir, "pcb-back.svg"), "--layers", "B.Cu,B.Silkscreen,Edge.Cuts", "--page-size-mode", "2", "--exclude-drawing-sheet", "--mirror")
    run("svg", "-o", os.path.join(outdir, "pcb-placement.svg"), "--layers", "F.Fab,F.Courtyard,Edge.Cuts,F.Silkscreen", "--page-size-mode", "2", "--exclude-drawing-sheet")


def add_track(path, pts_mm, net, layer="F.Cu", width=0.25):
    board = pcbnew.LoadBoard(path)
    ni = board.GetNetsByName()[net]
    L = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu}[layer]
    for a, b in zip(pts_mm, pts_mm[1:]):
        t = pcbnew.PCB_TRACK(board); t.SetStart(pt(*a)); t.SetEnd(pt(*b)); t.SetWidth(FromMM(width)); t.SetLayer(L); t.SetNet(ni); board.Add(t)
    board.Save(path)


def add_via(path, xy, net, dia=0.6, drill=0.3):
    board = pcbnew.LoadBoard(path)
    v = pcbnew.PCB_VIA(board); v.SetPosition(pt(*xy)); v.SetWidth(FromMM(dia)); v.SetDrill(FromMM(drill))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(board.GetNetsByName()[net]); board.Add(v); board.Save(path)
