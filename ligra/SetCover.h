#pragma once

#include "ligra.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace setcover {

template <class vertex, class collection>
inline bool success(const graph<vertex>& G, const collection& cover) {
    std::cout << "|V| = " << G.n << " |E| = " << G.m << std::endl;
    std::cout << "|cover|: " << cover.size() << std::endl;

    std::vector<bool> isElemCovered(G.n, false);
    // Handle directed graphs: a vertex with no incoming edges will translate to
    // an element in no set. This isn't really the spirit of set cover, as the
    // union of all sets should equal the universe. Just mark it covered.
    for (size_t elem = 0; elem < G.n; elem++) {
        if (!G.V[elem].getInDegree()) isElemCovered[elem] = true;
    }

    // Validate feasibility of the solution:
    // 1) All elements should be covered by the set cover
    // 2) No set in the cover should be redundant (i.e. all its elements are
    //    already covered) 
    // 3) N.B. validating optimality is hard (this is NP comlete). Maybe we
    //    could check the H_n-approximate guarantee? But then we'd need to know
    //    the optimal solution...
    //
    //GP: The greedy algo can produce some redundant sets while running correctly so
    //remove this check
    //std::vector<uint64_t> redundantSets;
    for (auto s : cover) {
        size_t sD = G.V[s].getOutDegree();
        //bool redundant = true;
        for (size_t i = 0; i < sD; i++) {
            size_t elem = G.V[s].getOutNeighbor(i);
            if (!isElemCovered[elem]) {
                //redundant = false;
                isElemCovered[elem] = true;
            }
        }
        //if (redundant) redundantSets.push_back(s);
    }

    auto isTrue = [] (bool b) { return b; };
    auto it = std::find(isElemCovered.begin(), isElemCovered.end(), false);
    bool ec = (it == isElemCovered.end());
    bool size = cover.size() <= G.n;
    //bool red = !redundantSets.empty();
    if (!ec) {
        std::cerr << "ERROR: element " << std::distance(isElemCovered.begin(), it)
                  << " was not covered"
                  << std::endl;
    }
    if (!size) std::cerr << "ERROR: the subcover was too large" << std::endl;
    /*if (red) {
        std::cerr << "ERROR: found redundant sets in the cover: " << std::endl;
        size_t count = 10;
        for (auto s : redundantSets) {
            std::cerr << s << std::endl;
            if (count-- == 0) break;
        }
    }*/
    bool success = (ec && size/* && !red*/);
    if (success) std::cout << "Validation succeeded" << std::endl;
    return success;
}

}
