#!/usr/bin/env python3
"""生成未跟踪文件盘点清单。

工具只读取 Git 状态和文件元数据，不删除、不移动被盘点文件。除用户显式
指定的输出文件及其必要父目录外，不写入其他位置。
"""

from __future__ import annotations

import argparse
import heapq
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Callable

TOP_GLOBAL = 25
TOP_INACCESSIBLE = 25

CATEGORY_META = {
    "firmware_backups": (
        "固件二进制/ELF 备份",
        "保留关键 SHA 对应版本；确认 Git/备份盘已有副本后再归档，不建议直接删除。",
    ),
    "partial_reports": (
        "夜间阶段性分析产物",
        "若交接文档已吸收结论，可归档；否则先保留。",
    ),
    "patch_artifacts": (
        "补丁文件备份",
        "已合入的补丁可归档；清理前与最终代码抽查比对。",
    ),
    "source_snapshots": (
        "源码快照",
        "仅在确认 Git 历史包含对应版本后归档。",
    ),
    "temporary_backup_dirs": (
        "临时备份文件/目录",
        "需要用户确认；优先归档而不是直接删除。",
    ),
    "build_output_baud_460800": (
        "460800 目录中的构建产物",
        "确认无唯一证据后可清理，并补充忽略规则。",
    ),
    "serial_logs": (
        "串口实测日志",
        "保留关键短日志和异常节点日志；重复/巨型日志建议压缩归档，不直接删。",
    ),
    "experimental_node_combined": (
        "node_combined 实验工程",
        "先判断是否为当前组合节点路线；需要用户确认后再收敛或归档。",
    ),
    "experimental_node_sensor": (
        "node_sensor 实验工程",
        "电池传感器路线可能复用；先保留并在低功耗阶段复核。",
    ),
    "experimental_node_switch": (
        "node_switch 实验工程",
        "小工程，待确认是否仍有参考价值。",
    ),
    "temporary_scripts": (
        "临时补丁脚本",
        "先在清单中标注无用项；经用户确认后分批归档/删除。",
    ),
    "planning_docs": (
        "候选规划文档",
        "审查内容是否重复；有长期价值的改名并在 HANDOFF 索引。",
    ),
    "tool_payloads": (
        "工具命令样例",
        "若仍用于硬件测试可移动到 fixtures；否则归档。",
    ),
    "tool_assets": (
        "工具图片/静态素材",
        "被文档或工具引用则保留，否则归档。",
    ),
    "tool_notes": (
        "工具输出笔记",
        "若交接文档已记录结论，可归档。",
    ),
    "tool_source": (
        "未跟踪的工具源码",
        "逐个确认功能；仍在使用的工具应显式纳入 Git。",
    ),
    "firmware_misc": (
        "其他固件文件",
        "需要确认是否属于当前工程或实验工程。",
    ),
    "other": (
        "其他未分类",
        "需要人工查看，不自动处理。",
    ),
}


@dataclass(frozen=True)
class Rule:
    category: str
    matcher: Callable[[tuple[str, ...], str], bool]


def first_dir(parts: tuple[str, ...], name: str, depth: int = 1) -> bool:
    return len(parts) > depth and parts[depth].lower() == name


def second_starts(parts: tuple[str, ...], prefix: str) -> bool:
    return len(parts) > 1 and parts[1].lower().startswith(prefix)


