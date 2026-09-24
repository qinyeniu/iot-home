# 工作区未跟踪文件盘点清单

- 生成时间：2026-09-24T08:47:39+08:00
- 项目根目录：`C:\Users\HJB\Documents\iot-home`
- 未跟踪文件：646 个
- 合计大小：572.85 MB
- 本清单只做分析，未删除、未移动、未修改任何文件。

## 分类汇总

| 分类 | 文件数 | 大小 MB | 建议 |
|---|---:|---:|---|
| 串口实测日志 (`serial_logs`) | 37 | 457.24 | 保留关键短日志和异常节点日志；重复/巨型日志建议压缩归档，不直接删。 |
| 460800 目录中的构建产物 (`build_output_baud_460800`) | 461 | 63.12 | 确认无唯一证据后可清理，并补充忽略规则。 |
| 固件二进制/ELF 备份 (`firmware_backups`) | 23 | 51.94 | 保留关键 SHA 对应版本；确认 Git/备份盘已有副本后再归档，不建议直接删除。 |
| 源码快照 (`source_snapshots`) | 2 | 0.18 | 仅在确认 Git 历史包含对应版本后归档。 |
| 临时补丁脚本 (`temporary_scripts`) | 61 | 0.12 | 先在清单中标注无用项；经用户确认后分批归档/删除。 |
| 其他固件文件 (`firmware_misc`) | 14 | 0.06 | 需要确认是否属于当前工程或实验工程。 |
| 临时备份目录 (`temporary_backup_dirs`) | 1 | 0.03 | 需要用户确认；优先归档而不是直接删除。 |
| 未跟踪的工具源码 (`tool_source`) | 11 | 0.02 | 逐个确认功能；仍在使用的工具应显式纳入 Git。 |
| 补丁文件备份 (`patch_artifacts`) | 2 | 0.02 | 已合入的补丁可归档；清理前与最终代码抽查比对。 |
| 候选规划文档 (`planning_docs`) | 4 | 0.02 | 审查内容是否重复；有长期价值的改名并在 HANDOFF 索引。 |
| node_combined 实验工程 (`experimental_node_combined`) | 7 | 0.02 | 先判断是否为当前组合节点路线；需要用户确认后再收敛或归档。 |
| node_sensor 实验工程 (`experimental_node_sensor`) | 7 | 0.02 | 电池传感器路线可能复用；先保留并在低功耗阶段复核。 |
| node_switch 实验工程 (`experimental_node_switch`) | 6 | 0.02 | 小工程，待确认是否仍有参考价值。 |
| 工具输出笔记 (`tool_notes`) | 1 | 0.02 | 若交接文档已记录结论，可归档。 |
| 夜间阶段性分析产物 (`partial_reports`) | 4 | 0.01 | 若交接文档已吸收结论，可归档；否则先保留。 |
| 工具命令样例 (`tool_payloads`) | 4 | 0.00 | 若仍用于硬件测试可移动到 fixtures；否则归档。 |
| 工具图片/静态素材 (`tool_assets`) | 1 | 0.00 | 被文档或工具引用则保留，否则归档。 |

## 扩展名汇总（前 20 项）

| 扩展名 | 文件数 | 大小 MB |
|---|---:|---:|
| `.obj` | 227 | 24.78 |
| `.cmake` | 132 | 0.32 |
| `.py` | 71 | 0.16 |
| `.txt` | 55 | 457.48 |
| `[no extension]` | 26 | 0.29 |
| `.a` | 26 | 22.94 |
| `.bin` | 19 | 17.72 |
| `.d` | 13 | 0.21 |
| `.json` | 12 | 7.69 |
| `.c` | 10 | 0.28 |
| `.md` | 9 | 0.04 |
| `.in` | 9 | 0.03 |
| `.yml` | 5 | 0.00 |
| `.defaults` | 5 | 0.00 |
| `.elf` | 4 | 34.13 |
| `.lock` | 3 | 0.00 |
| `.csv` | 3 | 0.00 |
| `.patch` | 2 | 0.02 |
| `.out` | 2 | 0.18 |
| `.ninja` | 2 | 6.13 |

