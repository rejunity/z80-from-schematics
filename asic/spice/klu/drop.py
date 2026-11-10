# drop.py IN.mm OUT.mm IDX... : the matrix without those rows and columns
# (each made an ideal, known voltage) -- a diagnostic, not a circuit
import sys, numpy as np
src, dst, drop = sys.argv[1], sys.argv[2], set(int(x) for x in sys.argv[3:])
with open(src) as fp:
    for l in fp:
        if l[0] != '%': n, m, nz = map(int, l.split()); break
    d = np.loadtxt(fp)
r = d[:, 0].astype(int) - 1; c = d[:, 1].astype(int) - 1
keep = np.ones(n, bool); keep[list(drop)] = False
new = np.cumsum(keep) - 1
k = keep[r] & keep[c]
with open(dst, "w") as f:
    f.write("%%%%MatrixMarket matrix coordinate real general\n%d %d %d\n" % (keep.sum(), keep.sum(), k.sum()))
    np.savetxt(f, np.column_stack([new[r[k]] + 1, new[c[k]] + 1, d[k, 2]]), fmt="%d %d %.17g")
