#include <tdzdd/DdSpec.hpp>
#include <tdzdd/DdSpecOp.hpp>
#include <tdzdd/DdStructure.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <vector>

class FastPathSpec : public tdzdd::StatelessDdSpec<FastPathSpec, 2> {
    std::vector<int> sortedItems;

public:
    explicit FastPathSpec(std::set<int> const& s) {
        sortedItems.reserve(s.size());
        for (int v : s) sortedItems.push_back(v);
        std::sort(sortedItems.rbegin(), sortedItems.rend());
    }

    int getRoot() const {
        if (sortedItems.empty()) return -1;
        return sortedItems.front();
    }

    int getChild(int level, int value) const {
        if (value == 0) return 0;
        auto it = std::upper_bound(sortedItems.begin(), sortedItems.end(), level, std::greater<int>());
        if (it == sortedItems.end()) return -1;
        return *it;
    }
};

int main() {
    std::set<int> s1 = {1, 7, 8};
    std::set<int> s2 = {1, 2, 4};

    tdzdd::DdStructure<2> dd{FastPathSpec(s1)};
    dd = tdzdd::DdStructure<2>(tdzdd::zddUnion(dd, FastPathSpec(s2)));
    dd.zddReduce();

    std::ofstream out("resultados_test/zdd_ejemplo_S_tdzdd.dot");
    dd.dumpDot(out, "S={{1,7,8},{1,2,4}}");
    return 0;
}
