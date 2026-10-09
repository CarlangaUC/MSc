// =============================================================================
// cmd/bench_qmem.h — latencia de membresía de snapshot en F_t (ZDD)
// =============================================================================
// Consulta nativa del ZDD (distinta de metatrie values_at):
//   dado S^τ (materializado desde .docs, fuera del timer de intersect),
//   mide Cudd_zddIntersect(ZDD^t, S) + countSubsets.
//
// Usa siempre el bosque **sin optimize** (build puro o .zpack de `build`).
//
// Uso:
//   ./zdd_cudd_plus_t bench-qmem u+t|log <docs> <queries.csv> [max_terms] [reps] [out.csv]
//   ./zdd_cudd_plus_t bench-qmem u+t|log <docs> <queries.csv> --pack <in.zpack> [reps] [out.csv]
//
// queries.csv: header term,rel. Se ignoran términos fuera del bosque (term >= V).
// =============================================================================

#ifndef ZDD_PLUS_T_CMD_BENCH_QMEM_H
#define ZDD_PLUS_T_CMD_BENCH_QMEM_H

#include "export/export.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct BenchQuery {
    uint32_t term = 0;
    uint32_t rel = 0;
};

static std::vector<uint64_t> expectedMastersAtRel(const std::vector<uint64_t>& postings,
                                                  uint32_t relative) {
    struct Pair {
        uint64_t master;
        uint64_t rel;
        bool operator<(const Pair& o) const {
            if (master != o.master) return master < o.master;
            return rel < o.rel;
        }
        bool operator==(const Pair& o) const { return master == o.master && rel == o.rel; }
    };
    std::vector<Pair> pairs;
    pairs.reserve(postings.size());
    for (uint64_t p : postings) pairs.push_back({ZDD_UNPACK_MASTER(p), ZDD_UNPACK_REL(p)});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

    std::vector<uint64_t> masters;
    size_t i = 0;
    const uint64_t rel = relative;
    while (i < pairs.size()) {
        const uint64_t master = pairs[i].master;
        uint64_t start = pairs[i].rel;
        uint64_t prev = start;
        ++i;
        while (i < pairs.size() && pairs[i].master == master) {
            if (pairs[i].rel == prev + 1) {
                prev = pairs[i].rel;
                ++i;
                continue;
            }
            if (start <= rel && rel <= prev) masters.push_back(master);
            start = prev = pairs[i].rel;
            ++i;
        }
        if (start <= rel && rel <= prev) masters.push_back(master);
    }
    std::sort(masters.begin(), masters.end());
    masters.erase(std::unique(masters.begin(), masters.end()), masters.end());
    return masters;
}

static bool loadQueriesCsv(const std::string& path, std::vector<BenchQuery>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line)) return false;  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string a, b;
        if (!std::getline(ss, a, ',')) continue;
        if (!std::getline(ss, b, ',')) continue;
        BenchQuery q;
        q.term = static_cast<uint32_t>(std::stoul(a));
        q.rel = static_cast<uint32_t>(std::stoul(b));
        out.push_back(q);
    }
    return !out.empty();
}

