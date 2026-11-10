/* kluorder A.mm B.mm : KLU with fill-reducing orderings from CHOLMOD on A+A'
   (AMD, METIS, NESDIS), given to klu_analyze_given (no BTF): fill, flops,
   factor / refactor time; and UMFPACK (multifrontal, BLAS-3) on the same matrix */
/* build (nixpkgs as pads.py pins it: suitesparse, suitesparse.dev, gcc):
     gcc -O2 -o kluorder kluorder.c -I$SSDEV/include/suitesparse -L$SS/lib \
         -lklu -lbtf -lamd -lcolamd -lcholmod -lumfpack -lsuitesparseconfig -Wl,-rpath,$SS/lib
   the matrices: Xyce's `.options linsol output_ls=1` (Transformed_Matrix<k>.mm) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "klu.h"
#include "cholmod.h"
#include "umfpack.h"

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
    A.nz = nnz; free(I); free(J); free(X); free(w);
    return A;
}
int main(int argc, char **argv) {
    csc A = readmm(argv[1]), B = readmm(argv[2]);
    int n = A.n;
    cholmod_common cc; cholmod_start(&cc);
    cholmod_sparse *M = cholmod_allocate_sparse(n, n, A.nz, 0, 1, 0, CHOLMOD_PATTERN, &cc);
    memcpy(M->p, A.Ap, (n + 1) * sizeof(int)); memcpy(M->i, A.Ai, A.nz * sizeof(int));
    const char *nm[] = {"AMD", "METIS", "NESDIS", "COLAMD"}; int meth[] = {CHOLMOD_AMD, CHOLMOD_METIS, CHOLMOD_NESDIS, CHOLMOD_COLAMD};
    double *b = malloc(n * sizeof(double));
    for (int m = 0; m < 3; m++) {
        cc.nmethods = 1; cc.method[0].ordering = meth[m]; cc.postorder = 1;
        int *P = malloc(n * sizeof(int));
        double t0 = now();
        /* the pattern of A+A' (unsymmetric input: cholmod orders A*A' unless stype; use stype on A+A') */
        cholmod_sparse *At = cholmod_transpose(M, 0, &cc);
        cholmod_sparse *S = cholmod_add(M, At, NULL, NULL, 0, 0, &cc); S->stype = 1;
        cholmod_factor *L = cholmod_analyze(S, &cc);
        double to = now() - t0;
        memcpy(P, L->Perm, n * sizeof(int));
        double chol_lnz = cc.lnz, chol_fl = cc.fl;
        klu_common c; klu_defaults(&c); c.btf = 0;
        klu_symbolic *Sy = klu_analyze_given(n, A.Ap, A.Ai, P, P, &c);
        t0 = now(); klu_numeric *N = klu_factor(A.Ap, A.Ai, A.Ax, Sy, &c); double tf = now() - t0;
        if (!N) { printf("%s: factor failed %d\n", nm[m], c.status); continue; }
        klu_flops(Sy, N, &c);
        t0 = now(); klu_refactor(B.Ap, B.Ai, B.Ax, Sy, N, &c); double tr = now() - t0;
        printf("KLU %-6s (cholmod order %.2f s; its lnz %.3g fl %.3g): L+U %.0f (%.1fx) flops %.3g factor %.3f s refactor %.3f s\n",
               nm[m], to, chol_lnz, chol_fl, (double)(N->lnz + N->unz), (double)(N->lnz + N->unz) / A.nz, c.flops, tf, tr);
        fflush(stdout);
        klu_free_numeric(&N, &c); klu_free_symbolic(&Sy, &c);
        cholmod_free_factor(&L, &cc); cholmod_free_sparse(&S, &cc); cholmod_free_sparse(&At, &cc); free(P);
    }
    /* UMFPACK: multifrontal, BLAS-3 */
    void *Sym, *Num; double Info[UMFPACK_INFO], Ctl[UMFPACK_CONTROL];
    umfpack_di_defaults(Ctl);
    double t0 = now(); umfpack_di_symbolic(n, n, A.Ap, A.Ai, A.Ax, &Sym, Ctl, Info); double ts = now() - t0;
    t0 = now(); int st = umfpack_di_numeric(A.Ap, A.Ai, A.Ax, Sym, &Num, Ctl, Info); double tn = now() - t0;
    printf("UMFPACK: status %d symbolic %.2f s numeric %.3f s, L+U %.0f flops %.3g, ordering %s\n", st, ts, tn,
           Info[UMFPACK_LNZ] + Info[UMFPACK_UNZ], Info[UMFPACK_FLOPS], Info[UMFPACK_ORDERING_USED] == UMFPACK_ORDERING_AMD ? "AMD" : Info[UMFPACK_ORDERING_USED] == UMFPACK_ORDERING_METIS ? "METIS" : "other");
    umfpack_di_free_numeric(&Num);
    t0 = now(); st = umfpack_di_numeric(B.Ap, B.Ai, B.Ax, Sym, &Num, Ctl, Info); tn = now() - t0;
    printf("UMFPACK: second numeric %.3f s\n", tn);
    Ctl[UMFPACK_ORDERING] = UMFPACK_ORDERING_METIS; umfpack_di_free_symbolic(&Sym); umfpack_di_free_numeric(&Num);
    umfpack_di_symbolic(n, n, A.Ap, A.Ai, A.Ax, &Sym, Ctl, Info);
    t0 = now(); st = umfpack_di_numeric(A.Ap, A.Ai, A.Ax, Sym, &Num, Ctl, Info); tn = now() - t0;
    printf("UMFPACK METIS: numeric %.3f s, L+U %.0f flops %.3g\n", tn, Info[UMFPACK_LNZ] + Info[UMFPACK_UNZ], Info[UMFPACK_FLOPS]);
    return 0;
}