def rules() -> list[Rule]:
    # 明确的构建目录必须放在泛化串口日志规则之前。
    return [
        Rule("firmware_backups", lambda p, _f: (p[0] == "backups"
             and len(p) > 1 and p[1] == "firmware")),
        Rule("partial_reports", lambda p, f: (first_dir(p, "backups", 0)
             and f.startswith("overnight-partial-"))),
        Rule("patch_artifacts", lambda p, _f: first_dir(p, "patches")),
        Rule("source_snapshots", lambda p, _f: first_dir(p, "source")),
        Rule("temporary_backup_dirs", lambda p, _f: (p[0] == "backups"
             and second_starts(p, "tmp-"))),
        Rule("build_output_baud_460800", lambda p, _f: (
            len(p) >= 4 and p[0] == "firmware" and p[1] == "node_zigbee"
            and p[2] == "460800"
        )),
        Rule("experimental_node_combined",
             lambda p, _f: len(p) > 2 and p[0] == "firmware"
             and p[1] == "node_combined"),
        Rule("experimental_node_sensor",
             lambda p, _f: len(p) > 2 and p[0] == "firmware"
             and p[1] == "node_sensor"),
        Rule("experimental_node_switch",
             lambda p, _f: len(p) > 2 and p[0] == "firmware"
             and p[1] == "node_switch"),
        Rule("serial_logs", lambda p, f: (
            len(p) >= 3 and p[0] == "firmware" and f.startswith("log.")
            and f.endswith(".txt")
        )),
        Rule("temporary_scripts", lambda p, f: (
            len(p) >= 2 and p[0] == "tools" and f.startswith("_")
            and f.endswith(".py")
        )),
        Rule("planning_docs", lambda p, f: (
            len(p) >= 2 and p[0] == "docs" and f.endswith(".md")
        )),
        Rule("tool_payloads", lambda p, f: (
            len(p) >= 2 and p[0] == "tools" and f.endswith(".json")
        )),
        Rule("tool_assets", lambda p, f: (
            len(p) >= 2 and p[0] == "tools" and f.endswith(".png")
        )),
        Rule("tool_notes", lambda p, f: (
            len(p) >= 2 and p[0] == "tools" and f.endswith(".txt")
        )),
        Rule("tool_source", lambda p, f: (
            len(p) >= 2 and p[0] == "tools" and f.endswith(".py")
        )),
        Rule("firmware_misc", lambda p, _f: len(p) >= 2 and p[0] == "firmware"),
    ]


def classify_path(path: str) -> tuple[str, str, str]:
    normalized = validate_relative_path(path)
    pure = PurePosixPath(normalized)
    parts = tuple(part.lower() for part in pure.parts)
    filename = pure.name.lower()

    for rule in rules():
        if rule.matcher(parts, filename):
            title, proposal = CATEGORY_META[rule.category]
            return rule.category, title, proposal
    title, proposal = CATEGORY_META["other"]
    return "other", title, proposal


def validate_relative_path(path: str) -> str:
    if not path or "\x00" in path:
        raise ValueError("路径为空或包含非法控制字符")
    normalized = path.replace("\\", "/")
    if (re.match(r"^[A-Za-z]:", normalized) or
            normalized.startswith("/") or "//" in normalized or
            normalized.endswith("/")):
        raise ValueError("Git 返回了绝对路径或空路径段")
    pure = PurePosixPath(normalized)
    if any(part in ("", "..") for part in pure.parts):
        raise ValueError("Git 返回了空路径段或越界路径")
    return normalized


def add_top(heap: list, item: dict, limit: int) -> None:
    heap_item = (item["size"], item["path"], item["category"])
    if len(heap) < limit:
        heapq.heappush(heap, heap_item)
    elif item["size"] > heap[0][0]:
        heapq.heapreplace(heap, heap_item)


def heap_to_items(heap: list) -> list[dict]:
    return [
        {"size": size, "path": path, "category": category}
        for size, path, category in sorted(heap, reverse=True)
    ]


