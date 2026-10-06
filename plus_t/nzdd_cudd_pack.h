// =============================================================================
// nzdd_cudd_pack.h — formato binario .zpack para bosque ZDD^t compartido (CUDD)
// =============================================================================
// v1 (orden identidad implícito):
//   MAGIC "ZPACKv1\0" | numZddVars u32 | docOffset u32 | nTerms u32 | nNodes u64
//   nNodes x (varIndex u32, elseId u64, thenId u64)
//   nTerms x rootId u64
//
// v2 (orden persistido — necesario para conservar el DAG compacto tras reorder):
//   MAGIC "ZPACKv2\0" | numZddVars u32 | docOffset u32 | nTerms u32 | nNodes u64
//   numZddVars x invPerm u32   // invPerm[level] = varIndex (= Cudd_ReadInvPermZdd)
//   nodos + roots (igual que v1)
//
// Ids: 0=Cudd_ReadZero, 1=Cudd_ReadOne, internos desde 2.
// =============================================================================

#ifndef NZDD_CUDD_PACK_H
#define NZDD_CUDD_PACK_H

#include <cstddef>
#include <cstdio>

#include "cudd.h"
#include "nzdd_cudd_common.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZddPack {

static constexpr char kMagicV1[8] = {'Z', 'P', 'A', 'C', 'K', 'v', '1', '\0'};
static constexpr char kMagicV2[8] = {'Z', 'P', 'A', 'C', 'K', 'v', '2', '\0'};

enum class PackFormat { V1, V2 };

struct PackNodeRec {
    uint32_t varIndex = 0;
    uint64_t elseId = 0;
    uint64_t thenId = 0;
};

struct ZddPackData {
    int numZddVars = 0;
    int docOffset = 0;
    PackFormat format = PackFormat::V1;
    // invPerm[level] = varIndex. Vacío en v1 (= identidad).
    std::vector<uint32_t> invPerm;
    std::vector<PackNodeRec> nodes;
    std::vector<uint64_t> rootIds;
};

inline std::vector<uint32_t> captureInvPerm(DdManager* dd) {
    const int nv = Cudd_ReadZddSize(dd);
    std::vector<uint32_t> p(static_cast<size_t>(nv));
    for (int lvl = 0; lvl < nv; ++lvl) {
        p[static_cast<size_t>(lvl)] = static_cast<uint32_t>(Cudd_ReadInvPermZdd(dd, lvl));
    }
    return p;
}

// Niveles que el DAG ocupa de verdad = varIndex distintos entre sus nodos. El
// .zpack ya guarda varIndex por nodo, asi que sale exacto sin recorrer el DAG ni
// consultar las subtablas del manager (donde la cadena univ ocupa TODOS los
// niveles con 1 nodo cada uno y haria que el conteo fuera siempre numZddVars).
// Invariante bajo reordenamiento: permutar niveles no cambia el conjunto de
// variables usadas, solo su profundidad.
inline uint32_t countNonEmptyLevels(const ZddPackData& pd) {
    if (pd.nodes.empty()) return 0u;
    uint32_t maxIdx = 0;
    for (const PackNodeRec& rec : pd.nodes)
        if (rec.varIndex > maxIdx) maxIdx = rec.varIndex;
    std::vector<bool> seen(static_cast<size_t>(maxIdx) + 1u, false);
    uint32_t distinct = 0;
    for (const PackNodeRec& rec : pd.nodes) {
        if (!seen[rec.varIndex]) {
            seen[rec.varIndex] = true;
            ++distinct;
        }
    }
    return distinct;
}

inline bool isIdentityInvPerm(const std::vector<uint32_t>& invPerm) {
    for (size_t i = 0; i < invPerm.size(); ++i) {
        if (invPerm[i] != static_cast<uint32_t>(i)) return false;
    }
    return true;
}

inline bool managerUsesNonIdentityOrder(DdManager* dd) {
    if (dd == nullptr) return false;
    const int nv = Cudd_ReadZddSize(dd);
    for (int lvl = 0; lvl < nv; ++lvl) {
        if (Cudd_ReadInvPermZdd(dd, lvl) != lvl) return true;
    }
    return false;
}

inline bool applyInvPerm(DdManager* dd, const std::vector<uint32_t>& invPerm) {
    if (invPerm.empty() || isIdentityInvPerm(invPerm)) return true;
    std::vector<int> perm(invPerm.begin(), invPerm.end());
    return Cudd_zddShuffleHeap(dd, perm.data()) != 0;
}

