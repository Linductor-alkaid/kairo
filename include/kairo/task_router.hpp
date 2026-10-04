#pragma once

#include "task_options.hpp"

#include <optional>
#include <vector>

namespace kairo {

/**
 * @brief Internal policy component for automatic submission and dispatch.
 *
 * It only evaluates immutable task options and advisory capability snapshots;
 * the facade remains responsible for the actual backend submission.
 */
class TaskRouter {
public:
    struct Request {
        TaskOptions options;
        bool cpu_gpu_task = false;
        std::optional<bool> gpu_selected;
    };

    RoutingDecision route(const Request& request,
                          const std::vector<ExecutorCapability>& capabilities) const;
    RoutingDecision route_dispatch(const TaskOptions& options,
                                   const std::vector<ExecutorCapability>& capabilities) const;
};

}  // namespace kairo
