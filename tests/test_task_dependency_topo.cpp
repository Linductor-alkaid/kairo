// CR-052 验收测试：TaskDependencyManager 的 Pearce-Kelly 风格增量拓扑序。
//
// 记号约定：add(u, v) 表示 "u 依赖 v"（边 u→v）；"x ⇝ y" 表示 x 沿依赖边
// 可达 y（x 传递依赖于 y）。加边 u→v 成环 ⟺ v ⇝ u 已可达；u ⇝ v 已可达时
// 新边只是传递冗余，必须接受。链统一按 "t_i 依赖 t_{i-1}" 构建（与
// review_verification/cr052_dependency_dfs.cpp scale 模式一致）。
//
// 覆盖面：
//   1. 环检测契约矩阵（链式 closing edge / 菱形 DAG / 自依赖 / 重复边 / 互指）
//   2. 多父汇合形态（菱形汇点反向加边拒绝）
//   3. remove_dependency / prune 之后拓扑序维护的正确性
//   4. 规模回归（迁移 review_verification/cr052_dependency_dfs.cpp 的 scale 模式：
//      正序链 n=20000 建链 < 1000ms；closing edge 拒绝；逆序建链无误报环）
//   5. 随机对拍（固定种子 mt19937，与内联暴力 DFS 全一致）
//
// 线程安全由既有 shared_mutex 语义保证，本文件只做单线程契约验证。

#include <kairo/task/task_dependency_manager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

using kairo::TaskDependencyManager;

namespace {

std::string tn(int i) { return "t" + std::to_string(i); }

double elapsed_ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

// 对拍用暴力环检测：加入 u→v（u 依赖 v）成环 ⟺ 从 v 出发沿依赖边可达 u。
// 与 /tmp/cr052_min.cpp 及 review_verification/cr052_dependency_dfs.cpp 的
// 判定完全同构。
bool brute_cycle(const std::vector<std::set<int>>& adj, int u, int v) {
    std::vector<int> stack{v};
    std::set<int> visited{v};
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        if (cur == u) {
            return true;
        }
        for (int next : adj[cur]) {
            if (visited.insert(next).second) {
                stack.push_back(next);
            }
        }
    }
    return false;
}