inline bool writeAll(std::ofstream& out, const void* data, size_t n) {
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    return out.good();
}

inline bool readAll(std::ifstream& in, void* data, size_t n) {
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(n));
    return in.good();
}

inline uint64_t nodeToId(DdManager* dd, DdNode* n) {
    if (n == nullptr || n == Cudd_ReadZero(dd)) return 0;
    if (n == Cudd_ReadOne(dd)) return 1;
    return 0;  // interno: asignado por el mapa
}

inline uint64_t assignNodeId(DdManager* dd, DdNode* n,
                             std::unordered_map<DdNode*, uint64_t>& idMap,
                             std::vector<PackNodeRec>& table) {
    auto it = idMap.find(n);
    if (it != idMap.end()) return it->second;

    if (Cudd_IsConstant(n)) {
        const uint64_t id = (n == Cudd_ReadOne(dd)) ? 1u : 0u;
        idMap.emplace(n, id);
        return id;
    }

    const uint64_t elseId = assignNodeId(dd, Cudd_E(n), idMap, table);
    const uint64_t thenId = assignNodeId(dd, Cudd_T(n), idMap, table);
    const uint64_t id = static_cast<uint64_t>(table.size()) + 2u;
    idMap.emplace(n, id);
    table.push_back(
        {static_cast<uint32_t>(Cudd_NodeReadIndex(n)), elseId, thenId});
    return id;
}

inline void countSubsetsRec(DdManager* dd, DdNode* node, uint64_t& count) {
    if (Cudd_IsConstant(node)) {
        if (node == Cudd_ReadOne(dd)) ++count;
        return;
    }
    countSubsetsRec(dd, Cudd_E(node), count);
    countSubsetsRec(dd, Cudd_T(node), count);
}

inline uint64_t countSubsets(DdManager* dd, DdNode* root) {
    if (root == nullptr || root == Cudd_ReadZero(dd)) return 0;
    uint64_t c = 0;
    countSubsetsRec(dd, root, c);
    return c;
}

inline void collectMastersRec(DdManager* dd, DdNode* node, int docOffset,
                              std::vector<int>& path, std::set<uint64_t>& acc) {
    if (Cudd_IsConstant(node)) {
        if (node == Cudd_ReadOne(dd)) {
            for (int v : path) {
                if (v >= docOffset) acc.insert(static_cast<uint64_t>(v - docOffset));
            }
        }
        return;
    }
    const int idx = Cudd_NodeReadIndex(node);
    collectMastersRec(dd, Cudd_E(node), docOffset, path, acc);
    path.push_back(idx);
    collectMastersRec(dd, Cudd_T(node), docOffset, path, acc);
    path.pop_back();
}

inline std::vector<uint64_t> mastersOfZdd(DdManager* dd, DdNode* root, int docOffset) {
    std::set<uint64_t> acc;
    std::vector<int> path;
    if (root != nullptr && root != Cudd_ReadZero(dd))
        collectMastersRec(dd, root, docOffset, path, acc);
    return std::vector<uint64_t>(acc.begin(), acc.end());
}

inline ZddPackData extractPackData(DdManager* dd, const std::vector<DdNode*>& termZdd,
                                   int numZddVars, int docOffset) {
    ZddPackData pd;
    pd.numZddVars = numZddVars;
    pd.docOffset = docOffset;

    std::unordered_map<DdNode*, uint64_t> idMap;
    idMap.reserve(termZdd.size() * 4 + 64);
    pd.nodes.reserve(static_cast<size_t>(Cudd_zddReadNodeCount(dd)));

    pd.rootIds.reserve(termZdd.size());
    for (DdNode* root : termZdd) {
        if (root == nullptr) {
            pd.rootIds.push_back(0);
        } else {
            pd.rootIds.push_back(assignNodeId(dd, root, idMap, pd.nodes));
        }
    }
    return pd;
}

