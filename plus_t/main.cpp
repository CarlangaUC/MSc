// =============================================================================
// plus_t/main.cpp — bosque ZDD^t unificado (tag u+t o log/φ(t))
// =============================================================================
// Universo logico 64-bit (version_packing.h: master 40b, rel 24b):
//   masters m  in [0, U)       U = 2^ZDD_MASTER_BITS
//   marca logica {u+t} in [U+1, U+V] t = numero de termino 1-based, V = |Vocab|
//
// Codificacion del tag (argv posicional tras el modo):
//   u+t  — tag canonico {u+t}: var CUDD = t (1-based), docOffset = V + 1
//   log  — tag binario phi(t): tagWidth = ceil_log2(V+1) vars, docOffset = 1 + tagWidth
// =============================================================================
//   g++ -O2 -std=c++17 -fopenmp -o zdd_cudd_plus_t plus_t/main.cpp \
//       -I plus_t -I . -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
//       -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
//       -I ./TdZdd/include \
//       -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils
//
// Modos:
//   build  u+t|log <docs> <voc> <out.zpack|none> [max_terms] [log_csv] [log_every]
//   load   u+t|log <in.zpack> [voc] [spot_word]
//   verify u+t|log <docs> <voc> <tmp.zpack> [max_terms]
//   optimize u+t|log <in.zpack> <docs> <out.zpack|none|-> <heuristica> [max_sift_vars] [timeout_s]
//   optimize sweep u+t|log <in.zpack> <docs> [max_sift_vars] [timeout_s] [heur...]
//   demo   u+t|log [out_dir]
//   heuristics-check u+t|log [out_dir]
// =============================================================================

#include "cmd/bench_qmem.h"
#include "cmd/build.h"
#include "cmd/heuristics_check.h"
#include "cmd/load.h"
#include "cmd/optimize.h"
#include "cmd/verify.h"
#include "demo/viz.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    if (mode == "build") return cmdBuild(argc, argv);
    if (mode == "load") return cmdLoad(argc, argv);
    if (mode == "optimize") return cmdOptimize(argc, argv);
    if (mode == "verify") return cmdVerify(argc, argv);
    if (mode == "demo") return cmdDemo(argc, argv);
    if (mode == "heuristics-check") return cmdHeuristicsCheck(argc, argv);
    if (mode == "bench-qmem") return cmdBenchQmem(argc, argv);
    std::cerr << "Modo desconocido: " << mode << std::endl;
    usage(argv[0]);
    return 1;
}
