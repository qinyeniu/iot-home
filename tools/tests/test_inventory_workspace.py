import importlib.util
import sys
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = TOOLS_DIR / "inventory_workspace.py"

spec = importlib.util.spec_from_file_location("inventory_workspace", MODULE_PATH)
inventory = importlib.util.module_from_spec(spec)
sys.modules["inventory_workspace"] = inventory
spec.loader.exec_module(inventory)


def test_classify_known_untracked_paths():
    cases = {
        "backups/firmware/a.bin": "firmware_backups",
        "backups/overnight-partial-1.json": "partial_reports",
        "backups/patches/a.patch": "patch_artifacts",
        "backups/source/main.c": "source_snapshots",
        "backups/tmp-x/file.tmp": "temporary_backup_dirs",
        "firmware/gateway/log.example.txt": "serial_logs",
        "firmware/node_zigbee/460800/a.obj": "build_output_baud_460800",
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
    for path, expected in cases.items():
        assert inventory.classify_path(path)[0] == expected
        assert inventory.classify_path(path.replace("/", "\\"))[0] == expected


def test_category_metadata_exists():
    category, _, _ = inventory.classify_path("unknown/random.bin")
    assert category == "other"
    assert inventory.CATEGORY_META[category][1]