void expect_contains(const std::vector<std::string>& deps,
                     const std::string& value) {
    EXPECT_NE(std::find(deps.begin(), deps.end(), value), deps.end())
        << "expected dependency " << value << " not found";
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 环检测契约矩阵
// ---------------------------------------------------------------------------

TEST(DependencyTopoCycleContract, ChainClosingEdgesRejected) {
    TaskDependencyManager mgr;
    // 链：t_i 依赖 t_{i-1}（可达性 t4 ⇝ t3 ⇝ t2 ⇝ t1 ⇝ t0）
    for (int i = 1; i <= 4; ++i) {
        ASSERT_TRUE(mgr.add_dependency(tn(i), tn(i - 1)));
    }
    ASSERT_EQ(mgr.get_stats().edge_count, 4u);

    // 首尾闭合：t0 依赖 t4（t4 ⇝ t0 已可达）→ 成环拒绝
    EXPECT_FALSE(mgr.add_dependency(tn(0), tn(4)))
        << "closing edge t0->t4 must be rejected";
    // 中间跨级闭合：t0 依赖 t3、t1 依赖 t4 → 成环拒绝
    EXPECT_FALSE(mgr.add_dependency(tn(0), tn(3)))
        << "closing edge t0->t3 must be rejected";
    EXPECT_FALSE(mgr.add_dependency(tn(1), tn(4)))
        << "closing edge t1->t4 must be rejected";
    // 直连回边（相邻级反向，即已有边 t_i→t_{i-1} 的逆向）：t1 依赖 t2、
    // t3 依赖 t4 → 成环拒绝
    EXPECT_FALSE(mgr.add_dependency(tn(1), tn(2)));
    EXPECT_FALSE(mgr.add_dependency(tn(3), tn(4)));

    // 被拒绝的边不得落库
    EXPECT_EQ(mgr.get_stats().edge_count, 4u);
    EXPECT_EQ(mgr.get_dependencies(tn(0)), std::vector<std::string>{});
    EXPECT_EQ(mgr.get_dependencies(tn(3)), (std::vector<std::string>{tn(2)}));

    // 传递冗余边（u ⇝ v 已成立的方向）必须接受、不得误判成环
    EXPECT_TRUE(mgr.add_dependency(tn(4), tn(0)));
    EXPECT_TRUE(mgr.add_dependency(tn(3), tn(0)));
    EXPECT_EQ(mgr.get_stats().edge_count, 6u);
    expect_contains(mgr.get_dependencies(tn(4)), tn(0));

    // 冗余边落库后闭合方向依旧拒绝
    EXPECT_FALSE(mgr.add_dependency(tn(0), tn(4)));
    EXPECT_EQ(mgr.get_stats().edge_count, 6u);
}

TEST(DependencyTopoCycleContract, DiamondDagAcceptsLegalEdges) {
    TaskDependencyManager mgr;
    // 菱形 DAG：a 依赖 b、c；b、c 依赖 d
    EXPECT_TRUE(mgr.add_dependency("a", "b"));
    EXPECT_TRUE(mgr.add_dependency("a", "c"));
    EXPECT_TRUE(mgr.add_dependency("b", "d"));
    EXPECT_TRUE(mgr.add_dependency("c", "d"));
    EXPECT_EQ(mgr.get_stats().edge_count, 4u);

    // 合法的多父汇合/分叉扩展（e、f 双父汇于 b、c）
    EXPECT_TRUE(mgr.add_dependency("e", "b"));
    EXPECT_TRUE(mgr.add_dependency("e", "c"));
    EXPECT_TRUE(mgr.add_dependency("f", "b"));
    EXPECT_TRUE(mgr.add_dependency("f", "c"));
    EXPECT_EQ(mgr.get_stats().edge_count, 8u);

    // is_ready 组合语义在 DAG 上保持：完成 d 后 b/c 就绪，a 需 b、c 全完成，
    // f 同样等 b、c
    EXPECT_FALSE(mgr.is_ready("a"));
    EXPECT_FALSE(mgr.is_ready("b"));
    mgr.mark_completed("d");
    EXPECT_TRUE(mgr.is_ready("b"));
    EXPECT_TRUE(mgr.is_ready("c"));
    EXPECT_FALSE(mgr.is_ready("a"));
    EXPECT_FALSE(mgr.is_ready("f"));
    mgr.mark_completed("b");
    EXPECT_FALSE(mgr.is_ready("a"));
    EXPECT_FALSE(mgr.is_ready("f"));
    mgr.mark_completed("c");
    EXPECT_TRUE(mgr.is_ready("a"));
    EXPECT_TRUE(mgr.is_ready("f"));

    // 菱形上的闭合边仍是环：d 依赖 a、b 依赖 a（a ⇝ d、a ⇝ b 已可达）
    EXPECT_FALSE(mgr.add_dependency("d", "a"));
    EXPECT_FALSE(mgr.add_dependency("b", "a"));
    // a 依赖 d 是传递冗余（a ⇝ d 已成立），必须接受
    EXPECT_TRUE(mgr.add_dependency("a", "d"));
    EXPECT_EQ(mgr.get_stats().edge_count, 9u);
}

TEST(DependencyTopoCycleContract, SelfDependencyRejected) {
    TaskDependencyManager mgr;
    EXPECT_FALSE(mgr.add_dependency("x", "x"));
    EXPECT_EQ(mgr.get_stats().task_count, 0u);
    EXPECT_EQ(mgr.get_stats().edge_count, 0u);

    // 已有其它边时自依赖仍拒绝，且不影响既有结构
    ASSERT_TRUE(mgr.add_dependency("x", "y"));
    EXPECT_FALSE(mgr.add_dependency("x", "x"));
    EXPECT_EQ(mgr.get_stats().edge_count, 1u);
}

TEST(DependencyTopoCycleContract, DuplicateEdgeReturnsTrueOnce) {
    TaskDependencyManager mgr;
    EXPECT_TRUE(mgr.add_dependency("a", "b"));
    // 重复边幂等返回 true，不重复落库
    EXPECT_TRUE(mgr.add_dependency("a", "b"));
    EXPECT_TRUE(mgr.add_dependency("a", "b"));
    EXPECT_EQ(mgr.get_stats().edge_count, 1u);
    EXPECT_EQ(mgr.get_dependencies("a"), (std::vector<std::string>{"b"}));

    // 重复边与成环边互不干扰：反向 b 依赖 a 仍是环
    EXPECT_FALSE(mgr.add_dependency("b", "a"));
    EXPECT_EQ(mgr.get_stats().edge_count, 1u);
    EXPECT_EQ(mgr.get_dependencies("a"), (std::vector<std::string>{"b"}));
}

TEST(DependencyTopoCycleContract, TwoNodeMutualDependencyRejected) {
    TaskDependencyManager mgr;
    EXPECT_TRUE(mgr.add_dependency("p", "q"));
    // 第二条互指边成环（p ⇝ q 已可达）
    EXPECT_FALSE(mgr.add_dependency("q", "p"));
    EXPECT_EQ(mgr.get_stats().edge_count, 1u);
    EXPECT_TRUE(mgr.get_dependencies("q").empty());

    // 拒绝后再补旁支边，结构仍健康
    EXPECT_TRUE(mgr.add_dependency("q", "r"));
    EXPECT_FALSE(mgr.add_dependency("r", "p"));  // p ⇝ q ⇝ r 可达
    EXPECT_EQ(mgr.get_stats().edge_count, 2u);
}

TEST(DependencyTopoCycleContract, EmptyIdsRejected) {
    TaskDependencyManager mgr;
    EXPECT_FALSE(mgr.add_dependency("", "a"));
    EXPECT_FALSE(mgr.add_dependency("a", ""));
    EXPECT_FALSE(mgr.add_dependency("", ""));
    EXPECT_EQ(mgr.get_stats().task_count, 0u);
}

// ---------------------------------------------------------------------------
// 2. 多父 / 汇合形态
// ---------------------------------------------------------------------------

TEST(DependencyTopoJoinForms, DiamondJoinReverseEdgeRejected) {
    TaskDependencyManager mgr;
    // 汇合：sink 依赖 m1、m2；m1、m2 依赖 src（sink ⇝ m1 ⇝ src）
    ASSERT_TRUE(mgr.add_dependency("sink", "m1"));
    ASSERT_TRUE(mgr.add_dependency("sink", "m2"));
    ASSERT_TRUE(mgr.add_dependency("m1", "src"));
    ASSERT_TRUE(mgr.add_dependency("m2", "src"));
    ASSERT_EQ(mgr.get_stats().edge_count, 4u);

    // 汇点反向加边：src 依赖 sink → 成环拒绝
    EXPECT_FALSE(mgr.add_dependency("src", "sink"));
    // 中间节点到汇点同样闭合
    EXPECT_FALSE(mgr.add_dependency("m1", "sink"));
    EXPECT_FALSE(mgr.add_dependency("m2", "sink"));
    // 传递冗余边接受：sink 依赖 src（经 m1/m2 的传递关系已成立）
    EXPECT_TRUE(mgr.add_dependency("sink", "src"));
    EXPECT_EQ(mgr.get_stats().edge_count, 5u);

    // 汇合后再入新源（无环旁支）合法，且与旧结构组合仍守约
    ASSERT_TRUE(mgr.add_dependency("sink", "src2"));
    EXPECT_FALSE(mgr.add_dependency("src2", "sink"));  // sink ⇝ src2 可达
    EXPECT_TRUE(mgr.add_dependency("src", "src2"));    // src2 ⇝ src 不成立
    EXPECT_FALSE(mgr.add_dependency("src2", "src"));   // src ⇝ src2 可达
    EXPECT_EQ(mgr.get_stats().edge_count, 7u);
}

TEST(DependencyTopoJoinForms, MultiParentThenCrossChainStillAcyclic) {
    TaskDependencyManager mgr;
    // j 依赖 p1、p2；p1 依赖 g1；p2 依赖 g2；k 依赖 j
    ASSERT_TRUE(mgr.add_dependency("j", "p1"));
    ASSERT_TRUE(mgr.add_dependency("j", "p2"));
    ASSERT_TRUE(mgr.add_dependency("p1", "g1"));
    ASSERT_TRUE(mgr.add_dependency("p2", "g2"));
    ASSERT_TRUE(mgr.add_dependency("k", "j"));
    ASSERT_EQ(mgr.get_stats().edge_count, 5u);

    // 跨链闭合边逐级拒绝（k ⇝ j ⇝ p1 ⇝ g1 可达）
    EXPECT_FALSE(mgr.add_dependency("g1", "k"));
    EXPECT_FALSE(mgr.add_dependency("g2", "k"));
    EXPECT_FALSE(mgr.add_dependency("p1", "k"));
    // 无可达关系的跨链边合法
    EXPECT_TRUE(mgr.add_dependency("g1", "g2"));
    EXPECT_FALSE(mgr.add_dependency("g2", "g1"));  // 加完反向即闭合
    EXPECT_EQ(mgr.get_stats().edge_count, 6u);
}

// ---------------------------------------------------------------------------
// 3. remove / prune 之后继续正确
// ---------------------------------------------------------------------------

TEST(DependencyTopoRemovePrune, RemoveMiddleEdgeThenReAddConsistent) {
    TaskDependencyManager mgr;
    // 链 a_i 依赖 a_{i-1}（a4 ⇝ a3 ⇝ a2 ⇝ a1 ⇝ a0）
    for (int i = 1; i <= 4; ++i) {
        ASSERT_TRUE(mgr.add_dependency("a" + std::to_string(i),
                                       "a" + std::to_string(i - 1)));
    }
    ASSERT_EQ(mgr.get_stats().edge_count, 4u);

    // 摘除中段边 a2→a1
    ASSERT_TRUE(mgr.remove_dependency("a2", "a1"));
    EXPECT_FALSE(mgr.remove_dependency("a2", "a1"));  // 重复删除 no-op
    EXPECT_EQ(mgr.get_stats().edge_count, 3u);

    // 行为随新图翻转：原方向（a2 依赖 a1）现在是合法加边，
    // 而原先合法的 a1 依赖 a2 现在成环
    EXPECT_TRUE(mgr.add_dependency("a1", "a2"));
    EXPECT_FALSE(mgr.add_dependency("a2", "a1"));

    // 摘除翻转边、加回原方向，整链恢复
    ASSERT_TRUE(mgr.remove_dependency("a1", "a2"));
    ASSERT_TRUE(mgr.add_dependency("a2", "a1"));
    EXPECT_EQ(mgr.get_stats().edge_count, 4u);

    // 恢复后闭合检测恢复：首尾 / 中间跨级
    EXPECT_FALSE(mgr.add_dependency("a0", "a4"));
    EXPECT_FALSE(mgr.add_dependency("a1", "a4"));
    EXPECT_FALSE(mgr.add_dependency("a0", "a3"));
    // 冗余方向仍接受
    EXPECT_TRUE(mgr.add_dependency("a4", "a0"));
    EXPECT_EQ(mgr.get_stats().edge_count, 5u);
}

TEST(DependencyTopoRemovePrune, RemoveHeadEdgeThenOppositeAccepted) {
    TaskDependencyManager mgr;
    // 链 b_i 依赖 b_{i-1}，摘除头部边 b1→b0
    ASSERT_TRUE(mgr.add_dependency("b1", "b0"));
    ASSERT_TRUE(mgr.add_dependency("b2", "b1"));
    ASSERT_TRUE(mgr.add_dependency("b3", "b2"));
    ASSERT_TRUE(mgr.remove_dependency("b1", "b0"));
    EXPECT_EQ(mgr.get_stats().edge_count, 2u);

    // b1 与 b0 断开后：b0 依赖 b1 合法，b1 依赖 b0 恢复合法前被拒
    EXPECT_TRUE(mgr.add_dependency("b0", "b1"));
    EXPECT_FALSE(mgr.add_dependency("b1", "b0"));  // b0 ⇝ b1（直连）成立
    ASSERT_TRUE(mgr.remove_dependency("b0", "b1"));
    EXPECT_TRUE(mgr.add_dependency("b1", "b0"));   // 恢复原方向
    EXPECT_EQ(mgr.get_stats().edge_count, 3u);

    // 整链闭合检测恢复
    EXPECT_FALSE(mgr.add_dependency("b0", "b3"));
    EXPECT_FALSE(mgr.add_dependency("b0", "b2"));
    EXPECT_TRUE(mgr.add_dependency("b3", "b0"));  // 冗余方向
    EXPECT_EQ(mgr.get_stats().edge_count, 4u);
}

TEST(DependencyTopoRemovePrune, PruneMiddleNodeThenReregisterSameName) {
    TaskDependencyManager mgr;
    // 链 t_i 依赖 t_{i-1}，中间节点 t1（被 t2 依赖、依赖 t0）
    for (int i = 1; i <= 3; ++i) {
        ASSERT_TRUE(mgr.add_dependency(tn(i), tn(i - 1)));
    }

    // 契约式剪除 t1：先移除唯一入边 t2→t1，使 t1 无依赖者，再 prune
    ASSERT_TRUE(mgr.remove_dependency(tn(2), tn(1)));
    EXPECT_EQ(mgr.prune(tn(1)), 1u);  // t1 的出边 t1→t0
    EXPECT_EQ(mgr.get_stats().edge_count, 1u);
    EXPECT_FALSE(mgr.is_completed(tn(1)));

    // 同名重新注册：重建 t1→t0、t2→t1
    EXPECT_TRUE(mgr.add_dependency(tn(1), tn(0)));
    EXPECT_TRUE(mgr.add_dependency(tn(2), tn(1)));
    EXPECT_EQ(mgr.get_stats().edge_count, 3u);

    // 重建后的环检测仍正确：闭合边拒绝、冗余边接受
    EXPECT_FALSE(mgr.add_dependency(tn(0), tn(3)));
    EXPECT_FALSE(mgr.add_dependency(tn(1), tn(3)));
    EXPECT_TRUE(mgr.add_dependency(tn(3), tn(0)));
    EXPECT_EQ(mgr.get_stats().edge_count, 4u);

    // 重新注册的节点参与新的合法扩展
    EXPECT_TRUE(mgr.add_dependency(tn(9), tn(1)));
    EXPECT_FALSE(mgr.add_dependency(tn(1), tn(9)));  // t9 ⇝ t1 直连成立
    EXPECT_EQ(mgr.get_stats().edge_count, 5u);
}

TEST(DependencyTopoRemovePrune, PruneDependentEndThenReregister) {
    TaskDependencyManager mgr;
    // 链 x0 依赖 x1 依赖 x2（x0 无依赖者，可契约式 prune）
    ASSERT_TRUE(mgr.add_dependency("x0", "x1"));
    ASSERT_TRUE(mgr.add_dependency("x1", "x2"));
    mgr.mark_completed("x0");
    EXPECT_TRUE(mgr.is_completed("x0"));

    EXPECT_EQ(mgr.prune("x0"), 2u);  // 出边 x0→x1 + completed 条目
    EXPECT_EQ(mgr.get_stats().edge_count, 1u);
    EXPECT_FALSE(mgr.is_completed("x0"));

    // 同名重注册并重建边，完成语义与环检测恢复
    EXPECT_TRUE(mgr.add_dependency("x0", "x1"));
    EXPECT_EQ(mgr.get_stats().edge_count, 2u);
    EXPECT_FALSE(mgr.is_ready("x0"));
    mgr.mark_completed("x1");
    EXPECT_TRUE(mgr.is_ready("x0"));
    mgr.mark_completed("x2");
    EXPECT_TRUE(mgr.is_ready("x1"));

    // 闭合边拒绝、冗余边接受
    EXPECT_FALSE(mgr.add_dependency("x2", "x0"));  // x0 ⇝ x2 可达
    EXPECT_FALSE(mgr.add_dependency("x1", "x0"));  // x0 ⇝ x1 直连成立
    EXPECT_TRUE(mgr.add_dependency("x0", "x2"));   // 冗余
    EXPECT_EQ(mgr.get_stats().edge_count, 3u);
}

TEST(DependencyTopoRemovePrune, ClearResetsTopologyFully) {
    TaskDependencyManager mgr;
    ASSERT_TRUE(mgr.add_dependency("c0", "c1"));
    ASSERT_TRUE(mgr.add_dependency("c1", "c2"));
    mgr.mark_completed("c2");
    ASSERT_FALSE(mgr.add_dependency("c2", "c0"));  // c0 ⇝ c2 可达

    mgr.clear();
    EXPECT_EQ(mgr.get_stats().task_count, 0u);
    EXPECT_EQ(mgr.get_stats().edge_count, 0u);
    EXPECT_EQ(mgr.get_stats().completed_count, 0u);

    // clear 后同名列全量重建，此前的序号状态不得残留
    EXPECT_TRUE(mgr.add_dependency("c2", "c0"));
    EXPECT_TRUE(mgr.add_dependency("c0", "c1"));
    EXPECT_FALSE(mgr.add_dependency("c1", "c2"));  // c2 ⇝ c1 可达

    // clear 同时回收完成状态；重建后完成语义重新按新图计算
    EXPECT_FALSE(mgr.is_completed("c2"));

    // 完成语义在新图上成立：c1 无依赖，c2 只等 c0
    EXPECT_TRUE(mgr.is_ready("c1"));
    mgr.mark_completed("c0");
    EXPECT_TRUE(mgr.is_ready("c2"));
    EXPECT_FALSE(mgr.is_ready("c0"));  // c0 等 c1（未完成）
    mgr.mark_completed("c1");
    EXPECT_TRUE(mgr.is_ready("c0"));
}

// ---------------------------------------------------------------------------
// 4. 规模回归（迁移 review_verification/cr052_dependency_dfs.cpp scale 模式）
// ---------------------------------------------------------------------------

TEST(DependencyTopoScale, ForwardChain20000BuildsUnder1000ms) {
    TaskDependencyManager mgr;
    const int n = 20000;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= n; ++i) {
        ASSERT_TRUE(mgr.add_dependency(tn(i), tn(i - 1)));
    }
    const double ms = elapsed_ms_since(t0);
    std::printf("forward chain n=%d build: %.1f ms\n", n, ms);

    // CR-052 验收线：正序链建链必须远低于 master 全图 DFS 的 ~1s+
    EXPECT_LT(ms, 1000.0);

    // 结构断言：边数 = n，t0 无依赖、t1 等待 t0
    ASSERT_EQ(mgr.get_stats().edge_count, static_cast<size_t>(n));
    EXPECT_TRUE(mgr.is_ready(tn(0)));
    EXPECT_FALSE(mgr.is_ready(tn(1)));

    // closing edge（链是 t_i 依赖 t_{i-1}，闭合方向是浅端依赖深端）
    EXPECT_FALSE(mgr.add_dependency(tn(0), tn(n)))
        << "closing edge t0->t20000 must be rejected";
    EXPECT_FALSE(mgr.add_dependency(tn(1), tn(n)))
        << "closing edge t1->t20000 must be rejected";
    EXPECT_FALSE(mgr.add_dependency(tn(2), tn(n)))
        << "closing edge t2->t20000 must be rejected";
    EXPECT_EQ(mgr.get_stats().edge_count, static_cast<size_t>(n));
    EXPECT_EQ(mgr.get_dependencies(tn(1)), (std::vector<std::string>{tn(0)}));

    // 冗余方向接受：t20000 依赖 t0（传递已成立），不误报环
    EXPECT_TRUE(mgr.add_dependency(tn(n), tn(0)));
    EXPECT_EQ(mgr.get_stats().edge_count, static_cast<size_t>(n) + 1);
    {
        const auto deps = mgr.get_dependencies(tn(n));
        ASSERT_EQ(deps.size(), 2u);
        expect_contains(deps, tn(n - 1));
        expect_contains(deps, tn(0));
    }
}