def summarize_paths(root: Path, rel_paths: list[str],
                    excluded_paths: set[str]) -> dict:
    categories: dict[str, dict] = {}
    extension_counts: Counter[str] = Counter()
    extension_sizes: defaultdict[str, int] = defaultdict(int)
    inaccessible = []
    invalid_paths = []
    invalid_paths_count = 0
    inaccessible_count = 0
    accessible_files = 0
    total_accessible_size = 0
    global_top: list = []

    def category_bucket(category: str, title: str,
                        proposal: str) -> dict:
        return categories.setdefault(category, {
            "title": title,
            "proposal": proposal,
            "files": 0,
            "size": 0,
            "inaccessible": 0,
        })

    for rel in rel_paths:
        try:
            normalized = validate_relative_path(rel)
        except ValueError as exc:
            invalid_paths_count += 1
            if len(invalid_paths) < TOP_INACCESSIBLE:
                invalid_paths.append({"path": rel, "reason": str(exc)})
            continue
        if normalized in excluded_paths:
            continue

        item_path = root / rel
        category, title, proposal = classify_path(normalized)
        bucket = category_bucket(category, title, proposal)

        try:
            stat = item_path.lstat()
        except OSError as exc:
            inaccessible_count += 1
            bucket["inaccessible"] += 1
            if len(inaccessible) < TOP_INACCESSIBLE:
                inaccessible.append({
                    "path": normalized,
                    "category": category,
                    "reason": str(exc),
                })
            continue

        item = {
            "path": normalized,
            "size": stat.st_size,
            "category": category,
        }
        accessible_files += 1
        total_accessible_size += stat.st_size
        bucket["files"] += 1
        bucket["size"] += stat.st_size
        add_top(global_top, item, TOP_GLOBAL)

        suffix = PurePosixPath(normalized).suffix.lower() or "[no extension]"
        extension_counts[suffix] += 1
        extension_sizes[suffix] += stat.st_size

    return {
        "generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "root": str(root),
        "accessible_files": accessible_files,
        "accessible_size": total_accessible_size,
        "inaccessible_count": inaccessible_count,
        "invalid_paths_count": invalid_paths_count,
        "categories": categories,
        "extension_counts": extension_counts,
        "extension_sizes": extension_sizes,
        "largest": heap_to_items(global_top),
        "inaccessible": inaccessible,
        "invalid_paths": invalid_paths,
    }


