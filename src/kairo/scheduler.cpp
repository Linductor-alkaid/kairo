#include "kairo/scheduler.hpp"

namespace kairo {

// M1 pipeline 化：约束过滤 → 候选生成 → 评分/排序 → 选择。
// 四阶段组件见 scheduling_pipeline.hpp；行为与 0.6.1 逐项一致。
RoutingDecision DefaultScheduler::route(
    const TaskRouter::Request& request,
    const std::vector<ExecutorCapability>& capabilities) {
    return pipeline_.route(request, capabilities);
}

}  // namespace kairo
