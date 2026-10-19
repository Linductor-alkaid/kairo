#!/usr/bin/env bash
# migrate_submit_priority.sh —— 0.7.2 submit_priority → submit_auto(TaskBuilder) 迁移助手
#
# 背景（roadmap §4.0 / docs/design/scheduling_runtime.md §14）：
#   优先级是调度策略维度而非提交协议。0.7.2 起 Executor::submit_priority
#   标记 [[deprecated]]（正式版移除），统一经
#       submit_auto(kairo::TaskBuilder(fn).priority(kairo::TaskPriority::P))
#   表达——与 QoS/deadline 共享同一 IScheduler 协议，调度顺序
#   （priority → EDF → FIFO）与 0.7.1 逐位一致。
#
# 自动处理：
#   submit_priority(P, CALLABLE)        → submit_auto(kairo::TaskBuilder(CALLABLE).priority(P'))
#   obj.submit_priority(P, CALLABLE)    → obj.submit_auto(kairo::TaskBuilder(CALLABLE).priority(P'))
#   其中 P 为 0/1/2/3 时映射为 TaskPriority::LOW/NORMAL/HIGH/CRITICAL，
#   其他表达式映射为 static_cast<kairo::TaskPriority>(P)。
#
# 不自动处理（保留原样并逐条报告，需人工迁移）：
#   - 带额外绑定参数的调用 submit_priority(P, fn, args...)：
#     TaskBuilder 只承载单个 callable，请把参数捕获进 lambda 或用 std::bind；
#   - 内部层 ThreadPool::submit_priority / try_submit_priority*（不属于
#     facade 弃用范围，脚本按命名边界自动跳过）。
#
# 局限：括号配平不感知字符串/注释字面量；多行调用可以处理，但宏展开出的
# 调用请人工确认。迁移后请重新编译并核对 --dry-run 输出。
#
# 用法：
#   scripts/migrate_submit_priority.sh [--dry-run] FILE|DIR...
# 退出码：0 = 全部处理且无遗留；1 = 存在需人工处理的遗留调用；2 = 用法错误。

set -uo pipefail

dry_run=0
if [[ "${1:-}" == "--dry-run" ]]; then
    dry_run=1
    shift
fi

if [[ $# -eq 0 ]]; then
    echo "usage: $0 [--dry-run] FILE|DIR..." >&2
    exit 2
fi

declare -a files=()
for target in "$@"; do
    if [[ -d "$target" ]]; then
        while IFS= read -r f; do files+=("$f"); done < <(
            find "$target" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \
                                    -o -name '*.hpp' -o -name '*.h' \) | sort)
    elif [[ -f "$target" ]]; then
        files+=("$target")
    else
        echo "skip: $target 不存在" >&2
    fi
done

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no input files" >&2
    exit 2
fi

transform() {
    perl -0777 -e '
        sub prio_expr {
            my ($p) = @_;
            $p =~ s/^\s+|\s+$//g;
            return "kairo::TaskPriority::LOW"     if $p eq "0";
            return "kairo::TaskPriority::NORMAL"  if $p eq "1";
            return "kairo::TaskPriority::HIGH"    if $p eq "2";
            return "kairo::TaskPriority::CRITICAL" if $p eq "3";
            return "static_cast<kairo::TaskPriority>($p)";
        }
        # 从 $pos（指向 "("）起做括号配平，返回闭括号位置，失败返回 -1。
        sub match_paren {
            my ($src, $pos) = @_;
            my $depth = 0;
            for (my $i = $pos; $i < length($src); ++$i) {
                my $c = substr($src, $i, 1);
                $depth++ if $c eq "(";
                if ($c eq ")") {
                    $depth--;
                    return $i if $depth == 0;
                }
            }
            return -1;
        }
        # 拆开参数串的顶层逗号。
        sub split_top_args {
            my ($s) = @_;
            my (@args, $depth, $cur);
            ($depth, $cur) = (0, "");
            for my $i (0 .. length($s) - 1) {
                my $c = substr($s, $i, 1);
                $depth++ if $c eq "(" || $c eq "{" || $c eq "[";
                $depth-- if $c eq ")" || $c eq "}" || $c eq "]";
                if ($c eq "," && $depth == 0) {
                    push @args, $cur; $cur = "";
                } else {
                    $cur .= $c;
                }
            }
            push @args, $cur;
            return @args;
        }
        my $src = <>;
        my $out = "";
        my $copied = 0;
        while ($src =~ m/(?<![\w:])((?:\w+[\w>:.*]*\.)?)submit_priority\s*\(/g) {
            my $head_start = $-[0];
            my $open = $+[0] - 1;
            my $close = match_paren($src, $open);
            last if $close < 0;                 # 配平失败：余下原样保留
            my $args_str = substr($src, $open + 1, $close - $open - 1);
            my @args = split_top_args($args_str);
            if (@args == 2) {
                my $receiver = $1 // "";
                my $prio = prio_expr($args[0]);
                my $fn = $args[1];
                $out .= substr($src, $copied, $head_start - $copied)
                      . "${receiver}submit_auto(kairo::TaskBuilder($fn).priority($prio))";
                $copied = $close + 1;
            }
            # 参数数不符时跳过本次匹配、原样保留，继续向后扫描。
        }
        $out .= substr($src, $copied);
        print $out;
    ' < "$1"
}

rc_total=0
for f in "${files[@]}"; do
    before=$(grep -cE '(^|[^[:alnum:]_:])submit_priority[[:space:]]*\(' "$f" || true)
    [[ "$before" -eq 0 ]] && continue

    echo "== $f: $before 处候选 =="
    if [[ "$dry_run" -eq 1 ]]; then
        transform "$f" | diff -u "$f" - || true
    else
        tmp="$(mktemp)"
        transform "$f" > "$tmp" && mv "$tmp" "$f"
    fi

    # 遗留报告：extra-args / 配平失败等未改写的 facade 调用。
    left=$(grep -nE '(^|[^[:alnum:]_:])submit_priority[[:space:]]*\(' "$f" || true)
    if [[ -n "$left" ]]; then
        rc_total=1
        echo "  需人工迁移（额外绑定参数或无法配平）："
        echo "$left" | sed 's/^/    /'
    fi
done

if [[ "$dry_run" -eq 1 ]]; then
    echo "(--dry-run：未写回任何文件)"
fi
exit "$rc_total"
