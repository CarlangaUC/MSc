// =============================================================================
// demo/viz.h — modo `demo`: dataset toy u=4,V=2 + export Graphviz
// =============================================================================
// Solo lo usa el comando `demo`; build/load/verify no generan .dot/.png.
// Etiquetas del grafo dependen de la codificacion (u+t vs log/φ(t)).
// =============================================================================

#ifndef ZDD_DEMO_VIZ_H
#define ZDD_DEMO_VIZ_H

#include "engine/engine.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdlib>
#include <map>
#include <unordered_set>

static std::string logicalElementLabel(int var, int docOffset, uint64_t demoU, TagEncoding enc) {
    if (var >= docOffset) return std::to_string(var - docOffset);
    if (var >= static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET)) {
        if (enc == TagEncoding::UPlusT) return std::to_string(demoU + static_cast<uint64_t>(var));
        const int bit = var - static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET);
        return std::string("τ") + std::to_string(bit) + " (bit " + std::to_string(bit) + ")";
    }
    return std::to_string(var);
}

static std::string demoDotVarLabel(int var, int docOffset, uint64_t demoU, TagEncoding enc) {
    if (var >= docOffset) return std::to_string(var - docOffset);
    if (var >= static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET)) {
        if (enc == TagEncoding::UPlusT) return std::to_string(demoU + static_cast<uint64_t>(var));
        const int bit = var - static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET);
        return std::string("τ") + std::to_string(bit) + "\\nbit " + std::to_string(bit) +
               "\\n2^" + std::to_string(bit);
    }
    return std::to_string(var);
}

static void enumZddSetsRec(DdManager* dd, DdNode* node, int docOffset, uint64_t demoU,
                           TagEncoding enc, std::vector<int>& path,
                           std::vector<std::vector<std::string>>& out) {
    if (Cudd_IsConstant(node)) {
        if (node == Cudd_ReadOne(dd)) {
            std::vector<std::string> oneSet;
            oneSet.reserve(path.size());
            for (int v : path) oneSet.push_back(logicalElementLabel(v, docOffset, demoU, enc));
            out.push_back(std::move(oneSet));
        }
        return;
    }
    const int idx = Cudd_NodeReadIndex(node);
    enumZddSetsRec(dd, Cudd_E(node), docOffset, demoU, enc, path, out);
    path.push_back(idx);
    enumZddSetsRec(dd, Cudd_T(node), docOffset, demoU, enc, path, out);
    path.pop_back();
}

static std::vector<std::vector<std::string>> enumerateZddSets(DdManager* dd, DdNode* root,
                                                              int docOffset, uint64_t demoU,
                                                              TagEncoding enc) {
    std::vector<std::vector<std::string>> sets;
    std::vector<int> path;
    if (root != nullptr && root != Cudd_ReadZero(dd))
        enumZddSetsRec(dd, root, docOffset, demoU, enc, path, sets);
    return sets;
}

static void printZddFamily(const std::string& label, DdManager* dd, DdNode* z, int docOffset,
                           uint64_t demoU, TagEncoding enc) {
    std::cout << label;
    if (z == nullptr || z == Cudd_ReadZero(dd)) {
        std::cout << " = ZERO (vacio)\n";
        return;
    }
    const auto sets = enumerateZddSets(dd, z, docOffset, demoU, enc);
    std::cout << " = {";
    for (size_t i = 0; i < sets.size(); ++i) {
        std::cout << " {";
        for (size_t j = 0; j < sets[i].size(); ++j) {
            std::cout << sets[i][j];
            if (j + 1 < sets[i].size()) std::cout << ", ";
        }
        std::cout << "}";
        if (i + 1 < sets.size()) std::cout << ",";
    }
    std::cout << " }\n";
    std::cout << "  DagSize=" << Cudd_zddDagSize(z) << " |ZDD^t|=" << sets.size() << "\n";
}

