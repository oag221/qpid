#pragma once

#include <vector>
#include <array>

class MRF;

namespace competitors_rbp {

    void solve(std::string qType, MRF* mrf, double sensitivity,
           std::vector<std::array<double,2> >* answer,
           int threadNum);

}

