#include "termination_detector_2.h"

// boost::optional<tuple<uint,GNode>>      boost::optional<tuple<uint, GNode>>
// std::optional<std::pair<uint,GNode>>    std::optional<std::pair<uint, GNode>>

template<typename RetType, typename ExtractMethod>
RetType try_extract(termination_detector_2& detector, ExtractMethod func) {
    // first, try removing normally
    auto ret = func();

    if (!ret) {
        // ret is nullopt -> initiate termination detection
        auto num = detector.increment_idle();
        while (true) {
            ret = func();
            if (ret) break;
            if (detector.terminate(num)) return {}; // TERMINATE
            
            num = detector.get_idle();
        }
        detector.decrement_idle();
    }
    
    return ret;
}
