# edges.py REF.dat OTHER.dat REF_TXT T_FROM : engines.compare's measures, in
# full -- every output edge (1.5 V crossing) of the reference with the other
# run's shift (ns, other - ref), and each output pin's largest settled
# difference with the time it occurs
import bisect, json, sys
sys.path.insert(0, "/home/rz/z80-from-schematics.fable-sim-engines/asic/spice")
import chip
ref, oth, ref_txt, t_from = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
tr, wr, _ = chip.load(ref); to, wo, _ = chip.load(oth)
tr = [x + t_from for x in tr] if tr[0] < t_from - 1 else tr
to = [x + t_from for x in to] if to[0] < t_from - 1 else to
L = chip.OUT_LEVEL
edges = []
for p in wr:
    if p in chip.INPUTS:
        continue
    for k in range(1, len(tr)):
        a, b = wr[p][k - 1], wr[p][k]
        if (a - L) * (b - L) < 0 and tr[k] <= to[-1]:
            tx = tr[k - 1] + (L - a) * (tr[k] - tr[k - 1]) / (b - a)
            near = [to[j - 1] + (L - wo[p][j - 1]) * (to[j] - to[j - 1]) / (wo[p][j] - wo[p][j - 1])
                    for j in range(1, len(to)) if abs(to[j] - tx) < 30
                    and (wo[p][j - 1] - L) * (wo[p][j] - L) < 0 and (wo[p][j] > wo[p][j - 1]) == (b > a)]
            near_j = [j for j in range(1, len(to)) if abs(to[j] - tx) < 30
                      and (wo[p][j - 1] - L) * (wo[p][j] - L) < 0 and (wo[p][j] > wo[p][j - 1]) == (b > a)]
            c = min(near, key=lambda x: abs(x - tx)) if near else None
            jo = min(near_j, key=lambda j: abs(to[j] - tx)) if near_j else None
            edges.append({"pin": p, "dir": "rise" if b > a else "fall", "t_ref_ns": round(tx, 3),
                          "shift_ps": None if c is None else round((c - tx) * 1000, 1),
                          # the steps the 1.5 V crossing is interpolated across (ps): reference, other
                          "bracket_ps": [round((tr[k] - tr[k - 1]) * 1000), None if jo is None else round((to[jo] - to[jo - 1]) * 1000)]})
settled, interp = {}, {}
for p in wr:
    if p in chip.INPUTS:
        continue
    best = (0.0, None, None, None)
    for k in range(1, len(tr) - 1, max(1, len(tr) // 4000)):
        if tr[k] < tr[0] + 25 or tr[k] > to[-1]:
            continue
        if abs(wr[p][k + 1] - wr[p][k - 1]) / max(tr[k + 1] - tr[k - 1], 1e-6) > 1e-3:
            continue
        v = chip.value_at(to, wo[p], tr[k])        # as engines.compare: the other's next point
        d = abs(v - wr[p][k])
        if d > best[0]:
            best = (round(d, 4), round(tr[k], 2), round(wr[p][k], 4), round(v, 4))
        j = min(max(bisect.bisect_left(to, tr[k]), 1), len(to) - 1)   # linear between its points
        vi = wo[p][j - 1] + (wo[p][j] - wo[p][j - 1]) * (tr[k] - to[j - 1]) / max(to[j] - to[j - 1], 1e-12)
        if abs(vi - wr[p][k]) > interp.get(p, (0,))[0]:
            interp[p] = (round(abs(vi - wr[p][k]), 4), round(tr[k], 2))
    settled[p] = best
top = sorted(settled.items(), key=lambda kv: -kv[1][0])[:6]
itop = sorted(interp.items(), key=lambda kv: -kv[1][0])[:3]
print(json.dumps({"settled_interpolated_top": [{"pin": p, "dV": v[0], "t_ns": v[1]} for p, v in itop], "edges": edges, "settled_top": [{"pin": p, "dV": v[0], "t_ns": v[1], "ref_V": v[2], "other_V": v[3]} for p, v in top]}, indent=1))