TEST(DependencyTopoScale, ReverseInsertionChain2000NoFalseCycles) {
    TaskDependencyManager mgr;
    // 逆序建链（已知 O(n^2) 重排边界，只验正确性不验耗时）：从深端往浅端
    // 插入，每次插入触发受限重排，必须全部接受且无误报环。
    const int n = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = n; i >= 1; --i) {
        ASSERT_TRUE(mgr.add_dependency(tn(i), tn(i - 1)))
            << "reverse chain edge t" << i << "->t" << (i - 1)
            << " wrongly rejected (false cycle)";
    }
    const double ms = elapsed_ms_since(t0);
    std::printf("reverse chain n=%d build: %.1f ms (accepted, known quadratic boundary)\n",
                n, ms);

    EXPECT_EQ(mgr.get_stats().edge_count, static_cast<size_t>(n));
    EXPECT_FALSE(mgr.add_dependency(tn(0), tn(n)));
    EXPECT_TRUE(mgr.add_dependency(tn(n), tn(0)));
    EXPECT_EQ(mgr.get_stats().edge_count, static_cast<size_t>(n) + 1);
}

// ---------------------------------------------------------------------------
// 5. 随机对拍（固定种子，与内联暴力 DFS 全一致）
// ---------------------------------------------------------------------------

