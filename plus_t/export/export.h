// =============================================================================
// export/export.h — exportacion/serializacion y diagnostico (zdd_cudd_plus_t)
// =============================================================================

#ifndef ZDD_PLUS_T_EXPORT_H
#define ZDD_PLUS_T_EXPORT_H

#include "engine/engine.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

static void spotCheck(DdManager* dd, const std::vector<DdNode*>& pointerList, const Vocabulary& voc,
                      int spotId, int docOffset, TagEncoding enc) {
    if (spotId < 0) {
        std::cout << "Spot: no resuelto\n";
        return;
    }
    std::cout << "term_id=" << spotId;
    if (voc.loaded && static_cast<uint32_t>(spotId) < voc.nwords)
        std::cout << " palabra='" << voc.words[static_cast<uint32_t>(spotId)] << "'";
    std::cout << std::endl;
    DdNode* z = pointerList[static_cast<uint32_t>(spotId)];
    if (z == nullptr || z == Cudd_ReadZero(dd)) {
        std::cout << "ZDD^t: vacio\n";
        return;
    }
    const int tagTermId = readTermIdFromTagSearch(dd, z, docOffset, enc);
    std::cout << "DagSize=" << Cudd_zddDagSize(z) << " |ZDD^t|=" << ZddPack::countSubsets(dd, z);
    if (tagTermId >= 0) {
        const uint64_t termOneBased = static_cast<uint64_t>(tagTermId) + 1u;
        if (enc == TagEncoding::UPlusT) {
            std::cout << " tag_var=" << uPlusIVar(static_cast<uint32_t>(termOneBased))
                      << " singleton={u+" << termOneBased << "} logical="
                      << logicalTagElement(termOneBased);
        } else {
            std::cout << " tagWidth=" << tagWidthFromDocOffset(docOffset)
                      << " tag_decoded={u+" << termOneBased << "} logical="
                      << logicalTagElement(termOneBased);
        }
    }
    std::cout << std::endl;
    printSet("|M_t|", ZddPack::mastersOfZdd(dd, z, docOffset));
}

static bool metricsEqual(const TermMetrics& a, const TermMetrics& b) {
    return a.dagSize == b.dagSize && a.nSubsets == b.nSubsets &&
           a.termTagLogical == b.termTagLogical && a.masters == b.masters;
}

static uint64_t countSharedSnapshots(const TermFtInputs& a, const TermFtInputs& b) {
    uint64_t shared = 0;
    for (const auto& sa : a.versionSnapshots) {
        for (const auto& sb : b.versionSnapshots) {
            if (sa == sb) ++shared;
        }
    }
    return shared;
}

struct ForestQueryResult {
    int termA = -1;
    int termB = -1;
    uint64_t q1_hits = 0;
    uint64_t q1_expected = 1;
    uint64_t q2_shared = 0;
    uint64_t q2_expected = 0;
    bool q1_ran = false;
    bool q2_ran = false;
};

static void resolveQueryTerms(const Vocabulary& voc, uint32_t limit, int& termA, int& termB) {
    termA = resolveSpot(voc, "Abraham", limit);
    if (termA < 0 && limit > 0) termA = 0;
    termB = resolveSpot(voc, "beta", limit);
    if (termB < 0) termB = resolveSpot(voc, "0", limit);
    if (termB < 0 || termB == termA) {
        if (limit >= 2) termB = (termA == 0) ? 1 : 0;
        else termB = -1;
    }
}