inline bool writeZddPackFile(const ZddPackData& pd, const std::string& path) {
    const uint32_t nTerms = static_cast<uint32_t>(pd.rootIds.size());
    const uint64_t nNodes = static_cast<uint64_t>(pd.nodes.size());
    const uint32_t numZddVarsU = static_cast<uint32_t>(pd.numZddVars);
    const uint32_t docOffsetU = static_cast<uint32_t>(pd.docOffset);
    const bool asV2 = (pd.format == PackFormat::V2 && !pd.invPerm.empty());

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[ZddPack] no se pudo abrir para escribir: " << path << std::endl;
        return false;
    }

    const char* magic = asV2 ? kMagicV2 : kMagicV1;
    if (!writeAll(out, magic, 8) || !writeAll(out, &numZddVarsU, sizeof(numZddVarsU)) ||
        !writeAll(out, &docOffsetU, sizeof(docOffsetU)) || !writeAll(out, &nTerms, sizeof(nTerms)) ||
        !writeAll(out, &nNodes, sizeof(nNodes))) {
        std::cerr << "[ZddPack] error escribiendo header" << std::endl;
        return false;
    }

    if (asV2) {
        for (uint32_t lvl = 0; lvl < numZddVarsU; ++lvl) {
            const uint32_t varIdx = pd.invPerm[static_cast<size_t>(lvl)];
            if (!writeAll(out, &varIdx, sizeof(varIdx))) {
                std::cerr << "[ZddPack] error escribiendo invPerm" << std::endl;
                return false;
            }
        }
    }

    for (const PackNodeRec& rec : pd.nodes) {
        if (!writeAll(out, &rec.varIndex, sizeof(rec.varIndex)) ||
            !writeAll(out, &rec.elseId, sizeof(rec.elseId)) ||
            !writeAll(out, &rec.thenId, sizeof(rec.thenId))) {
            std::cerr << "[ZddPack] error escribiendo tabla de nodos" << std::endl;
            return false;
        }
    }

    for (uint64_t rid : pd.rootIds) {
        if (!writeAll(out, &rid, sizeof(rid))) {
            std::cerr << "[ZddPack] error escribiendo roots" << std::endl;
            return false;
        }
    }

    out.close();
    std::cout << "[ZddPack] guardado: " << path << " format=" << (asV2 ? "v2" : "v1")
              << " nTerms=" << nTerms << " nNodes=" << nNodes << " numZddVars=" << pd.numZddVars
              << std::endl;
    return true;
}

inline bool saveZddPack(DdManager* dd, const std::vector<DdNode*>& termZdd, int numZddVars,
                        int docOffset, const std::string& path) {
    if (dd == nullptr) return false;
    ZddPackData pd = extractPackData(dd, termZdd, numZddVars, docOffset);
    pd.invPerm = captureInvPerm(dd);
    if (isIdentityInvPerm(pd.invPerm)) {
        pd.invPerm.clear();
        pd.format = PackFormat::V1;
    } else {
        pd.format = PackFormat::V2;
    }
    return writeZddPackFile(pd, path);
}

inline DdNode* rebuildZddNode(DdManager* dd, int varIndex, DdNode* elseN, DdNode* thenN) {
    DdNode* changed = Cudd_zddChange(dd, thenN, varIndex);
    if (changed == nullptr) return nullptr;
    Cudd_Ref(changed);
    DdNode* node = Cudd_zddUnion(dd, elseN, changed);
    if (node == nullptr) {
        Cudd_RecursiveDerefZdd(dd, changed);
        return nullptr;
    }
    Cudd_Ref(node);
    Cudd_RecursiveDerefZdd(dd, changed);
    return node;
}

