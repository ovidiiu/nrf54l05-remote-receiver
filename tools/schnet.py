"""Minimal netlist extractor for EasyEDA/Altium ASCII .SchDoc files.

Usage: python3 -I schnet.py file.schdoc
Prints components and nets (pins joined by wires, net labels, power ports).
"""
import sys, re
from collections import defaultdict

def parse(path):
    recs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("|"):
                continue
            d = {}
            for kv in line.split("|")[1:]:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    d[k.replace("%UTF8%", "")] = v
            recs.append(d)
    return recs

def num(d, k):
    v = int(d.get(k, 0))
    fr = int(d.get(k + "_FRAC", 0) or 0)
    return v + fr / 100000.0

def main(path):
    recs = parse(path)
    comps = {}      # record index -> info
    # record index in file: Altium OWNERINDEX refers to index among records after header
    # (header line = index 0 is the HEADER record line? HEADER line has no RECORD).
    idx_map = {}
    for i, d in enumerate(recs):
        idx_map[i] = d
    # OWNERINDEX counts records excluding the HEADER line -> record i (0-based in recs incl header) has index i-1
    pins = []
    wires, labels, ports = [], [], []
    for i, d in enumerate(recs):
        r = d.get("RECORD")
        idx = i - 1
        if r == "1":
            comps[idx] = {"lib": d.get("LIBREFERENCE"), "desc": d.get("COMPONENTDESCRIPTION", "")[:60], "des": "?", "comment": "", "partid": d.get("CURRENTPARTID", "1")}
        elif r == "34" and d.get("NAME") == "Designator":
            o = int(d["OWNERINDEX"]); comps.setdefault(o, {})["des"] = d.get("TEXT", "?")
        elif r == "41" and d.get("NAME") in ("Comment", "Value") and "OWNERINDEX" in d:
            o = int(d["OWNERINDEX"])
            if d.get("TEXT"):
                comps.setdefault(o, {}).setdefault("vals", set()).add(d["TEXT"])
        elif r == "2":
            x, y = num(d, "LOCATION.X"), num(d, "LOCATION.Y")
            L = int(d.get("PINLENGTH", 0))
            rot = int(d.get("PINCONGLOMERATE", 0)) & 3
            dx, dy = [(L, 0), (0, L), (-L, 0), (0, -L)][rot]
            pins.append((int(d["OWNERINDEX"]), d.get("DESIGNATOR"), d.get("NAME"), (round(x + dx, 3), round(y + dy, 3))))
        elif r == "27":
            n = int(d.get("LOCATIONCOUNT", 0))
            pts = [(round(num(d, f"X{k}"), 3), round(num(d, f"Y{k}"), 3)) for k in range(1, n + 1)]
            wires.append(pts)
        elif r == "25":
            if d.get("TEXT"):
                labels.append((d["TEXT"], (num(d, "LOCATION.X"), num(d, "LOCATION.Y"))))
        elif r == "17":
            ports.append((d.get("TEXT"), (num(d, "LOCATION.X"), num(d, "LOCATION.Y"))))
        elif r == "29":  # junction
            pass

    # union-find over points
    parent = {}
    def find(a):
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    segs = []
    for pts in wires:
        for a, b in zip(pts, pts[1:]):
            segs.append((a, b)); union(a, b)
        if len(pts) == 1:
            find(pts[0])

    def on_seg(p, a, b):
        (x, y), (x1, y1), (x2, y2) = p, a, b
        if abs((x2 - x1) * (y - y1) - (y2 - y1) * (x - x1)) > 1e-6:
            return False
        return min(x1, x2) - 1e-6 <= x <= max(x1, x2) + 1e-6 and min(y1, y2) - 1e-6 <= y <= max(y1, y2) + 1e-6

    def attach(p):
        find(p)
        for a, b in segs:
            if on_seg(p, a, b):
                union(p, a)
        # wire endpoints that lie on other segments (T junctions)
    for a, b in list(segs):
        for p in (a, b):
            attach(p)
    for _, _, _, p in pins:
        attach(p)
    for _, p in labels + ports:
        attach(p)

    netname = defaultdict(set)
    for t, p in labels + ports:
        netname[find(p)].add(t)
    # merge same-named nets
    byname = {}
    for root, names in list(netname.items()):
        for n in names:
            if n in byname:
                union(root, byname[n])
            else:
                byname[n] = root
    netname2 = defaultdict(set)
    for t, p in labels + ports:
        netname2[find(p)].add(t)

    nets = defaultdict(list)
    for owner, pd, pn, p in pins:
        c = comps.get(owner, {})
        nets[find(p)].append(f"{c.get('des','?')}.{pd}({pn})")

    print("== Components ==")
    for o, c in sorted(comps.items(), key=lambda kv: kv[1].get("des", "")):
        if "lib" in c:
            print(f"{c.get('des'):10s} {c.get('lib'):28s} {'/'.join(sorted(c.get('vals', [])))[:20]:20s} {c.get('desc','')}")
    print("\n== Nets ==")
    unnamed = 0
    for root, members in sorted(nets.items(), key=lambda kv: (",".join(sorted(netname2.get(kv[0], {'~'}))), kv[1])):
        name = ",".join(sorted(netname2.get(root, []))) or f"(unnamed{unnamed})"
        if not netname2.get(root):
            unnamed += 1
        tag = "  <-- single pin" if len(members) == 1 else ""
        print(f"{name:14s}: {' '.join(sorted(members))}{tag}")
    # labels / ports with no pins
    pinroots = set(nets)
    for t, p in labels + ports:
        if find(p) not in pinroots:
            print(f"!! label/port '{t}' at {p} not connected to any pin")

if __name__ == "__main__":
    main(sys.argv[1])