TEST(DependencyTopoDifferential, RandomOpsMatchBruteDfs) {
    // 迁移 /tmp/cr052_min.cpp 的对拍逻辑：40 节点、50 trial × 200 op，
    // mt19937 固定种子，逐 op 与暴力 DFS 环检测比对返回值。
    constexpr int kNodes = 40;
    constexpr int kTrials = 50;
    constexpr int kOpsPerTrial = 200;
    constexpr uint32_t kSeed = 20261010u;

    std::mt19937 rng(kSeed);
    auto name = [](int i) { return "n" + std::to_string(i); };

    for (int trial = 0; trial < kTrials; ++trial) {
        TaskDependencyManager mgr;
        std::vector<std::set<int>> adj(kNodes);
        size_t accepted = 0;

        for (int op = 0; op < kOpsPerTrial; ++op) {
            int u = static_cast<int>(rng() % kNodes);
            int v = static_cast<int>(rng() % kNodes);
            if (u == v) {
                continue;
            }
            const bool expected = !brute_cycle(adj, u, v);
            const bool got = mgr.add_dependency(name(u), name(v));
            ASSERT_EQ(got, expected)
                << "trial=" << trial << " op=" << op << " add(n" << u
                << ", n" << v << "): brute=" << expected << " impl=" << got;
            if (got) {
                if (adj[u].insert(v).second) {
                    ++accepted;  // 重复边返回 true 但不重复落库
                }
            }
        }

        // 每个 trial 结束做一次结构一致性核对：接受的边都应落库
        size_t impl_edges = 0;
        for (int i = 0; i < kNodes; ++i) {
            const auto deps = mgr.get_dependencies(name(i));
            ASSERT_EQ(deps.size(), adj[i].size())
                << "trial=" << trial << " node n" << i
                << " edge count mismatch";
            for (int d : adj[i]) {
                ASSERT_NE(std::find(deps.begin(), deps.end(), name(d)),
                          deps.end())
                    << "trial=" << trial << " node n" << i
                    << " missing edge to n" << d;
            }
            impl_edges += deps.size();
        }
        EXPECT_EQ(impl_edges, accepted);
    }
}

