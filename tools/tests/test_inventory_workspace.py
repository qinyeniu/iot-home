import importlib.util
import sys
from pathlib import Path

import pytest


TOOLS_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = TOOLS_DIR / "inventory_workspace.py"

spec = importlib.util.spec_from_file_location("inventory_workspace", MODULE_PATH)
inventory = importlib.util.module_from_spec(spec)
sys.modules["inventory_workspace"] = inventory
spec.loader.exec_module(inventory)


CLASSIFICATION_CASES = {
    "backups/firmware/a.bin": "firmware_backups",
    "backups/overnight-partial-1.json": "partial_reports",
    "backups/patches/a.patch": "patch_artifacts",
    "backups/source/main.c": "source_snapshots",
    "backups/tmp-x/file.tmp": "temporary_backup_dirs",
    "backups/tmp-foo.txt": "temporary_backup_dirs",
    "firmware/gateway/log.example.txt": "serial_logs",
    "firmware/node_zigbee/460800/a.obj": "build_output_baud_460800",
    "firmware/node_zigbee/460800/log.example.txt":
        "build_output_baud_460800",
    "firmware/node_combined/build.ninja": "experimental_node_combined",
    "firmware/node_sensor/main.c": "experimental_node_sensor",
    "firmware/node_switch/main.c": "experimental_node_switch",
    "tools/_temporary.py": "temporary_scripts",
    "docs/example.md": "planning_docs",
    "tools/cmd.json": "tool_payloads",
    "tools/hotspot.png": "tool_assets",
    "tools/note.txt": "tool_notes",
    "tools/real_tool.py": "tool_source",
    "firmware/sdkconfig.defaults": "firmware_misc",
}


@pytest.mark.parametrize("rel_path", CLASSIFICATION_CASES)
def test_classify_known_untracked_paths(rel_path):
    expected = CLASSIFICATION_CASES[rel_path]
    assert inventory.classify_path(rel_path)[0] == expected
    assert inventory.classify_path(rel_path.replace("/", chr(92)))[0] == expected
    assert inventory.classify_path(rel_path.upper())[0] == expected


def test_similar_prefixes_are_not_misclassified():
    assert inventory.classify_path("backups/firmware2/a.bin")[0] == "other"
    assert inventory.classify_path("firmware/node_combined_extra/x.c")[0] == "firmware_misc"


def test_category_metadata_exists():
    category, _, proposal = inventory.classify_path("unknown/random.bin")
    assert category == "other"
    assert proposal


def test_every_rule_has_metadata():
    for rule in inventory.rules():
        assert rule.category in inventory.CATEGORY_META
        title, proposal = inventory.CATEGORY_META[rule.category]
        assert title and proposal


@pytest.mark.parametrize("bad_path", [
    "",
    "/absolute.txt",
    "C:/windows/path.txt",
    "../escape.txt",
    "firmware//empty.txt",
    "firmware/" + chr(0) + ".txt",
])
def test_validate_relative_path_rejects_bad_values(bad_path):
    with pytest.raises(ValueError):
        inventory.validate_relative_path(bad_path)


def test_markdown_code_uses_dynamic_backtick_fence():
    source = "a|b`c" + chr(13) + "d" + chr(10) + "e" + chr(92) + "f"
    text = inventory.markdown_code(source)

    assert text == "``a|b`c d e" + chr(92) + "f``"
    assert chr(92) + "|" not in text
    assert chr(92) + "`" not in text
    assert chr(13) not in text
    assert chr(10) not in text


def test_markdown_code_pads_backtick_boundaries():
    assert inventory.markdown_code("`x``") == "``` `x`` ```"