inline bool rebuildInto(DdManager* dd, const ZddPackData& pd, std::vector<DdNode*>& rootsOut) {
    if (dd == nullptr) return false;

    const uint64_t nNodes = static_cast<uint64_t>(pd.nodes.size());
    const uint32_t nRoots = static_cast<uint32_t>(pd.rootIds.size());

    DdNode* zero = Cudd_ReadZero(dd);
    DdNode* one = Cudd_ReadOne(dd);
    Cudd_Ref(zero);
    Cudd_Ref(one);

    const size_t nodeCap = static_cast<size_t>(nNodes) + 2u;
    std::vector<DdNode*> nodes(nodeCap, nullptr);
    nodes[0] = zero;
    nodes[1] = one;

    for (uint64_t i = 0; i < nNodes; ++i) {
        const PackNodeRec& rec = pd.nodes[static_cast<size_t>(i)];
        if (rec.elseId >= nodes.size() || rec.thenId >= nodes.size() ||
            nodes[rec.elseId] == nullptr || nodes[rec.thenId] == nullptr) {
            std::cerr << "[ZddPack] referencia invalida en nodo " << (i + 2) << std::endl;
            return false;
        }
        DdNode* built =
            rebuildZddNode(dd, static_cast<int>(rec.varIndex), nodes[rec.elseId], nodes[rec.thenId]);
        if (built == nullptr) {
            std::cerr << "[ZddPack] rebuild fallo nodo " << (i + 2) << std::endl;
            return false;
        }
        nodes[static_cast<size_t>(i) + 2u] = built;
    }

    rootsOut.assign(nRoots, nullptr);
    for (uint32_t t = 0; t < nRoots; ++t) {
        const uint64_t rid = pd.rootIds[t];
        if (rid >= nodes.size() || nodes[rid] == nullptr) {
            std::cerr << "[ZddPack] rootId invalido term " << t << " id=" << rid << std::endl;
            for (DdNode* nptr : nodes) {
                if (nptr != nullptr) Cudd_RecursiveDerefZdd(dd, nptr);
            }
            for (DdNode* r : rootsOut) {
                if (r != nullptr) Cudd_RecursiveDerefZdd(dd, r);
            }
            rootsOut.clear();
            return false;
        }
        rootsOut[t] = nodes[rid];
        Cudd_Ref(rootsOut[t]);
    }
    // Solo las raices deben quedar referenciadas: la referencia de construccion de
    // cada nodo se libera aqui, si no freeTermZdd no puede liberar nada nunca.
    for (DdNode* nptr : nodes) {
        if (nptr != nullptr) Cudd_RecursiveDerefZdd(dd, nptr);
    }
    return true;
}

inline bool readZddPackFile(const std::string& path, ZddPackData& pd) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "[ZddPack] no se pudo abrir: " << path << std::endl;
        return false;
    }

    char magic[8];
    uint32_t numZddVarsU = 0, docOffsetU = 0, nTerms = 0;
    uint64_t nNodes = 0;

    if (!readAll(in, magic, 8)) {
        std::cerr << "[ZddPack] magic invalido" << std::endl;
        return false;
    }
    const bool isV2 = (std::memcmp(magic, kMagicV2, 8) == 0);
    const bool isV1 = (std::memcmp(magic, kMagicV1, 8) == 0);
    if (!isV1 && !isV2) {
        std::cerr << "[ZddPack] magic invalido" << std::endl;
        return false;
    }

    if (!readAll(in, &numZddVarsU, sizeof(numZddVarsU)) ||
        !readAll(in, &docOffsetU, sizeof(docOffsetU)) || !readAll(in, &nTerms, sizeof(nTerms)) ||
        !readAll(in, &nNodes, sizeof(nNodes))) {
        std::cerr << "[ZddPack] error leyendo header" << std::endl;
        return false;
    }

    pd.numZddVars = static_cast<int>(numZddVarsU);
    pd.docOffset = static_cast<int>(docOffsetU);
    pd.format = isV2 ? PackFormat::V2 : PackFormat::V1;
    pd.invPerm.clear();

    if (isV2) {
        pd.invPerm.assign(static_cast<size_t>(numZddVarsU), 0);
        for (uint32_t lvl = 0; lvl < numZddVarsU; ++lvl) {
            if (!readAll(in, &pd.invPerm[static_cast<size_t>(lvl)], sizeof(uint32_t))) {
                std::cerr << "[ZddPack] error leyendo invPerm" << std::endl;
                return false;
            }
        }
    }
    pd.nodes.assign(static_cast<size_t>(nNodes), PackNodeRec{});
    for (uint64_t i = 0; i < nNodes; ++i) {
        if (!readAll(in, &pd.nodes[static_cast<size_t>(i)].varIndex, sizeof(uint32_t)) ||
            !readAll(in, &pd.nodes[static_cast<size_t>(i)].elseId, sizeof(uint64_t)) ||
            !readAll(in, &pd.nodes[static_cast<size_t>(i)].thenId, sizeof(uint64_t))) {
            std::cerr << "[ZddPack] error leyendo nodo " << i << std::endl;
            return false;
        }
    }

    pd.rootIds.assign(nTerms, 0);
    for (uint32_t t = 0; t < nTerms; ++t) {
        if (!readAll(in, &pd.rootIds[t], sizeof(uint64_t))) {
            std::cerr << "[ZddPack] error leyendo root " << t << std::endl;
            return false;
        }
    }
    return true;
}

