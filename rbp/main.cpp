#include <cassert>
#include <iostream>

#include <cmath>
#include <string>
#include <fstream>
#include <vector>
#include <array>
#include <optional>

#include "examples_mrf.h"
#include "mrf.h"
#include "residual_bp.h"
#include "relaxed-rbp.h"
#include "bucket_rbp.h"
#include "heap_rbp.h"
#include "stm_rbp.h"
#include "SMQ_rbp.h"
#include "compet_rbp.h"

using Results = std::vector<std::array<double,2>>;

int main(int argc, const char** argv) {
    if (argc < 11) {
        std::cerr << "Usage: "
                  << argv[0]
                  << " <algorithm>"
                  << " <mrf>"
                  << " <size>"
                  << " [<threads>]"
                  << " [<queues>]"
                  << " [<batchPop>]"
                  << " [<batchPush>]"
                  << " [<delta>]"
                  << " [<buckets>]"
                  << " [<usePrefetch>]"
                  << " [<stickiness>]"
                  << " [<chunksize>]"
                  << " [<batch_ins>]"
                  << " [<strict>]"
                  << std::endl;
        return -1;
    }

    std::string algorithm(argv[1]);
    std::string mrfName(argv[2]);
    uint64_t size = atol(argv[3]);
    uint64_t threadNum = atol(argv[4]);
    uint64_t queueNum = atol(argv[5]);
    uint64_t batchSizePop = atol(argv[6]);
    uint64_t batchSizePush = atol(argv[7]);
    uint64_t delta = atol(argv[8]);
    uint64_t bucketNum = atol(argv[9]);
    uint64_t usePrefetch = atol(argv[10]);
    uint64_t stickiness = atol(argv[11]);
    uint64_t chunksize = atol(argv[12]);
    uint64_t batch_ins = atol(argv[13]);
    uint64_t strict = atol(argv[14]);
    assert(size > 0);

    assert(argc == 15 || algorithm == "relaxed-rbp");

    MRF* mrf = nullptr;
    if (mrfName == "ising") {
        mrf = examples_mrf::isingMRF(size, 2, 1);
    } else if (mrfName == "potts") {
        mrf = examples_mrf::pottsMRF(size, 5, 1);
    } else if (mrfName == "tree") {
        mrf = examples_mrf::randomTree(size, 5, 1);
    } else if (mrfName == "deterministic_tree") {
        mrf = examples_mrf::deterministicTree(size);
    } else {
        std::cerr << "Unrecognized MRF: " << mrfName << std::endl;
        return 1;
    }

    assert(mrf);
    std::cout << "The " << mrfName << " model has been set up" << std::endl;

    double sensitivity = 1e-5;
    Results res;

    if (algorithm == "residual") {
        residual_bp::solve(mrf, sensitivity, &res);
    } else if (algorithm == "bucket") {
        bucket_rbp::solve(
            mrf, sensitivity, &res, threadNum, queueNum, batchSizePop, batchSizePush,
            delta, bucketNum, usePrefetch, stickiness);        
    } else if (algorithm == "heap") {
        heap_rbp::solve(
            mrf, sensitivity, &res, threadNum, queueNum, batchSizePop, batchSizePush,
            usePrefetch, stickiness);  
    } else if (algorithm == "relaxed-rbp") {
        relaxed_rbp::solve(mrf, sensitivity, threadNum, &res);
    } else if (algorithm == "skiphashpq-rbp") {
        skiphash_rbp::solve(mrf, sensitivity, &res, threadNum, queueNum, chunksize, delta, batch_ins, strict);
    } else if (algorithm == "smq") {
        smq_rbp::solve(mrf, sensitivity, &res, threadNum);
    } else if (algorithm == "spray" || algorithm == "linden" || algorithm == "pipq") {
        competitors_rbp::solve(algorithm, mrf, sensitivity, &res, threadNum);
    } else {
        std::cerr << "Unrecognized algorithm: " << algorithm << std::endl;
        return 1;
    }

    delete mrf;
    assert(!res.empty());

    if (mrfName == "deterministic_tree") {
        for (uint64_t i = 0; i < size; i++) {
            if (std::abs (res[i][0] - 0.1) > 0.001) {
                std::cerr << "Something is wrong with vertex "
                          << i << std::endl;
                return 1;
            }
        }
        std::cout << "Everything is fine\n";
    }

    std::string arg3 = std::to_string(size);
    std::string filename("output-" + mrfName + "-" + arg3);
    uint64_t len = res.size();
    uint64_t wid = res[0].size();
    if (algorithm == "residual") {
        std::ofstream ostrm(filename);
        for (uint64_t i = 0; i < len; i++) {
            for (uint64_t j = 0; j < wid; j++) {
                ostrm << res[i][j] << " ";
            }
            ostrm << "\n";
        }
    } else {
        std::ifstream istrm(filename);
        if (! istrm.good()) {
            std::cerr <<  "Correctness-checking file does not exist "
                      << filename << std::endl;
            return 1;
        }

        Results jury(res.size());

        for (uint64_t i = 0; i < len; i++) {
            for (uint64_t j = 0; j < wid; j++) {
                double d1;
                istrm >> d1;
                jury[i][j] = d1;
            }
        }

        double accuracy = 0;
        double accuracyMax = 0;
        for (uint64_t i = 0; i < len; i++) {
            double L1 = 0;
            for (uint64_t j = 0; j < wid; j++) {
                L1 += std::abs(jury[i][j] - res[i][j]);
            }
            accuracy += L1;
            accuracyMax = std::max(accuracyMax, L1);
        }
        std::cout << "Accuracy:" << std::to_string(accuracy / res.size()) << std::endl;
        std::cout << "AccuracyMax:" << std::to_string(accuracyMax) << std::endl;
    }
    std::cout << "The first results are "
              << res[0][0]
              << " and "
              << res[0][1]
              << std::endl;

    // TODO(mcj) results to file like the Java version.

    return 0;
}


