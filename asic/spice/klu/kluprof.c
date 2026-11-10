/* kluprof A.mm [B.mm] : KLU on a Matrix Market matrix -- analyze, factor,
   refactor (with B's values, same pattern), solve: times, fill, BTF blocks,
   for ordering AMD / COLAMD and BTF on / off */
/* build (nixpkgs as pads.py pins it: suitesparse, suitesparse.dev, gcc):
     gcc -O2 -o kluprof kluprof.c -I$SSDEV/include/suitesparse -L$SS/lib \
         -lklu -lbtf -lamd -lcolamd -lsuitesparseconfig -Wl,-rpath,$SS/lib
   the matrices: Xyce's `.options linsol output_ls=1` (Transformed_Matrix<k>.mm) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "klu.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

typedef struct { int n, nz; int *Ap, *Ai; double *Ax; } csc;

static csc readmm(const char *f) {
    FILE *fp = fopen(f, "r"); char line[512]; csc A; int m, nnz;
    if (!fp) { perror(f); exit(1); }
    do { if (!fgets(line, sizeof line, fp)) exit(1); } while (line[0] == '%');
    sscanf(line, "%d %d %d", &m, &A.n, &nnz);
    int *I = malloc(nnz * sizeof(int)), *J = malloc(nnz * sizeof(int)); double *X = malloc(nnz * sizeof(double));
    for (int k = 0; k < nnz; k++) { if (fscanf(fp, "%d %d %lf", &I[k], &J[k], &X[k]) != 3) exit(2); I[k]--; J[k]--; }
    fclose(fp);
    A.Ap = calloc(A.n + 1, sizeof(int));
    for (int k = 0; k < nnz; k++) A.Ap[J[k] + 1]++;
    for (int j = 0; j < A.n; j++) A.Ap[j + 1] += A.Ap[j];
    int *w = malloc(A.n * sizeof(int)); memcpy(w, A.Ap, A.n * sizeof(int));
    A.Ai = malloc(nnz * sizeof(int)); A.Ax = malloc(nnz * sizeof(double));
    for (int k = 0; k < nnz; k++) { int p = w[J[k]]++; A.Ai[p] = I[k]; A.Ax[p] = X[k]; }
    /* sort each column by row (KLU wants no duplicates; sorting not needed) */
    A.nz = nnz; free(I); free(J); free(X); free(w);
    return A;
}

int main(int argc, char **argv) {
    csc A = readmm(argv[1]);
    csc B = argc > 2 ? readmm(argv[2]) : A;
    double *b = malloc(A.n * sizeof(double));
    printf("n %d nnz %d (%.1f per row)\n", A.n, A.nz, (double)A.nz / A.n);
    for (int ord = 0; ord <= (getenv("ONLYAMD") ? 0 : 1); ord++) for (int btf = 1; btf >= 0; btf--) {
        klu_common c; klu_defaults(&c); c.ordering = ord; c.btf = btf;
        double t0 = now(); klu_symbolic *S = klu_analyze(A.n, A.Ap, A.Ai, &c); double ta = now() - t0;
        if (!S) { printf("analyze failed %d\n", c.status); continue; }
        t0 = now(); klu_numeric *N = klu_factor(A.Ap, A.Ai, A.Ax, S, &c); double tf = now() - t0;
        if (!N) { printf("factor failed %d\n", c.status); continue; }
        klu_flops(S, N, &c); double flops = c.flops;
        t0 = now(); int ok = klu_refactor(B.Ap, B.Ai, B.Ax, S, N, &c); double tr = now() - t0;
        for (int i = 0; i < A.n; i++) b[i] = 1.0;
        t0 = now(); klu_solve(S, N, A.n, 1, b, &c); double ts = now() - t0;
        klu_rcond(S, N, &c);
        /* block sizes */
        int big = 0, n1 = 0, nbig = 0; for (int k = 0; k < S->nblocks; k++) { int s = S->R[k + 1] - S->R[k]; if (s > big) big = s; if (s == 1) n1++; if (s > 100) nbig++; }
        printf("%-6s btf=%d: blocks %d (1x1 %d, >100 %d, largest %d), off-diag nz %d | lnz %.0f unz %.0f -> L+U %.0f (%.1fx nnz) flops %.3g | analyze %.3f s factor %.3f s refactor %.3f s (rc %d) solve %.4f s rcond %.2e\n",
               ord ? "COLAMD" : "AMD", btf, S->nblocks, n1, nbig, big, S->nzoff, (double)N->lnz, (double)N->unz, (double)(N->lnz + N->unz),
               (double)(N->lnz + N->unz) / A.nz, flops, ta, tf, tr, ok, ts, c.rcond);
        fflush(stdout);
        klu_free_numeric(&N, &c); klu_free_symbolic(&S, &c);
    }
    return 0;
}
