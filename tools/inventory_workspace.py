#!/usr/bin/env python3
"""生成未跟踪文件盘点清单。

工具只读取 Git 状态和文件元数据，不删除、不移动、不修改任何文件。
用途是在仓库清理前提供分类、大小、风险和建议，避免误删硬件实测证据。
"""

from __future__ import annotations

import argparse
import subprocess
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable

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
        "临时备份目录",
        "需要用户确认；优先归档而不是直接删除。",
    ),
    "serial_logs": (
        "串口实测日志",
        "保留关键短日志和异常节点日志；重复/巨型日志建议压缩归档，不直接删。",
    ),
    "build_output_baud_460800": (
        "460800 目录中的构建产物",
        "确认无唯一证据后可清理，并补充忽略规则。",
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
    matcher: Callable[[str], bool]


def has_prefix(prefix: str) -> Callable[[str], bool]:
    return lambda path: path.startswith(prefix)


def has_suffix(suffix: str) -> Callable[[str], bool]:
    return lambda path: path.endswith(suffix)


RULES = [
    Rule("firmware_backups", has_prefix("backups/firmware/")),
    Rule("partial_reports", lambda p: p.startswith("backups/overnight-partial-")),
    Rule("patch_artifacts", has_prefix("backups/patches/")),
    Rule("source_snapshots", has_prefix("backups/source/")),
    Rule("temporary_backup_dirs", lambda p: p.startswith("backups/tmp-")),
    Rule("serial_logs", lambda p: p.startswith("firmware/") and "/log." in p
         and p.endswith(".txt")),
    Rule("build_output_baud_460800",
         has_prefix("firmware/node_zigbee/460800/")),
    Rule("experimental_node_combined",
         has_prefix("firmware/node_combined/")),
    Rule("experimental_node_sensor",
         has_prefix("firmware/node_sensor/")),
    Rule("experimental_node_switch",
         has_prefix("firmware/node_switch/")),
    Rule("temporary_scripts", lambda p: p.startswith("tools/_")
         and p.endswith(".py")),
    Rule("planning_docs", lambda p: p.startswith("docs/") and p.endswith(".md")),
    Rule("tool_payloads", lambda p: p.startswith("tools/") and p.endswith(".json")),
    Rule("tool_assets", lambda p: p.startswith("tools/") and p.endswith(".png")),
    Rule("tool_notes", lambda p: p.startswith("tools/") and p.endswith(".txt")),
    Rule("tool_source", has_prefix("tools/")),
    Rule("firmware_misc", has_prefix("firmware/")),
]


def classify_path(path: str) -> tuple[str, str, str]:
    normalized = path.replace("\\", "/")
    for rule in RULES:
        if rule.matcher(normalized):
            title, proposal = CATEGORY_META[rule.category]
            return rule.category, title, proposal
    title, proposal = CATEGORY_META["other"]
    return "other", title, proposal


def list_untracked(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    return [item for item in result.stdout.decode("utf-8").split("\0") if item]


def collect(root: Path, excluded_paths: set[str]) -> dict:
    categories: dict[str, dict] = {}
    extension_counts: Counter[str] = Counter()
    extension_sizes: defaultdict[str, int] = defaultdict(int)
    files = []

    for rel in list_untracked(root):
        normalized = rel.replace("\\", "/")
        if normalized in excluded_paths:
            continue
        item_path = root / rel
        try:
            stat = item_path.stat()
        except OSError:
            continue
        category, title, proposal = classify_path(normalized)
        entry = {
            "path": normalized,
            "size": stat.st_size,
            "category": category,
        }
        files.append(entry)

        bucket = categories.setdefault(
            category,
            {"title": title, "proposal": proposal, "files": 0, "size": 0,
             "largest": []},
        )
        bucket["files"] += 1
        bucket["size"] += stat.st_size
        bucket["largest"].append(entry)

        suffix = Path(normalized).suffix.lower() or "[no extension]"
        extension_counts[suffix] += 1
        extension_sizes[suffix] += stat.st_size

    for bucket in categories.values():
        bucket["largest"].sort(key=lambda item: item["size"], reverse=True)
        bucket["largest"] = bucket["largest"][:10]

    return {
        "generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "root": str(root),
        "total_files": len(files),
        "total_size": sum(item["size"] for item in files),
        "categories": categories,
        "extension_counts": extension_counts,
        "extension_sizes": extension_sizes,
        "largest": sorted(files, key=lambda item: item["size"],
                          reverse=True)[:25],
    }


def mb(value: int) -> str:
    return f"{value / 1024 / 1024:.2f}"


def render_markdown(data: dict) -> str:
    lines = []
    lines.append("# 工作区未跟踪文件盘点清单")
    lines.append("")
    lines.append(f"- 生成时间：{data['generated_at']}")
    lines.append(f"- 项目根目录：`{data['root']}`")
    lines.append(f"- 未跟踪文件：{data['total_files']} 个")
    lines.append(f"- 合计大小：{mb(data['total_size'])} MB")
    lines.append("- 本清单只做分析，未删除、未移动、未修改任何文件。")
    lines.append("")
    lines.append("## 分类汇总")
    lines.append("")
    lines.append("| 分类 | 文件数 | 大小 MB | 建议 |")
    lines.append("|---|---:|---:|---|")
    for key, bucket in sorted(
        data["categories"].items(),
        key=lambda item: item[1]["size"],
        reverse=True,
    ):
        lines.append(
            f"| {bucket['title']} (`{key}`) | {bucket['files']} | "
            f"{mb(bucket['size'])} | {bucket['proposal']} |"
        )

    lines.append("")
    lines.append("## 扩展名汇总（前 20 项）")
    lines.append("")
    lines.append("| 扩展名 | 文件数 | 大小 MB |")
    lines.append("|---|---:|---:|")
    for ext, count in data["extension_counts"].most_common(20):
        lines.append(
            f"| `{ext}` | {count} | {mb(data['extension_sizes'][ext])} |"
        )

    lines.append("")
    lines.append("## 最大的 25 个未跟踪文件")
    lines.append("")
    lines.append("| 大小 MB | 分类 | 路径 |")
    lines.append("|---:|---|---|")
    for item in data["largest"]:
        lines.append(
            f"| {mb(item['size'])} | `{item['category']}` | "
            f"`{item['path']}` |"
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

    data = collect(root, excluded)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render_markdown(data), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