static TermFtInputs makeDemoTerm(const std::vector<std::vector<uint64_t>>& snaps) {
    TermFtInputs td;
    td.parsed = true;
    td.versionSnapshots = snaps;
    td.versionSnapshotCount = static_cast<uint32_t>(snaps.size());
    td.nVersions = td.versionSnapshotCount;
    td.nPostings = td.versionSnapshotCount;
    return td;
}

static std::vector<std::string> buildDemoVarLabels(int numZddVars, int docOffset, uint64_t demoU,
                                                   TagEncoding enc) {
    std::vector<std::string> labels(static_cast<size_t>(numZddVars));
    for (int v = 0; v < numZddVars; ++v)
        labels[static_cast<size_t>(v)] = demoDotVarLabel(v, docOffset, demoU, enc);
    return labels;
}

static std::string dotEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

static std::string formatFamilyLegend(DdManager* dd, DdNode* root, int docOffset, uint64_t demoU,
                                      TagEncoding enc) {
    if (root == nullptr || root == Cudd_ReadZero(dd)) return "{}";
    const auto sets = enumerateZddSets(dd, root, docOffset, demoU, enc);
    std::string leg = "{ ";
    for (size_t i = 0; i < sets.size(); ++i) {
        leg += "{";
        for (size_t j = 0; j < sets[i].size(); ++j) {
            leg += sets[i][j];
            if (j + 1 < sets[i].size()) leg += ", ";
        }
        leg += "}";
        if (i + 1 < sets.size()) leg += ", ";
    }
    leg += " }";
    return leg;
}

class PrettyZddDot {
public:
    PrettyZddDot(DdManager* ddIn, int docOffsetIn, const std::vector<std::string>& varLabelsIn,
                 TagEncoding encIn, const std::string& idPrefix = "n")
        : dd(ddIn), docOffset(docOffsetIn), varLabels(varLabelsIn), enc(encIn), prefix(idPrefix) {}

    void buildFromRoot(DdNode* root) {
        if (root == nullptr || root == Cudd_ReadZero(dd)) return;
        walk(root);
    }

    std::string nodeIdFor(DdNode* n) const {
        auto it = nodeName.find(n);
        return (it != nodeName.end()) ? it->second : "";
    }

    void writeBody(std::ostream& os) const {
        if (enc == TagEncoding::UPlusT) {
            os << "  legend [shape=plaintext, label=\"1 = pertenece (solido)\\n"
               << "0 = no pertenece (punteado)\\n"
               << "amarillo = tag {u+t}   azul = master\\n"
               << "niveles: mayor arriba, menor abajo\"];\n";
        } else {
            os << "  legend [shape=plaintext, label=\""
               << "τk = bit k del term_id (LSB k=0, peso 2^k)\\n"
               << "φ(1)={τ0}→{u+1}   φ(2)={τ1}→{u+2}\\n"
               << "1=solido pertenece   0=punteado no\\n"
               << "amarillo=tag   azul=master\"];\n";
        }
        writeRankConstraints(os);
        for (const std::string& def : nodeDefs) os << "  " << def << "\n";
        for (const std::string& e : edges) os << "  " << e << "\n";
    }

    void write(std::ostream& os, const std::string& title, const std::string& familyLegend) const {
        os << "digraph \"" << dotEscape(title) << "\" {\n";
        os << "  graph [rankdir=TB, bgcolor=white, fontname=\"Helvetica\", label=\""
           << dotEscape(title) << "\\n" << dotEscape(familyLegend)
           << "\", labelloc=t, fontsize=14];\n";
        os << "  node [fontname=\"Helvetica\", fontsize=11];\n";
        os << "  edge [fontname=\"Helvetica\", fontsize=10];\n";
        writeBody(os);
        os << "}\n";
    }

private:
    DdManager* dd;
    int docOffset;
    const std::vector<std::string>& varLabels;
    TagEncoding enc;
    std::string prefix;
    std::unordered_map<DdNode*, std::string> nodeName;
    std::unordered_set<DdNode*> edgesDone;
    std::unordered_map<std::string, int> nodeLogicalLevel;
    std::vector<std::string> terminalNames;
    std::vector<std::string> nodeDefs;
    std::vector<std::string> edges;