// Consultas CUDD: Q1 snapshot membership, Q2 interseccion entre terminos.
static ForestQueryResult runForestQueryChecks(DdManager* dd, const std::vector<DdNode*>& roots,
                                              int docOffset,
                                              const std::vector<TermFtInputs>& termData,
                                              const Vocabulary& voc, uint32_t limit,
                                              const char* phaseLabel) {
    ForestQueryResult r;
    if (dd == nullptr || roots.empty() || limit == 0) return r;

    resolveQueryTerms(voc, limit, r.termA, r.termB);

    std::cout << "\n=== Query " << phaseLabel << ": snapshot membership ===\n";
    if (r.termA >= 0 && static_cast<uint32_t>(r.termA) < limit &&
        roots[static_cast<uint32_t>(r.termA)] != nullptr &&
        roots[static_cast<uint32_t>(r.termA)] != Cudd_ReadZero(dd)) {
        const auto& snaps = termData[static_cast<uint32_t>(r.termA)].versionSnapshots;
        if (!snaps.empty()) {
            r.q1_ran = true;
            const auto& snap = snaps.front();
            DdNode* snapZdd = buildVersionSnapshotZdd(dd, snap, docOffset);
            if (snapZdd != nullptr) {
                Cudd_Ref(snapZdd);
                DdNode* q = Cudd_zddIntersect(dd, roots[static_cast<uint32_t>(r.termA)], snapZdd);
                if (q != nullptr) {
                    Cudd_Ref(q);
                    r.q1_hits = ZddPack::countSubsets(dd, q);
                    Cudd_RecursiveDerefZdd(dd, q);
                }
                Cudd_RecursiveDerefZdd(dd, snapZdd);
            }
            std::cout << "term_id=" << r.termA;
            if (voc.loaded && static_cast<uint32_t>(r.termA) < voc.nwords)
                std::cout << " palabra='" << voc.words[static_cast<uint32_t>(r.termA)] << "'";
            std::cout << " |snapshot|=" << snap.size() << " hits=" << r.q1_hits
                      << " gt=" << r.q1_expected << std::endl;
            printSet("  masters", snap, 16);
        }
    } else {
        std::cout << "(omitida: termA vacio o fuera de rango)\n";
    }

    std::cout << "\n=== Query " << phaseLabel << ": intersect(terms) ===\n";
    if (r.termA >= 0 && r.termB >= 0 && r.termA != r.termB &&
        static_cast<uint32_t>(r.termA) < limit && static_cast<uint32_t>(r.termB) < limit &&
        roots[static_cast<uint32_t>(r.termA)] != nullptr &&
        roots[static_cast<uint32_t>(r.termB)] != nullptr &&
        roots[static_cast<uint32_t>(r.termA)] != Cudd_ReadZero(dd) &&
        roots[static_cast<uint32_t>(r.termB)] != Cudd_ReadZero(dd)) {
        r.q2_ran = true;
        r.q2_expected = countSharedSnapshots(termData[static_cast<uint32_t>(r.termA)],
                                             termData[static_cast<uint32_t>(r.termB)]);
        DdNode* inter = Cudd_zddIntersect(dd, roots[static_cast<uint32_t>(r.termA)],
                                          roots[static_cast<uint32_t>(r.termB)]);
        if (inter != nullptr) {
            Cudd_Ref(inter);
            r.q2_shared = ZddPack::countSubsets(dd, inter);
            Cudd_RecursiveDerefZdd(dd, inter);
        }
        std::cout << "termA=" << r.termA << " termB=" << r.termB
                  << " |ZDD^A ∩ ZDD^B|=" << r.q2_shared
                  << " gt_shared_snapshots=" << r.q2_expected << std::endl;
    } else {
        std::cout << "(omitida: terminos invalidos o vacios)\n";
    }

    return r;
}

static uint64_t countQueryGroundTruthMismatches(const ForestQueryResult& r, const char* phaseLabel) {
    uint64_t mismatches = 0;
    if (r.q1_ran && r.q1_hits != r.q1_expected) {
        ++mismatches;
        std::cerr << "[QUERY_GT_MISMATCH][" << phaseLabel << "] snapshot membership term=" << r.termA
                  << " exp=" << r.q1_expected << " got=" << r.q1_hits << std::endl;
    }
    if (r.q2_ran && r.q2_shared != r.q2_expected) {
        ++mismatches;
        std::cerr << "[QUERY_GT_MISMATCH][" << phaseLabel << "] intersect terms " << r.termA << " & "
                  << r.termB << " exp=" << r.q2_expected << " got=" << r.q2_shared << std::endl;
    }
    return mismatches;
}

