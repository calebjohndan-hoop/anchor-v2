import sys, re, json, boards, place, pcb_build as pb
from collections import Counter
which = sys.argv[1:] or ["mic", "hub", "main"]
for b in which:
    cfg = {"mic": place.MIC, "hub": place.HUB, "main": place.MAIN}[b]
    parts = boards.BOARDS[b][1]()
    path = pb.build(b, f"out/{b}", parts, cfg["place"], cfg["size"], drop_rule_areas=("U4",), edge_keepout=(0.45 if b == "mic" else 0.8))
    if b == "mic":  # the ICS-43434 GND ring is a custom pad the router skips: pre-route a stub and via
        pb.add_track(path, [(4.66, 3.71), (4.66, 5.1)], "GND", width=0.25)
    log = pb.autoroute(path, passes=(300 if b == "main" else 100), timeout=1500)
    pb.add_pours(path, cfg["size"], both=(b == "main"))
    txt, _ = pb.finish(path)
    c = Counter(re.findall(r"^\[(\w+)\]", txt, flags=re.M))
    print("==", b, dict(c)); print(log[-600:])
    sys.stdout.flush()