    int logicalLevelOfVar(int var) const {
        if (enc == TagEncoding::LogBits && var >= static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET) &&
            var < docOffset) {
            return 10000 + var;
        }
        if (var >= 0 && static_cast<size_t>(var) < varLabels.size())
            return std::stoi(varLabels[static_cast<size_t>(var)]);
        return var;
    }

    std::string varLabel(int var) const {
        if (var >= 0 && static_cast<size_t>(var) < varLabels.size()) return varLabels[static_cast<size_t>(var)];
        return std::to_string(var);
    }

    bool isTagVar(int var) const { return var >= 0 && var < docOffset; }

    void writeRankConstraints(std::ostream& os) const {
        std::map<int, std::vector<std::string>, std::greater<int>> byLevel;
        for (const auto& kv : nodeLogicalLevel) byLevel[kv.second].push_back(kv.first);

        std::string prevAnchor;
        for (const auto& kv : byLevel) {
            if (kv.second.empty()) continue;
            os << "  { rank=same; ";
            for (const std::string& n : kv.second) os << n << "; ";
            os << "}\n";
            if (!prevAnchor.empty())
                os << "  " << prevAnchor << " -> " << kv.second.front()
                   << " [style=invis,weight=100];\n";
            prevAnchor = kv.second.front();
        }
        if (!terminalNames.empty()) {
            os << "  { rank=max; ";
            for (const std::string& t : terminalNames) os << t << "; ";
            os << "}\n";
            if (!prevAnchor.empty())
                os << "  " << prevAnchor << " -> " << terminalNames.front()
                   << " [style=invis,weight=100];\n";
        }
    }

    std::string idOf(DdNode* n) {
        auto it = nodeName.find(n);
        if (it != nodeName.end()) return it->second;
        const std::string name = prefix + std::to_string(nodeName.size());
        nodeName[n] = name;
        defineNode(name, n);
        return name;
    }

    void defineNode(const std::string& name, DdNode* n) {
        if (Cudd_IsConstant(n)) {
            terminalNames.push_back(name);
            if (n == Cudd_ReadZero(dd)) {
                nodeDefs.push_back(name +
                                   " [shape=box,label=\"0\",style=filled,fillcolor=\"#e0e0e0\","
                                   "width=0.5,height=0.3,fixedsize=true];");
            } else {
                nodeDefs.push_back(name +
                                   " [shape=box,label=\"1\",style=filled,fillcolor=\"#c8e6c9\","
                                   "width=0.5,height=0.3,fixedsize=true];");
            }
            return;
        }
        const int var = Cudd_NodeReadIndex(n);
        nodeLogicalLevel[name] = logicalLevelOfVar(var);
        const std::string vlab = varLabel(var);
        const char* color = isTagVar(var) ? "#fff9c4" : "#bbdefb";
        nodeDefs.push_back(name + " [shape=diamond,label=\"" + dotEscape(vlab) +
                           "\",style=filled,fillcolor=\"" + color + "\",width=0.9,height=0.9,fixedsize=false];");
    }

    void walk(DdNode* n) {
        if (n == nullptr || n == Cudd_ReadZero(dd)) {
            idOf(Cudd_ReadZero(dd));
            return;
        }
        if (edgesDone.count(n)) return;
        const std::string from = idOf(n);
        if (Cudd_IsConstant(n)) {
            edgesDone.insert(n);
            return;
        }
        edgesDone.insert(n);
        DdNode* lo = Cudd_E(n);
        DdNode* hi = Cudd_T(n);
        const std::string loId = idOf(lo);
        const std::string hiId = idOf(hi);
        edges.push_back(from + " -> " + loId + " [label=\"0\",style=dashed,color=\"#666666\";];");
        edges.push_back(from + " -> " + hiId + " [label=\"1\",style=solid,color=\"#1565c0\";];");
        walk(lo);
        walk(hi);
    }
};