## 最大的 25 个未跟踪文件

| 大小 MB | 分类 | 路径 |
|---:|---|---|
| 456.09 | `serial_logs` | `firmware/gateway/log.overnight-gw-20260923-034906.txt` |
| 11.30 | `firmware_backups` | `backups/firmware/2026-09-23-lock-health-hardening/iot-home-gateway.elf` |
| 11.23 | `firmware_backups` | `backups/firmware/2026-09-23-strict-rf-transaction/iot-home-gateway.elf` |
| 7.07 | `build_output_baud_460800` | `firmware/node_zigbee/460800/esp-idf/wpa_supplicant/libwpa_supplicant.a` |
| 6.03 | `build_output_baud_460800` | `firmware/node_zigbee/460800/build.ninja` |
| 5.80 | `firmware_backups` | `backups/firmware/2026-09-23-lock-health-hardening/iot-home-zigbee-node.elf` |
| 5.80 | `firmware_backups` | `backups/firmware/2026-09-23-strict-rf-transaction/iot-home-zigbee-node.elf` |
| 5.11 | `build_output_baud_460800` | `firmware/node_zigbee/460800/compile_commands.json` |
| 3.21 | `build_output_baud_460800` | `firmware/node_zigbee/460800/esp-idf/driver/libdriver.a` |
| 2.32 | `build_output_baud_460800` | `firmware/node_zigbee/460800/config/kconfig_menus.json` |
| 1.43 | `build_output_baud_460800` | `firmware/node_zigbee/460800/esp-idf/esp_driver_mcpwm/libesp_driver_mcpwm.a` |
| 1.37 | `firmware_backups` | `backups/firmware/2026-09-23-lock-health-hardening/iot-home-gateway.bin` |
| 1.37 | `firmware_backups` | `backups/firmware/2026-09-23-pre-assert-debug/iot-home-gateway.bin` |
| 1.37 | `firmware_backups` | `backups/firmware/2026-09-23-strict-rf-transaction/iot-home-gateway.bin` |
| 1.37 | `firmware_backups` | `backups/firmware/2026-09-23-runtime-rf-policy/iot-home-gateway.bin` |
| 1.36 | `firmware_backups` | `backups/firmware/2026-09-23-configurable-atpc-bounds/iot-home-gateway.bin` |
| 1.36 | `firmware_backups` | `backups/firmware/2026-09-23-failure-driven-atpc/iot-home-gateway.bin` |
| 1.35 | `firmware_backups` | `backups/firmware/2026-09-22-low-tx-power/iot-home-gateway.bin` |
| 1.35 | `firmware_backups` | `backups/firmware/2026-09-22-pre-rf-cli/iot-home-gateway.bin` |
| 1.15 | `build_output_baud_460800` | `firmware/node_zigbee/460800/esp-idf/esp_driver_i2s/libesp_driver_i2s.a` |
| 1.03 | `build_output_baud_460800` | `firmware/node_zigbee/460800/esp-idf/esp_driver_spi/libesp_driver_spi.a` |
| 0.86 | `firmware_backups` | `backups/firmware/2026-09-23-lock-health-hardening/iot-home-zigbee-node.bin` |
| 0.86 | `firmware_backups` | `backups/firmware/2026-09-23-pre-assert-debug/iot-home-zigbee-node.bin` |
| 0.86 | `firmware_backups` | `backups/firmware/2026-09-23-runtime-rf-policy/iot-home-zigbee-node.bin` |
| 0.86 | `firmware_backups` | `backups/firmware/2026-09-23-strict-rf-transaction/iot-home-zigbee-node.bin` |

## 建议的安全整理顺序

1. 先由用户确认实验工程和关键日志的去留，不做任何自动删除；
2. 对确认的构建产物目录补充忽略规则，再清理本地生成文件；
3. 将关键串口日志和固件备份压缩归档，保留与交接文档对应的证据；
4. 临时补丁脚本按批次确认无用后再删除；
5. 每完成一小批整理就提交一次 Git，提交中不使用 `git add -A`。
