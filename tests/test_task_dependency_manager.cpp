#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <kairo/task/task_dependency_manager.hpp>

using kairo::TaskDependencyManager;

#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { std::cerr << "FAIL: " << msg << " at " << __FILE__ << ":" << __LINE__ << std::endl; return 1; } } while(0)

// P-260623-002: prune() must release memory so long-running services don't grow unbounded.
int test_prune_releases_memory() {
    std::cout << "[P-260623-002] test_prune_releases_memory..." << std::endl;
    TaskDependencyManager mgr;
    const size_t N = 1000;

    for (size_t i = 0; i < N; ++i) {
        std::string task = "task_" + std::to_string(i);
        std::string dep = "dep_" + std::to_string(i);
        ASSERT_TRUE(mgr.add_dependency(task, dep), "add_dependency should succeed");
    }
    auto s1 = mgr.get_stats();
    ASSERT_TRUE(s1.task_count == N, "should have N tasks in dependencies_");
    ASSERT_TRUE(s1.edge_count == N, "should have N edges");

    for (size_t i = 0; i < N; ++i) {
        mgr.mark_completed("dep_" + std::to_string(i));
    }
    for (size_t i = 0; i < N; ++i) {
        mgr.prune("dep_" + std::to_string(i));
    }
    auto s2 = mgr.get_stats();
    ASSERT_TRUE(s2.completed_count == 0, "completed_tasks_ should be empty after pruning all deps");

    for (size_t i = 0; i < N; ++i) {
        mgr.prune("task_" + std::to_string(i));
    }
    auto s3 = mgr.get_stats();
    ASSERT_TRUE(s3.task_count == 0, "dependencies_ should be empty after pruning all tasks");
    ASSERT_TRUE(s3.completed_count == 0, "completed_tasks_ should still be empty");
    ASSERT_TRUE(s3.edge_count == 0, "no edges should remain");

    std::cout << "  ok (initial=" << s1.task_count << " edges=" << s1.edge_count
              << ", after-deps-prune=" << s2.task_count << ", after-all-prune="
              << s3.task_count << ")" << std::endl;
    return 0;
}

int test_remove_dependency_clears_empty_tasks() {
    std::cout << "[P-260623-002] test_remove_dependency_clears_empty_tasks..." << std::endl;
    TaskDependencyManager mgr;

    ASSERT_TRUE(mgr.add_dependency("A", "B"), "A->B should succeed");
    ASSERT_TRUE(mgr.add_dependency("A", "C"), "A->C should succeed");
    ASSERT_TRUE(mgr.get_stats().task_count == 1, "A should be the only task");
    ASSERT_TRUE(mgr.get_stats().edge_count == 2, "A should have 2 edges");

    ASSERT_TRUE(mgr.remove_dependency("A", "B"), "removing A->B should succeed");
    ASSERT_TRUE(mgr.get_stats().edge_count == 1, "1 edge should remain");
    ASSERT_TRUE(mgr.get_stats().task_count == 1, "A still present (1 edge left)");

    ASSERT_TRUE(mgr.remove_dependency("A", "C"), "removing A->C should succeed");
    ASSERT_TRUE(mgr.get_stats().edge_count == 0, "no edges should remain");
    ASSERT_TRUE(mgr.get_stats().task_count == 0, "A entry should be removed when last edge gone");

    ASSERT_TRUE(!mgr.remove_dependency("A", "B"), "removing non-existent A->B should return false");
    ASSERT_TRUE(!mgr.remove_dependency("X", "Y"), "removing non-existent X->Y should return false");
    ASSERT_TRUE(!mgr.remove_dependency("", "B"), "empty task_id should return false");
    ASSERT_TRUE(!mgr.remove_dependency("A", ""), "empty depends_on should return false");

    std::cout << "  ok" << std::endl;
    return 0;
}

int test_prune_safe_on_empty_and_missing() {
    std::cout << "[P-260623-002] test_prune_safe_on_empty_and_missing..." << std::endl;
    TaskDependencyManager mgr;
    ASSERT_TRUE(mgr.prune("") == 0, "prune(\"\") should return 0");
    ASSERT_TRUE(mgr.prune("never_added") == 0, "prune of missing task should return 0");
    ASSERT_TRUE(mgr.add_dependency("A", "B"), "A->B should succeed");
    ASSERT_TRUE(mgr.prune("B") == 0, "prune of B (which is depended on but has no entry) should be 0");
    ASSERT_TRUE(mgr.is_ready("A") == false, "A still depends on B (B not completed)");

    mgr.mark_completed("B");
    ASSERT_TRUE(mgr.is_ready("A") == true, "A should be ready now that B is completed");
    ASSERT_TRUE(mgr.prune("B") == 1, "prune of completed B should remove 1 entry (completed_tasks_)");
    ASSERT_TRUE(mgr.is_completed("B") == false, "B should no longer be marked completed after prune");
    std::cout << "  ok (note: pruning a task that active tasks still depend on is caller error)" << std::endl;
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_prune_releases_memory();
    rc |= test_remove_dependency_clears_empty_tasks();
    rc |= test_prune_safe_on_empty_and_missing();
    if (rc == 0) {
        std::cout << "\n=== All TaskDependencyManager P-260623-002 tests PASSED ===" << std::endl;
    } else {
        std::cout << "\n=== Some tests FAILED ===" << std::endl;
    }
    return rc;
}