static bool writePrettyZddDot(const std::string& path, DdManager* dd, DdNode* root, int docOffset,
                              uint64_t demoU, TagEncoding enc,
                              const std::vector<std::string>& varLabels, const std::string& title) {
    if (root == nullptr || root == Cudd_ReadZero(dd)) return false;
    PrettyZddDot builder(dd, docOffset, varLabels, enc, "n");
    builder.buildFromRoot(root);
    const std::string legend = formatFamilyLegend(dd, root, docOffset, demoU, enc);
    std::ofstream out(path);
    if (!out.is_open()) return false;
    builder.write(out, title, legend);
    return true;
}

static bool writePrettyBosqueDot(const std::string& path, DdManager* dd,
                                 const std::vector<DdNode*>& roots,
                                 const std::vector<std::string>& titles, int docOffset,
                                 uint64_t demoU, TagEncoding enc,
                                 const std::vector<std::string>& varLabels) {
    PrettyZddDot builder(dd, docOffset, varLabels, enc, "n");
    for (DdNode* root : roots) builder.buildFromRoot(root);

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << "digraph bosque_demo {\n";
    const char* encLabel = (enc == TagEncoding::UPlusT) ? "tag {u+t}" : "tag phi(t) binario";
    out << "  graph [rankdir=TB, bgcolor=white, fontname=\"Helvetica\", "
           "label=\"Bosque ZDD^t compartido (u=4, V=2) — "
        << encLabel << "\\nnodos compartidos en pool CUDD\", "
           "labelloc=t, fontsize=14];\n";
    out << "  node [fontname=\"Helvetica\", fontsize=11];\n";
    out << "  edge [fontname=\"Helvetica\", fontsize=10];\n";

    for (size_t i = 0; i < roots.size(); ++i) {
        if (roots[i] == nullptr || roots[i] == Cudd_ReadZero(dd)) continue;
        const std::string title = (i < titles.size()) ? titles[i] : ("ZDD^" + std::to_string(i + 1));
        const std::string family = formatFamilyLegend(dd, roots[i], docOffset, demoU, enc);
        const std::string entry = "entry" + std::to_string(i);
        const std::string rootId = builder.nodeIdFor(roots[i]);
        if (rootId.empty()) continue;
        out << "  " << entry << " [shape=plaintext,label=\"" << dotEscape(title) << "\\n"
            << dotEscape(family) << "\"];\n";
        out << "  " << entry << " -> " << rootId << " [style=bold,color=\"#424242\",penwidth=2];\n";
    }

    builder.writeBody(out);
    out << "}\n";
    return true;
}

static bool dotToPng(const std::string& dotPath, const std::string& pngPath) {
    const std::string cmd = "dot -Tpng \"" + dotPath + "\" -o \"" + pngPath + "\"";
    return std::system(cmd.c_str()) == 0;
}

