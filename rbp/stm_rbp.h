#pragma once

#include <vector>
#include <array>

class MRF;

namespace skiphash_rbp {

void solve(MRF* mrf, double sensitivity,
           std::vector<std::array<double,2> >* answer,
           int threadNum, int queueNum, int chunkSize, int delta,
           bool batch_ins, bool strict);

}