def list_untracked(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    return [item for item in result.stdout.decode("utf-8").split("\0") if item]


def collect(root: Path, excluded_paths: set[str]) -> dict:
    return summarize_paths(root, list_untracked(root), excluded_paths)


def mib(value: int) -> str:
    return f"{value / 1024 / 1024:.2f}"


def markdown_code(text: str, table_cell: bool = False) -> str:
    # CommonMark 行内代码中反斜杠不能转义反引号；按内容中最长的连续
    # 反引号动态选择更长定界符，避免异常文件名提前结束代码跨度。
    normalized = text.replace(chr(13), " ").replace(chr(10), " ")
    longest_run = max(
        (len(match.group()) for match in re.finditer("`+", normalized)),
        default=0,
    )
    fence = "`" * (longest_run + 1)
    if table_cell:
        # GFM 表格先按未转义的竖线切分。若竖线前已有反斜杠，需把
        # 该段反斜杠成对转义后，再额外用一个反斜杠转义竖线。
        normalized = re.sub(
            r"(\\*)\|",
            lambda match: chr(92) * (2 * len(match.group(1)) + 1) + "|",
            normalized,
        )
    if normalized.startswith("`") or normalized.endswith("`"):
        normalized = f" {normalized} "
    return f"{fence}{normalized}{fence}"


def render_markdown(data: dict) -> str:
    lines = []
    lines.append("# 工作区未跟踪文件盘点清单")
    lines.append("")
    lines.append(f"- 生成时间：{data['generated_at']}")
    lines.append(f"- 项目根目录：{markdown_code(data['root'])}")
    lines.append(f"- 可读取的未跟踪文件：{data['accessible_files']} 个")
    lines.append(f"- 合计大小：{mib(data['accessible_size'])} MiB")
    lines.append(f"- 无法读取：{data['inaccessible_count']} 个")
    lines.append(f"- 异常路径：{data['invalid_paths_count']} 个")
    lines.append("- 除本输出文件及必要父目录外，未删除、未移动、未修改其他文件。")
    lines.append("")
    lines.append("## 分类汇总")
    lines.append("")
    lines.append("| 分类 | 可读取文件数 | 大小 MiB | 无法读取 | 建议 |")
    lines.append("|---|---:|---:|---:|---|")
    for key, bucket in sorted(
        data["categories"].items(),
        key=lambda item: item[1]["size"],
        reverse=True,
    ):
        lines.append(
            f"| {bucket['title']} ({markdown_code(key, table_cell=True)}) | {bucket['files']} | "
            f"{mib(bucket['size'])} | {bucket['inaccessible']} | "
            f"{bucket['proposal']} |"
        )

    lines.append("")
    lines.append("## 扩展名汇总（前 20 项）")
    lines.append("")
    lines.append("| 扩展名 | 文件数 | 大小 MiB |")
    lines.append("|---|---:|---:|")
    for ext, count in data["extension_counts"].most_common(20):
        lines.append(
            f"| {markdown_code(ext, table_cell=True)} | {count} | {mib(data['extension_sizes'][ext])} |"
        )

    lines.append("")
    lines.append("## 最大的 25 个未跟踪文件")
    lines.append("")
    lines.append("| 大小 MiB | 分类 | 路径 |")
    lines.append("|---:|---|---|")
    for item in data["largest"]:
        lines.append(
            f"| {mib(item['size'])} | {markdown_code(item['category'], table_cell=True)} | "
            f"{markdown_code(item['path'], table_cell=True)} |"
        )

    if data["inaccessible"]:
        lines.append("")
        lines.append(
            f"## 无法读取的文件（展示前 {len(data['inaccessible'])} 项，"
            f"共 {data['inaccessible_count']} 项）"
        )
        lines.append("")
        lines.append("| 原因 | 分类 | 路径 |")
        lines.append("|---|---|---|")
        for item in data["inaccessible"]:
            lines.append(
                f"| {markdown_code(item['reason'], table_cell=True)} | "
                f"{markdown_code(item['category'], table_cell=True)} | {markdown_code(item['path'], table_cell=True)} |"
            )

    if data["invalid_paths"]:
        lines.append("")
        lines.append(
            f"## 异常路径（展示前 {len(data['invalid_paths'])} 项，"
            f"共 {data['invalid_paths_count']} 项）"
        )
        lines.append("")
        lines.append("| 原因 | 路径 |")
        lines.append("|---|---|")
        for item in data["invalid_paths"]:
            lines.append(
                f"| {markdown_code(item['reason'], table_cell=True)} | "
                f"{markdown_code(item['path'], table_cell=True)} |"
            )

    lines.append("")
    lines.append("## 建议的安全整理顺序")
    lines.append("")
    lines.append("1. 先由用户确认实验工程和关键日志的去留，不做任何自动删除；")
    lines.append("2. 对确认的构建产物目录补充忽略规则，再清理本地生成文件；")
    lines.append("3. 将关键串口日志和固件备份压缩归档，保留与交接文档对应的证据；")
    lines.append("4. 临时补丁脚本按批次确认无用后再删除；")
    lines.append("5. 每完成一小批整理就提交一次 Git，提交中不使用 `git add -A`。")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(description="生成未跟踪文件盘点清单")
    parser.add_argument("--output", type=Path, required=True,
                        help="Markdown 输出路径")
    args = parser.parse_args()

    root = Path.cwd()
    output = args.output.resolve()
    try:
        excluded = {output.relative_to(root).as_posix()}
    except ValueError:
        excluded = set()

    try:
        data = collect(root, excluded)
    except subprocess.CalledProcessError as exc:
        print(f"读取 Git 状态失败，退出码 {exc.returncode}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"无法读取仓库状态：{exc}", file=sys.stderr)
        return 2

    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(render_markdown(data), encoding="utf-8")
    except OSError as exc:
        print(f"无法写入盘点清单：{exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
