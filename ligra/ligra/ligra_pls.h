#ifndef LIGRA_PLS_H
#define LIGRA_PLS_H


#include "swarm/api.h"
#include "swarm/algorithm.h"

#include <cstdlib>
//#include "utils.h"
#include "vertex.h"
#include "vertexSubset.h"
#include "graph.h"

#include "edgeMap_utils.h"

template <class data, class vertex, class VS, class F>
void pls_edgeMapDense(swarm::Timestamp ts, graph<vertex>& GA,
                      VS& vertexSubset, F* f) {
    using D = tuple<bool, data>;
    size_t n = GA.n;
    vertex *G = GA.V;
    auto g = get_emdense_nooutput_gen<data>();
    swarm::enqueue_all_progressive<swarm::max_children/2>(
            swarm::u32it(0), swarm::u32it(n),
            [G, &vertexSubset, f, g] (swarm::Timestamp ts, uintE v) {
        swarm::enqueueLambda([G, &vertexSubset, f, v, g] (swarm::Timestamp ts) {
            if (f->cond(v)) {
                G[v].decodeInNghBreakEarly(v, vertexSubset, *f, g);
            }
        }, ts,
#ifdef PLS_LIGRA_COARSE_GRAIN
        f->hintCG(v)
#else
        // All this task does is enqueue more tasks, so to use resources well,
        // tag it as PRODUCER. Note: soft priorities would have a nice use case
        // here. We want the decodeInNghBreakEarly lambda to run before any
        // enqueue task in the progressive enqueue. That's hard to express.
        EnqFlags(NOHINT | PRODUCER | MAYSPEC)
#endif
        );
    },
    [ts] (uintE) { return ts; },
    [] (uintE) { return EnqFlags(NOHINT | MAYSPEC); });
}


template <class VS, class F>
void pls_vertexMapDense(swarm::Timestamp ts, VS& V, F* f) {
    size_t n = V.numRows(), m = V.numNonzeros();
    assert(V.dense());
    swarm::enqueue_all_progressive<swarm::max_children/2>(
            swarm::u32it(0), swarm::u32it(n),
            [&V, f] (swarm::Timestamp ts, uintE i) {
        swarm::enqueueLambda([&V, f, i] (swarm::Timestamp) {
            if (V.isIn(i)) (*f)(i);
        }, ts, f->hint(i));
    },
    [ts] (uintE) { return ts; },
    [] (uintE) { return EnqFlags(NOHINT | MAYSPEC); });
}


#endif //LIGRA_PLS_H
