# Xyce 7.10 (serial) against a Trilinos with threads: Kokkos' OpenMP,
# ShyLU-node Basker (`.options linsol type=shylu_basker`, OMP_NUM_THREADS),
# METIS for Basker's nested dissection, and Amesos' UMFPACK
# (`type=umfpack`; AMESOS_UMFPACK_ORDERING=metis for METIS instead of AMD).
# nixpkgs as pads.py pins it; its Trilinos and Xyce, three changes. With
# -fopenmp, SuiteSparse_config.h includes omp.h, whose C++ templates break
# where Trilinos includes amd.h / umfpack.h inside extern "C": omp.h first.
#   nix build -f asic/spice/nix/xyce-threaded.nix
let
  nixpkgs = builtins.getFlake "github:NixOS/nixpkgs/494ce7fd23ff6a5dff39e1fb11e9b6f2ac74bf25";
  pkgs = nixpkgs.legacyPackages.x86_64-linux;
  inherit (pkgs) lib metis suitesparse;

  trilinos = pkgs.trilinos.overrideAttrs (old: {
    buildInputs = old.buildInputs ++ [ metis ];
    preConfigure = old.preConfigure + ''
      cmakeFlagsArray+=(
        -DTrilinos_ENABLE_OpenMP=ON
        -DKokkos_ENABLE_OPENMP=ON
        -DTrilinos_ENABLE_ShyLU_NodeBasker=ON
        -DAmesos2_ENABLE_ShyLU_NodeBasker=ON
        -DTPL_ENABLE_METIS=ON
        -DTPL_METIS_INCLUDE_DIRS=${lib.getDev metis}/include
        -DTPL_METIS_LIBRARY_DIRS=${lib.getLib metis}/lib
        -DTPL_ENABLE_UMFPACK=ON
        -DTPL_UMFPACK_INCLUDE_DIRS=${lib.getDev suitesparse}/include/suitesparse
        -DTPL_UMFPACK_LIBRARY_DIRS=${lib.getLib suitesparse}/lib
        -DAmesos_ENABLE_UMFPACK=ON
        -DAmesos2_ENABLE_UMFPACK=OFF
        -DEpetraExt_ENABLE_UMFPACK=OFF
        -DCMAKE_CXX_FLAGS="-O3 -fPIC -include omp.h"
      )
    '';
    # Amesos_Umfpack orders with UMFPACK's defaults (AMD); METIS on request
    postPatch = (old.postPatch or "") + ''
      substituteInPlace packages/amesos/src/Amesos_Umfpack.cpp \
        --replace-fail '#include "Amesos_Umfpack.h"' '#include "Amesos_Umfpack.h"
      #include <cstdlib>
      #include <cstring>' \
        --replace-fail 'double *Control = (double *) NULL, *Info = (double *) NULL;' 'double ControlV[UMFPACK_CONTROL], *Info = (double *) NULL;
        umfpack_di_defaults (ControlV);
        { const char *o = std::getenv ("AMESOS_UMFPACK_ORDERING");
          if (o && !std::strcmp (o, "metis")) ControlV[UMFPACK_ORDERING] = UMFPACK_ORDERING_METIS; }
        double *Control = ControlV;'
    '';
  });
in
(pkgs.xyce.override {
  inherit trilinos;
  enableDocs = false;
  enableTests = false;
}).overrideAttrs (old: {
  # Amesos knows UMFPACK as "Amesos_Umfpack"; Xyce passes types upper case
  postPatch = (old.postPatch or "") + ''
    substituteInPlace src/LinearAlgebraServicesPKG/N_LAS_AmesosSolver.C \
      --replace-fail 'else if( type_ == "MUMPS" )' 'else if( type_ == "UMFPACK" )
        {
          solverType = "Amesos_Umfpack";
        }
        else if( type_ == "MUMPS" )'
  '';
})