static int cmdBenchQmem(int argc, char** argv) {
    // argv: prog bench-qmem u+t|log <docs> <queries.csv>
    //         [max_terms] [reps] [out.csv]
    //         OR --pack <in.zpack> [reps] [out.csv]
    if (argc < 5) {
        std::cerr << "Usage:\n  " << argv[0]
                  << " bench-qmem u+t|log <docs> <queries.csv> [max_terms] [reps] [out.csv]\n  "
                  << argv[0]
                  << " bench-qmem u+t|log <docs> <queries.csv> --pack <in.zpack> [reps] [out.csv]\n"
                  << "  (bosque sin optimize: build in-process o .zpack de `build`)\n";
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[2], enc)) return 1;
    const std::string docsPath = argv[3];
    const std::string queriesPath = argv[4];

    std::string packPath;
    uint32_t maxTerms = 0;
    uint32_t reps = 5;
    std::string outCsv;
    int i = 5;
    if (i < argc && std::string(argv[i]) == "--pack") {
        if (i + 1 >= argc) {
            std::cerr << "ERROR: --pack requiere ruta .zpack\n";
            return 1;
        }
        packPath = argv[i + 1];
        i += 2;
        if (i < argc) reps = std::max(1u, static_cast<uint32_t>(std::stoul(argv[i++])));
        if (i < argc) outCsv = argv[i++];
    } else {
        if (i < argc) maxTerms = static_cast<uint32_t>(std::stoul(argv[i++]));
        if (i < argc) reps = std::max(1u, static_cast<uint32_t>(std::stoul(argv[i++])));
        if (i < argc) outCsv = argv[i++];
    }

    std::vector<BenchQuery> queriesRaw;
    if (!loadQueriesCsv(queriesPath, queriesRaw)) {
        std::cerr << "ERROR: cannot read queries " << queriesPath << std::endl;
        return 1;
    }

    BuildResult br;
    double buildOrLoadS = 0.0;
    auto t0load = std::chrono::steady_clock::now();
    if (!packPath.empty()) {
        std::cout << "[bench-qmem] loading .zpack (no optimize): " << packPath << "\n";
        DdManager* dd = nullptr;
        std::vector<DdNode*> pointerList;
        int numZddVars = 0, docOffset = 0;
        if (!ZddPack::loadZddPack(dd, pointerList, numZddVars, docOffset, packPath)) return 1;
        br.dd = dd;
        br.pointerList = std::move(pointerList);
        br.numZddVars = numZddVars;
        br.docOffset = docOffset;
        br.nTerms = static_cast<uint32_t>(br.pointerList.size());
        br.limit = br.nTerms;
        br.encoding = enc;
        br.poolNodes = Cudd_zddReadNodeCount(dd);
        buildOrLoadS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0load).count();
        std::cout << "[bench-qmem] loaded terms=" << br.limit << " docOffset=" << br.docOffset
                  << " pool_nodes=" << br.poolNodes << " load_s=" << buildOrLoadS << "\n";
    } else {
        std::cout << "[bench-qmem] buildForest in-process (no optimize) max_terms=" << maxTerms
                  << "\n";
        if (!buildForest(docsPath, maxTerms, enc, br, /*logCsv=*/"", /*logEvery=*/0)) return 1;
        buildOrLoadS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0load).count();
        trimSnapshotRefs(br);
        std::cout << "[bench-qmem] built terms=" << br.limit << " build_s=" << buildOrLoadS << "\n";
    }

    const uint32_t limit = static_cast<uint32_t>(br.pointerList.size());
    // Keep only queries whose term is inside the (baseline) forest.
    std::vector<BenchQuery> queries;
    queries.reserve(queriesRaw.size());
    uint64_t skippedOor = 0;
    for (const auto& q : queriesRaw) {
        if (q.term >= limit) {
            ++skippedOor;
            continue;
        }
        queries.push_back(q);
    }
    if (queries.empty()) {
        std::cerr << "ERROR: no queries with term < " << limit
                  << " (raw=" << queriesRaw.size() << " skipped_oor=" << skippedOor << ")\n";
        releaseForest(br);
        return 1;
    }
    std::cout << "bench_zdd_qmem_queries_raw=" << queriesRaw.size()
              << " kept=" << queries.size() << " skipped_term_oor=" << skippedOor << "\n";

    DocsIndex di = buildDocsIndex(docsPath);
    std::vector<std::vector<uint64_t>> postings(limit);
    for (uint32_t t = 0; t < limit; ++t) {
        if (t >= di.listOffsets.size()) continue;
        (void)readPostingListAt(docsPath, di.listOffsets[t], postings[t]);
    }

    std::vector<std::vector<uint64_t>> answers(queries.size());
    uint64_t emptyAnswers = 0;
    for (size_t i = 0; i < queries.size(); ++i) {
        answers[i] = expectedMastersAtRel(postings[queries[i].term], queries[i].rel);
        if (answers[i].empty()) ++emptyAnswers;
    }

    for (size_t i = 0; i < queries.size(); ++i) {
        const auto& q = queries[i];
        if (br.pointerList[q.term] == nullptr) continue;
        DdNode* snap = buildVersionSnapshotZdd(br.dd, answers[i], br.docOffset);
        if (snap == nullptr) continue;
        Cudd_Ref(snap);
        DdNode* inter = Cudd_zddIntersect(br.dd, br.pointerList[q.term], snap);
        if (inter != nullptr) {
            Cudd_Ref(inter);
            (void)ZddPack::countSubsets(br.dd, inter);
            Cudd_RecursiveDerefZdd(br.dd, inter);
        }
        Cudd_RecursiveDerefZdd(br.dd, snap);
    }

    uint64_t mismatches = 0;
    uint64_t totalHits = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t r = 0; r < reps; ++r) {
        for (size_t i = 0; i < queries.size(); ++i) {
            const auto& q = queries[i];
            if (br.pointerList[q.term] == nullptr || br.pointerList[q.term] == Cudd_ReadZero(br.dd)) {
                if (r == 0 && !answers[i].empty()) ++mismatches;
                continue;
            }
            DdNode* snap = buildVersionSnapshotZdd(br.dd, answers[i], br.docOffset);
            if (snap == nullptr) {
                if (r == 0) ++mismatches;
                continue;
            }
            Cudd_Ref(snap);
            DdNode* inter = Cudd_zddIntersect(br.dd, br.pointerList[q.term], snap);
            uint64_t hits = 0;
            if (inter != nullptr) {
                Cudd_Ref(inter);
                hits = ZddPack::countSubsets(br.dd, inter);
                Cudd_RecursiveDerefZdd(br.dd, inter);
            }
            Cudd_RecursiveDerefZdd(br.dd, snap);
            if (r == 0) {
                totalHits += hits;
                if (!answers[i].empty() && hits == 0) ++mismatches;
            }
        }
    }
    const double totalS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double nOps = static_cast<double>(queries.size()) * static_cast<double>(reps);
    const double nsPer = (nOps > 0.0) ? (totalS * 1e9 / nOps) : 0.0;

    std::cout << "bench_zdd_qmem_source=" << (packPath.empty() ? "buildForest" : "zpack_load") << "\n";
    std::cout << "bench_zdd_qmem_optimize=0\n";
    std::cout << "bench_zdd_qmem_build_or_load_s=" << buildOrLoadS << "\n";
    std::cout << "bench_zdd_qmem_n_queries=" << queries.size() << "\n";
    std::cout << "bench_zdd_qmem_reps=" << reps << "\n";
    std::cout << "bench_zdd_qmem_empty_answers=" << emptyAnswers << "\n";
    std::cout << "bench_zdd_qmem_mismatches=" << mismatches << "\n";
    std::cout << "bench_zdd_qmem_total_hits_pass0=" << totalHits << "\n";
    std::cout << "bench_zdd_qmem_s=" << totalS << "\n";
    std::cout << "bench_zdd_qmem_ns_per_query=" << nsPer << "\n";
    std::cout << "NOTE: timed region = buildVersionSnapshotZdd(S)+Intersect+countSubsets; "
                 "S^τ from .docs; forest is baseline (no optimize).\n";

    if (!outCsv.empty()) {
        std::ofstream csv(outCsv, std::ios::app);
        const bool needHeader = csv.tellp() == 0;
        if (needHeader) {
            csv << "structure,mode,docs,n_queries,reps,mismatches,avg_answer_size,total_s,ns_per_query\n";
        }
        double avgAns = 0.0;
        if (!queries.empty()) {
            uint64_t sum = 0;
            for (const auto& a : answers) sum += a.size();
            avgAns = static_cast<double>(sum) / static_cast<double>(queries.size());
        }
        csv << "zdd_qmem_baseline," << tagEncodingName(enc) << ',' << docsPath << ','
            << queries.size() << ',' << reps << ',' << mismatches << ',' << avgAns << ',' << totalS
            << ',' << nsPer << '\n';
        std::cout << "bench_csv_appended=" << outCsv << "\n";
    }

    releaseForest(br);
    // Don't abort the pipeline on soft issues: only hard mismatches among in-range queries.
    return mismatches == 0 ? 0 : 3;
}

#endif  // ZDD_PLUS_T_CMD_BENCH_QMEM_H
