// =============================================================================
// plus_t/utils/bpi_scan.h — escaneo de denominadores BPI sobre .docs
// =============================================================================
// Requiere nzdd_cudd_common.h y version_packing.h en el include path.
// =============================================================================

#ifndef PLUS_T_UTILS_BPI_SCAN_H
#define PLUS_T_UTILS_BPI_SCAN_H

#include "utils/bpi.h"

#include "nzdd_cudd_common.h"
#include "version_packing.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NzddBpi {

enum class DenomMode { RawOnly, FullAudit };

inline Denominators scanDocsRawOnly(const std::string& docsPath, uint32_t maxTerms) {
    Denominators d;
    std::ifstream docs(docsPath, std::ios::binary);
    if (!docs) return d;
    docs.read(reinterpret_cast<char*>(&d.nlists), sizeof(d.nlists));
    if (!docs) return d;
    const uint32_t limit =
        (maxTerms == 0u || maxTerms > d.nlists) ? d.nlists : maxTerms;
    d.termsScanned = limit;
    for (uint32_t t = 0; t < limit; ++t) {
        uint32_t len = 0;
        docs.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!docs) return d;
        d.nRaw += len;
        docs.seekg(static_cast<std::streamoff>(sizeof(uint64_t) * len), std::ios::cur);
    }
    d.deepScanned = false;
    return d;
}

inline Denominators scanDocsFullAudit(const std::string& docsPath, uint32_t maxTerms) {
    Denominators d;
    const NzddCommon::DocsIndex idx = NzddCommon::buildDocsIndex(docsPath);
    if (idx.nlists == 0u || idx.listOffsets.empty()) return d;
    d.nlists = idx.nlists;
    const uint32_t limit =
        (maxTerms > 0u && maxTerms < idx.nlists) ? maxTerms : idx.nlists;
    d.termsScanned = limit;

    uint64_t nRaw = 0, nPairs = 0, nSnapElems = 0, nMasters = 0;
    uint64_t nVersions = 0, nSnapshots = 0, emptyTerms = 0;
    uint64_t maxMaster = 0, maxRel = 0;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 16)                                          \
    reduction(+ : nRaw, nPairs, nSnapElems, nMasters, nVersions, nSnapshots, emptyTerms) \
    reduction(max : maxMaster, maxRel)
#endif
    for (int t = 0; t < static_cast<int>(limit); ++t) {
        std::vector<uint64_t> postings;
        if (!NzddCommon::readPostingListAt(docsPath,
                                           idx.listOffsets[static_cast<uint32_t>(t)],
                                           postings))
            continue;
        nRaw += postings.size();
        if (postings.empty()) {
            emptyTerms += 1u;
            continue;
        }

        std::vector<uint64_t> uniq = postings;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        nPairs += uniq.size();

        std::unordered_map<uint64_t, std::vector<uint64_t>> byRel;
        std::unordered_set<uint64_t> mastersAll;
        for (uint64_t x : postings) {
            const uint64_t m = ZDD_UNPACK_MASTER(x);
            const uint64_t r = ZDD_UNPACK_REL(x);
            byRel[r].push_back(m);
            mastersAll.insert(m);
            if (m > maxMaster) maxMaster = m;
            if (r > maxRel) maxRel = r;
        }
        nVersions += byRel.size();
        nMasters += mastersAll.size();

        std::unordered_set<std::string> seen;
        for (auto& kv : byRel) {
            std::vector<uint64_t>& ms = kv.second;
            std::sort(ms.begin(), ms.end());
            ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
            if (ms.empty()) continue;
            std::string key(reinterpret_cast<const char*>(ms.data()),
                            ms.size() * sizeof(uint64_t));
            if (seen.insert(std::move(key)).second) {
                nSnapshots += 1u;
                nSnapElems += ms.size();
            }
        }
    }

    d.nRaw = nRaw;
    d.nPairsUniq = nPairs;
    d.nSnapElems = nSnapElems;
    d.nMastersUniq = nMasters;
    d.versionsTotal = nVersions;
    d.snapshotsDistinct = nSnapshots;
    d.emptyTerms = emptyTerms;
    d.maxMaster = maxMaster;
    d.maxRel = maxRel;
    d.dupPostings = (nRaw > nPairs) ? (nRaw - nPairs) : 0u;
    d.deepScanned = true;
    return d;
}

inline Denominators scanDocs(const std::string& docsPath, uint32_t maxTerms, DenomMode mode) {
    if (mode == DenomMode::FullAudit) return scanDocsFullAudit(docsPath, maxTerms);
    return scanDocsRawOnly(docsPath, maxTerms);
}

}  // namespace NzddBpi

#endif  // PLUS_T_UTILS_BPI_SCAN_H
