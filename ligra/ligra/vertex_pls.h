#ifndef VERTEX_PLS_H
#define VERTEX_PLS_H

#include "vertexSubset.h"

#include "swarm/api.h"
#include "swarm/algorithm.h"

namespace decode_uncompressed {

template <class vertex, class F, class G, class VS>
inline void pls_decodeInNghBreakEarlyCG(vertex* v, uintE v_id,
        VS& vertexSubset, F& f, G g) {
    uintE d = v->getInDegree();
    for (uint64_t j = 0; j < d; j++) {
        uintE ngh = v->getInNeighbor(j);
        if (vertexSubset.isIn(ngh)) {
#ifndef WEIGHTED
            auto m = f.update(ngh, v_id);
#else
            auto m = f.update(ngh, v_id, v->getInWeight(j));
#endif
            g(v_id, m);
        }
        if (!f.cond(v_id)) break;
    }
}

// FIXME(mcj) This Swarmified version doesn't break early. We'd need to
// heap-allocate a done variable, and strictly order the iterations in a Fractal
// domain.
#ifndef WEIGHTED
template <class vertex, class F, class G, class VS>
inline void pls_decodeInNghFG(vertex* v, uintE v_id,
        VS& vertexSubset, F& f, G g) {
    uintE d = v->getInDegree();
    swarm::enqueue_all_progressive<swarm::max_children>(
            v->getInNeighbors(), v->getInNeighbors() + d,
            [&vertexSubset, &f, v_id, g] (swarm::Timestamp ts, uintE ngh) {
        if (vertexSubset.isIn(ngh)) {
            swarm::enqueueLambda([&f, v_id, ngh, g] (swarm::Timestamp) {
                auto m = f.updateAtomic(ngh, v_id);
                g(v_id, m);
            }, ts, f.hintFG(ngh, v_id));
        }
    },
    [] (uintE) { return swarm::timestamp(); },
    [] (const uintE& n) -> swarm::Hint {
        return {swarm::Hint::cacheLine(&n), EnqFlags::MAYSPEC};
    });
}

#else

template <class vertex, class F, class G, class VS>
inline void pls_decodeInNghFG(vertex* v, uintE v_id,
        VS& vertexSubset, F& f, G g) {
    uintE d = v->getInDegree();
    swarm::enqueue_all_progressive<swarm::max_children>(
            swarm::u32it(0), swarm::u32it(d),
            [&vertexSubset, v, &f, v_id, g] (swarm::Timestamp ts, uint32_t j) {
        uintE ngh = v->getInNeighbor(j);
        if (vertexSubset.isIn(ngh)) {
            intE weight = v->getInWeight(j);
            swarm::enqueueLambda([&f, v_id, ngh, weight, g]
                    (swarm::Timestamp) {
                auto m = f.updateAtomic(ngh, v_id, weight);
                g(v_id, m);
            }, ts, f.hintFG(ngh, v_id));
        }
    },
    [] (uint32_t) { return swarm::timestamp(); },
    [] (uint32_t) { return EnqFlags(NOHINT | MAYSPEC); });
}

#endif


// Used by edgeMapDense. Callers ensure cond(v_id). For each vertex, decode
// its in-edges, and check to see whether this neighbor is in the current
// frontier, calling update if it is. If processing the edges sequentially,
// break once !cond(v_id).
template <class vertex, class F, class G, class VS>
inline void pls_decodeInNghBreakEarly(vertex* v, uintE v_id,
        VS& vertexSubset, F& f, G g) {
#ifdef PLS_LIGRA_COARSE_GRAIN
    pls_decodeInNghBreakEarlyCG(v, v_id, vertexSubset, f, g);
#else
    pls_decodeInNghFG(v, v_id, vertexSubset, f, g);
#endif
}

}

#endif // VERTEX_PLS_H