static uint64_t countQueryParityMismatches(const ForestQueryResult& build,
                                           const ForestQueryResult& loaded) {
    uint64_t mismatches = 0;
    std::cout << "\n=== Query parity: build vs loaded ===\n";
    if (build.q1_ran != loaded.q1_ran) {
        ++mismatches;
        std::cerr << "[QUERY_PARITY] q1_ran build=" << build.q1_ran << " loaded=" << loaded.q1_ran
                  << std::endl;
    } else if (build.q1_ran && build.q1_hits != loaded.q1_hits) {
        ++mismatches;
        std::cerr << "[QUERY_PARITY] q1_hits build=" << build.q1_hits << " loaded=" << loaded.q1_hits
                  << std::endl;
    } else if (build.q1_ran) {
        std::cout << "Q1 snapshot hits: build=" << build.q1_hits << " loaded=" << loaded.q1_hits
                  << " OK\n";
    }

    if (build.q2_ran != loaded.q2_ran) {
        ++mismatches;
        std::cerr << "[QUERY_PARITY] q2_ran build=" << build.q2_ran << " loaded=" << loaded.q2_ran
                  << std::endl;
    } else if (build.q2_ran && build.q2_shared != loaded.q2_shared) {
        ++mismatches;
        std::cerr << "[QUERY_PARITY] q2_shared build=" << build.q2_shared
                  << " loaded=" << loaded.q2_shared << std::endl;
    } else if (build.q2_ran) {
        std::cout << "Q2 intersect: build=" << build.q2_shared << " loaded=" << loaded.q2_shared
                  << " OK\n";
    }

    if (mismatches == 0 && (build.q1_ran || build.q2_ran)) {
        std::cout << "semantica empirica build == loaded (misma respuesta a consultas CUDD)\n";
    }
    return mismatches;
}

static void usage(const char* prog) {
    std::cerr
        << "Uso:\n"
        << "  " << prog
        << " build  u+t|log <docs> <voc> <out.zpack|none> [max_terms] [log_csv] [log_every]\n"
        << "  " << prog << " load   u+t|log <in.zpack> [voc] [spot_word]\n"
        << "  " << prog
        << " optimize u+t|log <in.zpack> <docs> <out.zpack|none|-> <heuristica> [max_sift_vars] [timeout_s]\n"
        << "  " << prog
        << " optimize sweep u+t|log <in.zpack> <docs> [max_sift_vars] [timeout_s] [heur...]\n"
        << "  " << prog << " verify u+t|log <docs> <voc> <tmp.zpack> [max_terms]  (consultas build+loaded+parity)\n"
        << "  " << prog << " demo   u+t|log [out_dir]\n"
        << "  " << prog << " heuristics-check u+t|log [out_dir]\n"
        << "\n"
        << "  Codificacion del tag (F_t + {codificacion}):\n"
        << "    u+t  — tag canonico {u+t}: 1 var CUDD por termino (u+i)\n"
        << "    log  — tag binario phi(t): ceil_log2(V+1) vars, bits de t (log i)\n"
        << "  ZDD^t = F_t U tag; masters en var (docOffset + m).\n"
        << "  demo: toy u=4, V=2; imprime familias + .dot/.png en out_dir.\n"
        << "  heuristics-check: toy + 14 heurísticas; semántica + mosaico DOT/PNG.\n"
        << "  out.zpack=none|-  construye sin serializar\n"
        << "  build escribe siempre cudd_evolucion_<docs>_<enc>.csv en resultados_test/\n"
        << "  optimize escribe optimize_runs.csv + CSV por corrida en resultados_test/\n"
        << "  log_csv/log_every en build (default auto si se omite log_csv)\n";
}

static int saveBuildResult(BuildResult& br, const std::string& packPath) {
    if (!shouldSavePack(packPath)) {
        std::cout << "[BUILD] serializacion omitida (out=" << packPath << ")\n";
        return 0;
    }
    auto tSave0 = std::chrono::steady_clock::now();
    if (!ZddPack::saveZddPack(br.dd, br.pointerList, br.numZddVars, br.docOffset, packPath)) {
        return 1;
    }
    auto tSave1 = std::chrono::steady_clock::now();
    const double saveS = std::chrono::duration<double>(tSave1 - tSave0).count();
    std::ifstream chk(packPath, std::ios::binary | std::ios::ate);
    const auto packBytes = chk.tellg();
    std::cout << "pack: " << packPath << " (" << packBytes << " bytes) save=" << saveS << "s\n";
    return 0;
}

#endif  // ZDD_PLUS_T_EXPORT_H