TEST(DependencyTopoDifferential, RandomOpsWithRemovalMatchBruteDfs) {
    // 对拍加强：在 add 之外混入 remove_dependency，暴力侧同步删边，
    // 验证反向边表/序号在删除后的维护与环判定仍一致。
    constexpr int kNodes = 24;
    constexpr int kTrials = 30;
    constexpr int kOpsPerTrial = 240;
    constexpr uint32_t kSeed = 5212026u;

    std::mt19937 rng(kSeed);
    auto name = [](int i) { return "m" + std::to_string(i); };

    for (int trial = 0; trial < kTrials; ++trial) {
        TaskDependencyManager mgr;
        std::vector<std::set<int>> adj(kNodes);

        for (int op = 0; op < kOpsPerTrial; ++op) {
            int u = static_cast<int>(rng() % kNodes);
            int v = static_cast<int>(rng() % kNodes);
            if (u == v) {
                continue;
            }
            if (rng() % 4 == 0) {
                // 删除操作：存在该边则两侧同步移除
                const bool had = adj[u].count(v) > 0;
                EXPECT_EQ(mgr.remove_dependency(name(u), name(v)), had)
                    << "trial=" << trial << " op=" << op << " remove(n" << u
                    << ", n" << v << ")";
                adj[u].erase(v);
            } else {
                const bool expected = !brute_cycle(adj, u, v);
                const bool got = mgr.add_dependency(name(u), name(v));
                ASSERT_EQ(got, expected)
                    << "trial=" << trial << " op=" << op << " add(n" << u
                    << ", n" << v << "): brute=" << expected
                    << " impl=" << got;
                if (got) {
                    adj[u].insert(v);
                }
            }
        }

        // 结构核对：实现侧与暴力侧逐节点边数一致
        for (int i = 0; i < kNodes; ++i) {
            const auto deps = mgr.get_dependencies(name(i));
            ASSERT_EQ(deps.size(), adj[i].size())
                << "trial=" << trial << " node m" << i
                << " edge count mismatch after mixed ops";
            for (int d : adj[i]) {
                ASSERT_NE(std::find(deps.begin(), deps.end(), name(d)),
                          deps.end())
                    << "trial=" << trial << " node m" << i
                    << " missing edge to m" << d;
            }
        }
    }
}
