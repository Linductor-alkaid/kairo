#include "kairo/scheduling.hpp"

namespace kairo {

const char* qos_class_to_string(QosClass qos) noexcept {
    switch (qos) {
    case QosClass::BestEffort:
        return "BestEffort";
    case QosClass::Interactive:
        return "Interactive";
    case QosClass::Critical:
        return "Critical";
    case QosClass::Standard:
    default:
        return "Standard";
    }
}

}  // namespace kairo