static int cmdDemo(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[2], enc)) return 1;
    const std::string defaultOut =
        (enc == TagEncoding::UPlusT) ? "resultados_test/demo_u4_t2" : "resultados_test/demo_u4_t2_bin";
    const std::string outDir = (argc >= 4) ? argv[3] : defaultOut;
    mkdir(outDir.c_str(), 0755);
    constexpr uint64_t DEMO_U = 4;
    constexpr uint32_t V = 2;

    std::vector<TermFtInputs> termData(V);
    termData[0] = makeDemoTerm({{1, 2}, {1, 2, 3}});
    termData[1] = makeDemoTerm({{0}});

    int docOffset = 0;
    int numZddVars = 0;
    uint32_t tagWidth = 0;
    if (!computeZddLayout(3, V, enc, docOffset, numZddVars, tagWidth)) return 1;

    std::cout << "=== Demo toy: u=" << DEMO_U << ", V=" << V << " enc=" << tagEncodingName(enc)
              << " ===\n";
    std::cout << "Universo: masters [0," << DEMO_U << "), tags {u+1}..{u+" << V << "}\n";
    if (enc == TagEncoding::UPlusT) {
        std::cout << "  {u+1}=" << (DEMO_U + 1) << "  {u+2}=" << (DEMO_U + 2) << "\n";
    } else {
        std::cout << "tagWidth=" << tagWidth << "\n";
        std::cout << "  τ_k = var CUDD del bit k (LSB k=0)\n";
        std::cout << "  φ(1)=01 -> {τ0} -> {u+1}=" << (DEMO_U + 1)
                  << "   φ(2)=10 -> {τ1} -> {u+2}=" << (DEMO_U + 2) << "\n";
    }
    std::cout << "docOffset=" << docOffset << " numZddVars=" << numZddVars << "\n\n";

    DdManager* dd = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) return 1;
    configureCuddManager(dd);

    std::unordered_map<std::string, DdNode*> snapshotCache;
    std::vector<DdNode*> sharedSnapshots;
    std::vector<DdNode*> pointerList;
    if (!buildFtPointersForRange(dd, termData, 0, V, docOffset, enc, snapshotCache, sharedSnapshots,
                                 pointerList)) {
        Cudd_Quit(dd);
        return 1;
    }

    const char* encDesc =
        (enc == TagEncoding::UPlusT) ? "ZDD^t = F_t U {{u+t}} (var t = tag CUDD)" :
                                       "ZDD^t = F_t U {phi(t)}; decode(phi(t))={u+t}";
    std::cout << encDesc << "\n\n";
    for (uint32_t t = 0; t < V; ++t) {
        const uint32_t termOneBased = t + 1u;
        std::cout << "--- termino t=" << termOneBased << " ---\n";
        printZddFamily("ZDD^" + std::to_string(termOneBased), dd, pointerList[t], docOffset, DEMO_U,
                       enc);
        if (enc == TagEncoding::UPlusT) {
            std::cout << "  tag CUDD var=" << termOneBased << " singleton={u+" << termOneBased
                      << "}=" << (DEMO_U + termOneBased) << "\n\n";
        } else {
            const int decoded = readTermIdFromTagSearch(dd, pointerList[t], docOffset, enc);
            std::cout << "  tag_decoded=" << decoded << " esperado={u+" << termOneBased << "}="
                      << (DEMO_U + termOneBased) << "\n\n";
        }
    }

    const auto varLabels = buildDemoVarLabels(numZddVars, docOffset, DEMO_U, enc);
    const std::string dot1 = outDir + "/zdd_demo_t1.dot";
    const std::string dot2 = outDir + "/zdd_demo_t2.dot";
    const std::string dotB = outDir + "/zdd_demo_bosque.dot";
    const std::string png1 = outDir + "/zdd_demo_t1.png";
    const std::string png2 = outDir + "/zdd_demo_t2.png";
    const std::string pngB = outDir + "/zdd_demo_bosque.png";

    if (writePrettyZddDot(dot1, dd, pointerList[0], docOffset, DEMO_U, enc, varLabels, "ZDD^1")) {
        std::cout << "[DOT] " << dot1 << "\n";
        if (dotToPng(dot1, png1)) std::cout << "[PNG] " << png1 << "\n";
    }
    if (writePrettyZddDot(dot2, dd, pointerList[1], docOffset, DEMO_U, enc, varLabels, "ZDD^2")) {
        std::cout << "[DOT] " << dot2 << "\n";
        if (dotToPng(dot2, png2)) std::cout << "[PNG] " << png2 << "\n";
    }
    if (writePrettyBosqueDot(dotB, dd, {pointerList[0], pointerList[1]}, {"ZDD^1", "ZDD^2"},
                             docOffset, DEMO_U, enc, varLabels)) {
        std::cout << "[DOT] " << dotB << "\n";
        if (dotToPng(dotB, pngB)) std::cout << "[PNG] " << pngB << "\n";
    }

    ZddPack::freeTermZdd(dd, pointerList);
    for (DdNode* s : sharedSnapshots) Cudd_RecursiveDerefZdd(dd, s);
    Cudd_Quit(dd);
    return 0;
}

#endif  // ZDD_DEMO_VIZ_H