def test_markdown_code_table_survives_markdown_parser():
    pytest.importorskip("markdown_it")
    from markdown_it import MarkdownIt

    malicious_path = (
        "a" + chr(92) + "b|c`x" + chr(13) + "d``tail"
    )
    expected = "a" + chr(92) + "b|c`x d``tail"
    source = (
        "| 大小 MiB | 路径 |\n"
        "|---:|---|\n"
        f"| 1.00 | {inventory.markdown_code(malicious_path, table_cell=True)} |\n"
    )

    tokens = MarkdownIt("commonmark").enable("table").parse(source)

    code_tokens = [
        child
        for token in tokens
        for child in (token.children or [])
        if child.type == "code_inline"
    ]

    assert sum(token.type == "table_open" for token in tokens) == 1
    assert sum(token.type == "tr_open" for token in tokens) == 2
    assert len(code_tokens) == 1
    assert code_tokens[0].content == expected


def test_summarize_paths_counts_aggregates_excludes_and_inaccessible(tmp_path):
    serial = tmp_path / "firmware/gateway/log.serial.txt"
    build = tmp_path / "firmware/node_zigbee/460800/log.build.txt"
    serial.parent.mkdir(parents=True)
    build.parent.mkdir(parents=True)
    serial.write_text("12345", encoding="utf-8")
    build.write_text("1234567", encoding="utf-8")

    result = inventory.summarize_paths(
        tmp_path,
        [
            "firmware/gateway/log.serial.txt",
            "firmware/node_zigbee/460800/log.build.txt",
            "missing.txt",
            "/invalid.txt",
            "firmware/gateway/log.excluded.txt",
        ],
        {"firmware/gateway/log.excluded.txt"},
    )

    assert result["accessible_files"] == 2
    assert result["accessible_size"] == 12
    assert result["inaccessible_count"] == 1
    assert result["invalid_paths_count"] == 1
    assert result["categories"]["serial_logs"]["files"] == 1
    assert result["categories"]["serial_logs"]["size"] == 5
    assert result["categories"]["build_output_baud_460800"]["files"] == 1
    assert result["categories"]["build_output_baud_460800"]["size"] == 7
    assert result["categories"]["other"]["inaccessible"] == 1
    assert result["largest"][0]["path"].endswith("log.build.txt")
    assert result["inaccessible"][0]["path"] == "missing.txt"
    assert result["invalid_paths"][0]["path"] == "/invalid.txt"


@pytest.mark.parametrize("bad_path", ["", "/absolute.txt"])
def test_classify_path_rejects_invalid_paths(bad_path):
    with pytest.raises(ValueError):
        inventory.classify_path(bad_path)


def test_inaccessible_count_is_not_limited_by_display_list(tmp_path):
    missing_paths = [f"missing-{index}.txt" for index in range(26)]
    result = inventory.summarize_paths(tmp_path, missing_paths, set())

    assert result["inaccessible_count"] == 26
    assert len(result["inaccessible"]) == inventory.TOP_INACCESSIBLE
    assert result["categories"]["other"]["inaccessible"] == 26
    assert result["inaccessible"][-1]["path"] == "missing-24.txt"


def test_invalid_paths_count_is_not_limited_by_display_list(tmp_path):
    invalid_paths = ["/invalid.txt"] * 26
    result = inventory.summarize_paths(tmp_path, invalid_paths, set())

    assert result["invalid_paths_count"] == 26
    assert len(result["invalid_paths"]) == inventory.TOP_INACCESSIBLE


def test_render_markdown_states_inaccessible_total_and_display_count():
    data = {
        "generated_at": "now",
        "root": "C:/tmp",
        "accessible_files": 0,
        "accessible_size": 0,
        "inaccessible_count": 26,
        "invalid_paths_count": 0,
        "categories": {},
        "extension_counts": inventory.Counter(),
        "extension_sizes": {},
        "largest": [],
        "inaccessible": [
            {"reason": "missing", "category": "other",
             "path": f"missing-{index}.txt"}
            for index in range(25)
        ],
        "invalid_paths": [],
    }
    text = inventory.render_markdown(data)
    assert "共 26 项" in text
    assert "展示前 25 项" in text
