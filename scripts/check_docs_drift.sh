#!/usr/bin/env bash
# 文档漂移扫描（0.6.1 docs governance，docs/design/scheduling_runtime.md §1）。
#
# 目标：把 0.6.0 大规模重命名后已证明有效的检查固化到 CI，防止旧命名、
# 废弃 API 与失效相对链接再次进入"当前规范"层文档。
#
# 检查项：
#   1. 旧命名空间 executor:: / 旧 include <executor/...>；
#   2. 旧 CMake 用法 find_package(executor) / executor::executor / EXECUTOR_* 宏；
#   3. 0.6.0 已删除的 API 符号（_ex 系列、with_handle 系列、cancel_task、
#      Executor::instance、HardRealtime 等）；
#   4. Markdown 相对链接指向不存在的文件。
#
# 豁免：docs/archive/ 整体、带"历史设计快照"横幅的文档、docs/MIGRATION.md
# （迁移指南的职责就是记录旧名称）。代码目录（include/ src/ tests/
# examples/）不豁免——旧命名在代码里是编译错误。
#
# 用法：scripts/check_docs_drift.sh
# 退出码：0 = 无漂移；1 = 发现漂移（输出逐条定位）。

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

failures=0
report() {
    echo "DRIFT: $1"
    failures=$((failures + 1))
}

# 判断文件是否豁免旧命名检查（历史材料允许保留旧名称，但不得出现在
# 当前规范文档中）。
is_exempt() {
    local file="$1"
    case "$file" in
        docs/archive/*|docs/MIGRATION.md) return 0 ;;
        # 版本迁移页面的职责就是记录旧名称（website 版本对照页）。
        website/*/reference/version-and-migration.md) return 0 ;;
    esac
    # 历史快照横幅：docs/design/ 下多个文档带此标记。
    grep -q '历史设计快照' "$file" 2>/dev/null && return 0
    return 1
}

echo "== docs drift scan (0.6.1 governance) =="

# ---------------------------------------------------------------- 代码目录
# 旧命名在代码里是硬错误，无条件扫描。
echo "-- scanning code trees for stale naming --"
while IFS= read -r file; do
    while IFS=: read -r line_no _match; do
        report "$file:$line_no: stale 'executor::' namespace or '<executor/' include in code"
    done < <(grep -nE 'executor::|<executor/|"executor/' "$file" || true)
done < <(find include src tests examples -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' -o -name '*.ipp' -o -name 'CMakeLists.txt' -o -name '*.cmake' -o -name '*.ps1' -o -name '*.sh' \) 2>/dev/null)

# ---------------------------------------------------------------- 文档目录
echo "-- scanning current-layer docs for stale naming / dropped symbols --"
doc_files=$(find docs README.md -type f -name '*.md' 2>/dev/null; \
            find website/en website/zh -type f -name '*.md' 2>/dev/null)

for file in $doc_files; do
    if is_exempt "$file"; then
        continue
    fi
    # 1. 旧命名空间 / 旧 include。
    while IFS=: read -r line_no _match; do
        report "$file:$line_no: stale 'executor::' / '<executor/' in current-layer doc"
    done < <(grep -nE 'executor::|<executor/' "$file" || true)

    # 2. 旧 CMake 用法（website 快速上手等代码片段常见）。
    while IFS=: read -r line_no _match; do
        report "$file:$line_no: stale find_package(executor) / executor::executor / EXECUTOR_* macro"
    done < <(grep -nE 'find_package\(executor\)|executor::executor|EXECUTOR_ENABLE|EXECUTOR_BUILD' "$file" || true)

    # 3. 0.6.0 已删除的 API（迁移指南之外不得再出现）。注意
    #    Executor::instance() 仍是现行 API（单例），不在删除清单；
    #    行内显式说明"已移除/接管主名"的删除注记不算漂移。
    while IFS=: read -r line_no match; do
        report "$file:$line_no: dropped API symbol '$match'"
    done < <(grep -nE 'HardRealtime|AffinityHint::exclusive|initialize_ex|wait_for_completion_ex|submit_delayed_with_handle|submit_periodic_with_handle|submit_delayed_cancellable_with_handle|submit_periodic_cancellable_with_handle|\bcancel_task\(' "$file" \
             | grep -vE '已移除|已删除|接管主名|removed|take over|took over' || true)
done

# ---------------------------------------------------------------- 相对链接
# 链接检查只覆盖当前文档层；docs/archive/ 的历史材料内部链接不检查
#（其历史属性已由目录位置声明，修复价值低于噪声）。
echo "-- scanning markdown relative links --"
for file in $doc_files; do
    case "$file" in
        docs/archive/*) continue ;;
    esac
    dir="$(dirname "$file")"
    # 匹配 ](path)；跳过 http(s)/mailto/锚点。含空白的"目标"是 C++ lambda
    # 捕获列表（如 [](void* stream)），不是 markdown 链接。
    while IFS= read -r link; do
        target="${link%%#*}"
        [ -z "$target" ] && continue
        case "$target" in
            *[[:space:]]*) continue ;;
        esac
        case "$target" in
            /*)
                # 站点根相对路径：website/ 下按 VitePress srcDir（website/）
                # 解析，其余按仓库根解析。
                case "$file" in
                    website/*) base="website" ;;
                    *) base="." ;;
                esac
                if [ -e "$base$target" ] || [ -e "$base$target.md" ] || [ -e "$base${target%/}" ] || [ -e "$base${target%/}.md" ]; then
                    continue
                fi
                ;;
            *)
                if [ -e "$dir/$target" ]; then
                    continue
                fi
                ;;
        esac
        report "$file: broken relative link '$link'"
    done < <(grep -oE '\]\([^)#]+(#[^)]*)?\)' "$file" 2>/dev/null \
             | sed -E 's/^\]\(//; s/\)$//' \
             | grep -vE '^(https?:|mailto:|#)' || true)
done

echo "== scan complete: $failures finding(s) =="
if [ "$failures" -ne 0 ]; then
    echo "FAIL: documentation drift detected (see above)."
    exit 1
fi
echo "OK: no documentation drift detected."
