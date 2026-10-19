#pragma once

// test_gpu_timing_feedback 的私有注入辅助（MSVC 兼容）。
//
// 该测试需要把假 IGpuExecutor 注册进 facade 的 manager_（公开 API 只能
// 创建真实后端）。仓库惯用的 `#define private public` 在 MSVC 上不可用：
// MSVC 修饰名编码访问权限，重定义后测试 TU 实例化的提交模板会引用
// public 形（Q…）符号，而库内定义是 private 形（A…）→ LNK2019。
// 因此注册动作被隔离到本 TU（只触达数据成员与 ExecutorManager 的公开
// 方法，不产生跨 TU 的成员函数符号引用），主测试 TU 保持干净 include，
// 模板按真实访问权限实例化。

#include <memory>
#include <string>

namespace kairo {
class Executor;
class IGpuExecutor;
}  // namespace kairo

namespace gpu_timing_test {

// 经 facade 的 manager_ 注册假执行器（假执行器对象由调用方持有探针）。
void register_fake_gpu(kairo::Executor& executor, const std::string& name,
                       std::unique_ptr<kairo::IGpuExecutor> fake);

}  // namespace gpu_timing_test