inline bool assembleForest(DdManager*& dd, std::vector<DdNode*>& termZdd, int numZddVars,
                           int docOffset, const std::vector<ZddPackData>& shards,
                           const std::vector<std::pair<uint32_t, uint32_t>>& shardRanges) {
    if (shards.size() != shardRanges.size()) {
        std::cerr << "[ZddPack] assembleForest: shards/ranges size mismatch" << std::endl;
        return false;
    }

    if (dd != nullptr) {
        Cudd_Quit(dd);
        dd = nullptr;
    }

    dd = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) {
        std::cerr << "[ZddPack] Cudd_Init fallo en assembleForest" << std::endl;
        return false;
    }
    NzddCommon::configureCuddManager(dd);

    // Si algún shard trae permutación v2, aplicarla una vez al manager vacío.
    for (const ZddPackData& shard : shards) {
        if (shard.format == PackFormat::V2 && !shard.invPerm.empty()) {
            if (!applyInvPerm(dd, shard.invPerm)) {
                std::cerr << "[ZddPack] assembleForest: applyInvPerm fallo" << std::endl;
                Cudd_Quit(dd);
                dd = nullptr;
                termZdd.clear();
                return false;
            }
            break;
        }
    }

    uint32_t totalTerms = 0;
    for (const auto& range : shardRanges) {
        const uint32_t end = range.first + range.second;
        if (end > totalTerms) totalTerms = end;
    }
    termZdd.assign(totalTerms, nullptr);

    for (size_t s = 0; s < shards.size(); ++s) {
        const auto& range = shardRanges[s];
        if (range.second == 0) continue;

        if (shards[s].numZddVars != numZddVars || shards[s].docOffset != docOffset) {
            std::cerr << "[ZddPack] shard " << s << " metadata mismatch" << std::endl;
            Cudd_Quit(dd);
            dd = nullptr;
            termZdd.clear();
            return false;
        }

        std::vector<DdNode*> roots;
        if (!rebuildInto(dd, shards[s], roots)) {
            Cudd_Quit(dd);
            dd = nullptr;
            termZdd.clear();
            return false;
        }
        if (roots.size() != range.second) {
            std::cerr << "[ZddPack] shard " << s << " root count mismatch" << std::endl;
            for (DdNode* r : roots) {
                if (r != nullptr && r != Cudd_ReadZero(dd)) Cudd_RecursiveDerefZdd(dd, r);
            }
            Cudd_Quit(dd);
            dd = nullptr;
            termZdd.clear();
            return false;
        }

        for (uint32_t j = 0; j < range.second; ++j) {
            termZdd[range.first + j] = roots[j];
        }
    }

    std::cout << "[ZddPack] assembleForest: nTerms=" << totalTerms
              << " pool_nodes=" << Cudd_zddReadNodeCount(dd) << std::endl;
    return true;
}

inline bool loadZddPack(DdManager*& dd, std::vector<DdNode*>& termZdd, int& numZddVars,
                        int& docOffset, const std::string& path) {
    ZddPackData pd;
    if (!readZddPackFile(path, pd)) return false;

    numZddVars = pd.numZddVars;
    docOffset = pd.docOffset;

    if (dd != nullptr) {
        Cudd_Quit(dd);
        dd = nullptr;
    }

    dd = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) {
        std::cerr << "[ZddPack] Cudd_Init fallo" << std::endl;
        return false;
    }
    NzddCommon::configureCuddManager(dd);

    if (!applyInvPerm(dd, pd.invPerm)) {
        std::cerr << "[ZddPack] applyInvPerm fallo" << std::endl;
        Cudd_Quit(dd);
        dd = nullptr;
        termZdd.clear();
        return false;
    }

    if (!rebuildInto(dd, pd, termZdd)) {
        Cudd_Quit(dd);
        dd = nullptr;
        termZdd.clear();
        return false;
    }

    std::cout << "[ZddPack] cargado: " << path << " format=" << (pd.format == PackFormat::V2 ? "v2" : "v1")
              << " nTerms=" << termZdd.size() << " nNodes=" << pd.nodes.size()
              << " pool_nodes=" << Cudd_zddReadNodeCount(dd)
              << " edd_nodes=" << NzddCommon::cuddForestNodeCount(dd, termZdd)
              << " levels=" << countNonEmptyLevels(pd) << std::endl;
    return true;
}

inline void freeTermZdd(DdManager* dd, std::vector<DdNode*>& termZdd) {
    if (dd == nullptr) return;
    for (DdNode* z : termZdd) {
        if (z != nullptr && z != Cudd_ReadZero(dd)) Cudd_RecursiveDerefZdd(dd, z);
    }
    termZdd.clear();
}

}  // namespace ZddPack

#endif  // NZDD_CUDD_PACK_H
