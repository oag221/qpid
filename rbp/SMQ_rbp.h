#pragma once

#include <vector>
#include <array>

class MRF;

namespace smq_rbp {

    void solve(MRF* mrf, double sensitivity,
           std::vector<std::array<double,2> >* answer,
           int threadNum);
}