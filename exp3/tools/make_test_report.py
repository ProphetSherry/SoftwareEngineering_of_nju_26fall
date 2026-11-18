"""Generate the technical test record from executed JSON and CTest artifacts."""
import argparse
import hashlib
import json
import platform
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVIDENCE = ROOT / "evidence"

# Map every literal action in window.cpp, plus dynamically rendered variants,
# to executable UI cases. Missing/new actions stop report generation.
COVERAGE = {
    "‹ 返回发现活动": "P07", "‹ 返回活动详情": "P10",
    "为修改申请修改 / 新增票种": "R04 R05", "为修改申请添加场次变更": "R03",
    "保存并提交发布审核": "D14B D15", "保存活动草稿": "D02 D08",
    "保存账号修改": "A06 A08B", "创建新账号": "A01 A02 A03 A04",
    "删除草稿": "O11 O12", "删除草稿场次 / 票种": "O08 O09 O10",
    "刷新活动与人员": "A14 A15 A16", "取消报名": "P12", "取消本人报名": "P14",
    "取消，返回我的活动": "D01 D09 D14B", "取消，返回账号管理": "A05 A08 A09",
    "启用 / 停用选中账号": "A07 A07B A09B", "备份与检查": "Q06 Q07 Q08 Q09",
    "备份数据库": "Q07 Q08", "导出 CSV 名单": "Q02 Q03",
    "我的申请": "N-REQ D11", "我的申请 · 查看 / 撤回": "N-REQ",
    "拒绝申请": "R08 R09", "拒绝申请（必填原因）": "R08 R09",
    "按容量生成座位编号": "D07", "提交发布审核": "D10 D13",
    "撤回待审核申请": "D14", "撤回申请": "D12", "撤回选中申请": "D17",
    "新建活动": "D01 D02", "新建活动 ＋": "D01 D02",
    "查看原内容与候选内容": "D16B", "查看原版本与申请内容 ›": "D16",
    "查看活动 ›": "P06 P08", "查询日志": "Q04 Q05 Q10",
    "核验并签到": "C01 C02", "检查数据完整性": "Q06 Q09",
    "消息通知": "N-MSG", "添加场次 ＋": "D03", "添加场次（草稿）": "O01 O02",
    "添加票种 ＋": "D05", "添加票种与座位（草稿）": "O03 O04 O04B",
    "申请取消活动 / 场次 / 票种": "R-C1 R-C2 R-C3",
    "登录": "L04 L05", "登记超时 / 取消后的异常实收（全额待退款）": "M07 M08",
    "确认分配": "A10 A11 A13", "确认撤销分配": "A12 A16",
    "确认已完成线下全额退款": "M09", "确认已收到线下付款": "M03",
    "确认报名": "P11 P13", "确认线下收款": "M01 M02", "确认线下退款": "M04 M05",
    "移除此场次": "D04", "移除此票种": "D06", "筛选": "P01 P02 P04",
    "提交修改审核": "R02",
    "编辑 ›": "A05 A06", "编辑活动 / 申请发布后修改": "R02",
    "编辑草稿场次": "O05 O06", "编辑草稿票种与座位": "O07 O07B",
    "编辑账号": "A08", "退出登录": "L07", "选择票种": "P08 P10",
    "通过审核": "R06 R07 R-CA1 R-CA2 R-CA3 D18",
    "首次使用 · 初始化管理员": "L02 L03", "编辑活动": "D09 R01",
    "查看待审核申请": "D11", "申请取消": "R-C1 R-C2 R-C3", "⋯ 更多菜单": "L06",
}


def norm(text):
    return " ".join(text.split())


def cell(text):
    return str(text).replace("|", "／").replace("\n", "<br>")


def table(headers, rows):
    return ["| " + " | ".join(headers) + " |",
            "| " + " | ".join("---" for _ in headers) + " |"] + [
        "| " + " | ".join(cell(value) for value in row) + " |" for row in rows]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--date", default="2026-11-18")
    args = parser.parse_args()
    result = json.loads((EVIDENCE / "button-tests.json").read_text())
    baseline = json.loads((EVIDENCE / "button-tests-before-fixes.json").read_text())
    cases = result["cases"]
    ids = {case["id"]: case for case in cases}
    source = (ROOT / "src/window.cpp").read_text()
    pattern = (r'(?:new\s+QPushButton\s*\(|new\s+QToolButton\s*\(|(?<!\w)Button\s*\('
               r'|addAction\s*\(|CardAction\s*\(\s*layout\s*,)\s*"([^"\n]+)"')
    literal_labels = {norm(text) for text in re.findall(pattern, source)}
    missing = literal_labels - COVERAGE.keys()
    if missing:
        raise SystemExit("Unmapped source buttons: " + ", ".join(sorted(missing)))
    for label, mapping in COVERAGE.items():
        for case_id in mapping.split():
            if case_id not in ids:
                raise SystemExit(f"Missing executed case: {label}: {case_id}")
    # Include each role's dynamically created bottom navigation tab.
    coverage = dict(COVERAGE)
    for case in cases:
        if case["area"] == "导航" and " → " in case["button"]:
            coverage[case["button"]] = case["id"]
    core_log = (ROOT / "build/Testing/Temporary/LastTest.log").read_text()
    core_summary = re.search(r"(\d+) assertions, (\d+) scenarios, (\d+) failures", core_log)
    if not core_summary:
        raise SystemExit("Missing current executed core test output")
    core_passes = re.findall(r"^PASS (.+)$", core_log, re.M)
    asan_path = EVIDENCE / "button-tests-asan.json"
    asan = json.loads(asan_path.read_text()) if asan_path.exists() else None
    real_path = EVIDENCE / "real-ime.status"
    real_exit = real_path.read_text().strip() if real_path.exists() else None
    all_log = (EVIDENCE / "all-tests.log").read_text() if (EVIDENCE / "all-tests.log").exists() else ""
    suite_passed = "100% tests passed" in all_log
    lines = ["# 校园活动系统：逐按钮功能测试记录", "",
             f"测试日期：{args.date}（Asia/Shanghai，客户端日期）。", "",
             "本文为程序的技术测试记录。自动测试结果来自执行日志与 JSON；人工试用反馈单独标注。", "",
             "## 1. 本轮结论", ""]
    passed = sum(case["status"] == "PASS" for case in cases)
    lines += [f"- 逐按钮及交互场景：**{len(cases)} 项，{passed} 项通过，{result['failed']} 项失败**；共执行 {result['assertions']} 项检查。",
              f"- 核心业务回归：**{core_summary[2]} 个场景、{core_summary[1]} 项断言、{core_summary[3]} 项失败**。",
              f"- 完整 CTest：{'全部通过' if suite_passed else '未取得全部通过记录，请查看日志'}；包含业务测试、中文与完整草稿流程测试、逐按钮测试。",
              f"- 源码清单提取到 {len(literal_labels)} 个不同文字的按钮／菜单动作，另补充动态标签及各角色导航；全部在下方覆盖矩阵中关联到已执行用例。",
              f"- 修复前定向执行 {len(baseline['cases'])} 项，发现 {baseline['failed']} 项失败，归为下述 4 类问题；修复后增加边界与菜单分支用例，再执行完整回归。", "",
              "本次新增 M06A：验证组织者报名管理的活动、场次与状态组合筛选，活动切换后场次重置，全部活动恢复名单。", "",
              "补充回归：发布后编辑已接入完整表单，R01 检查场次字段存在，R02 实际编辑活动、场次和票种并提交同一审核。新增核心场景 complete_published_edit / complete_published_edit_protection，验证新增场次票种、审批前隔离、审批生效、报名票价保护、失败回滚及撤回。此前的按钮测试未检查编辑字段是否满足完整需求。", "",
              "全部通过只表示这些用例与前置数据下未再复现问题，不表示所有输入组合、平台、窗口缩放和系统故障都已穷尽。", "",
              "## 2. 测试方式与环境", "",
              f"- 系统：`{platform.platform()}`；Qt `{result['qt_version']}`，平台插件 `{result['platform']}`。",
              "- 构建：C++17、CMake、GCC，启用 `-Wall -Wextra -Wpedantic -Werror`。",
              "- 每组使用独立临时 SQLite 数据库，通过业务接口创建账号、活动、报名和审核前置数据；未读取或改写用户正在使用的业务数据库。",
              "- UI 测试通过 `QPushButton::click()`、单选按钮点击和 `QAction::trigger()` 执行真实信号与处理函数，再核对页面、数据库状态及生成文件。菜单展开也单独检查。",
              "- 弹窗通过实际 OK／Cancel、Yes／No、保存按钮完成；文件选择强制使用 Qt 非原生对话框，窗口关闭使用 Qt close 操作。",
              "- 输入字段、组合筛选、角色切换、时间边界、无匹配、空名单、重复请求、并发及回滚由 UI 与业务测试组合验证。",
              "- 自动点击信号不等同于人工鼠标命中测试；操作系统原生文件窗口、标题栏按钮、输入法候选窗样式、所有 DPI／屏幕缩放组合不在这份自动按钮覆盖率内。", ""]
    if asan:
        asan_log = (EVIDENCE / "button-tests-asan.log").read_text()
        assert not re.search(r"ERROR: AddressSanitizer|runtime error:|SUMMARY: (?:AddressSanitizer|UndefinedBehaviorSanitizer)", asan_log)
        assert (EVIDENCE / "asan.status").read_text().strip() == "0"
        lines += [f"- 历史 AddressSanitizer／UndefinedBehaviorSanitizer：执行 {len(asan['cases'])} 项 UI 用例，失败 {asan['failed']} 项；本次未重跑，不代表当前新增筛选代码已完成 sanitizer 检查。",
                  "- 此环境禁用 LeakSanitizer（`ASAN_OPTIONS=detect_leaks=0`），因此不声称完成内存泄漏检查。"]
    else:
        lines += ["- Sanitizer：本次尚未取得可用执行结果，不计通过。"]
    if real_exit == "0":
        lines += ["- 此前真实中文输入测试通过：独立 X11 窗口实际键入拼音，Fcitx5 提交中文并保存到测试数据库；见 `evidence/real-ime-current.log`。本次文档整理与报名筛选更新未重新执行真实桌面输入测试；当前 CTest 已重新验证模拟中文输入。"]
    else:
        lines += [f"- 本轮真实桌面中文输入：{'未执行' if real_exit is None else '未通过或环境阻塞（退出码 '+real_exit+'）'}，不计入按钮通过项。模拟中文提交和数据库保存的测试已单独执行；实际桌面情况见后文限制。"]
    lines += ["", "## 3. 本轮发现并修复的问题", ""]
    findings = [
        ("F01", "O03、O05、O08、R06", "取消被当成错误", "取消场次选择、编辑输入或审核意见后仍弹出“已取消操作”。数据未改变，但交互误报。", "用独立的 UI 取消异常结束流程，取消时不再显示错误警告。", "O03 O05 O08 R06"),
        ("F02", "P13", "重复报名暴露数据库错误", "取消后再报名出现 registrations 唯一约束错误。", "在事务内检查该用户活动报名历史，区分已取消不可重报与已有报名。", "P13"),
        ("F03", "O04B", "座位冲突暴露数据库错误", "新增票种使用已有座位时出现 seats 唯一约束错误。", "新增、编辑票种及批准候选座位前检查同场次座位冲突，失败整体回滚。补测编辑重复座位。", "O04B O07B"),
        ("F04", "A03", "重复登录名暴露数据库错误", "创建相同账号时直接显示 users.username 唯一约束错误。", "创建前在写事务内检查登录名，提示使用其他账号，不增加账号或成功日志。", "A03"),
    ]
    rows = []
    for fid, originals, title, before, fix, related in findings:
        state = "复测通过" if all(ids[key]["status"] == "PASS" for key in related.split()) else "仍有失败"
        rows.append((fid, title, originals, before, fix, state))
    lines += table(["编号", "问题", "修复前失败用例", "复现结果", "修改", "最终状态"], rows)
    lines += ["", "修复前原始证据：`evidence/button-tests-before-fixes.json` 和同名 `.log`。最早一轮只检查取消行为的结果另保存在 `button-tests-initial.json`。", "",
              "## 4. 逐项测试记录", "",
              "每一行都对应 `button_tests.cpp` 中执行过的测试。取消用例期望不写入数据；错误用例期望被明确拒绝，不是把任意报错算作成功。", ""]
    groups = {}
    for case in cases:
        groups.setdefault(case["area"], []).append(case)
    for area, group in groups.items():
        lines += [f"### {area}", ""]
        lines += table(["ID", "测试按钮／动作", "步骤", "预期", "实测结果", "结论"], [
            (case["id"], case["button"], case["steps"], case["expected"], case["actual"],
             "通过" if case["status"] == "PASS" else "失败") for case in group])
        lines += [""]
    lines += ["## 5. 按钮与菜单完整性核对", "",
              "以下清单从当前 `src/window.cpp` 提取文字动作，再补充运行时生成的标签及导航。相同文字的公共组件合并列出；表中列出实际执行用例 ID，不能找到用例的新增按钮会令生成脚本失败。", ""]
    matrix = []
    for label, mapping in sorted(coverage.items()):
        ok = all(ids[key]["status"] == "PASS" for key in mapping.split())
        matrix.append((label, mapping.replace(" ", "、"), "通过" if ok else "存在失败"))
    lines += table(["按钮／菜单／导航", "关联用例", "结果"], matrix)
    lines += ["", "标准对话框的 OK／Cancel／Yes／No／保存、票种单选、角色与筛选下拉、金额与容量输入、滚动区域，作为上方用例的步骤检查。日历内部每个日期格及所有原生系统按钮未逐个穷举。", "",
              "## 6. 核心业务与既有完整流程回归", ""]
    lines += table(["已执行测试名称", "结果"], [(name, "通过") for name in core_passes if name not in [case["button"] for case in cases] and not re.match(r"[A-Z][A-Z0-9-]* ", name)])
    benchmark = (EVIDENCE / "benchmark.txt").read_text() if (EVIDENCE / "benchmark.txt").exists() else ""
    summary = "\n".join(line for line in benchmark.splitlines() if not line.startswith("query_"))
    lines += ["", "### 历史本地并发查询", "", "以下为既有 `campus_benchmark` 日志，本次未重新执行性能专项。本机 SQLite 查询，不含 HTTP／网络延迟，也不代表所有接口都具有同样性能。", "", "```text", summary, "```", "",
              "## 7. 尚未覆盖与当前限制", "",
              "### 人工试用与本次处理", "",
              "用户已完成个人简要试用，但未提供逐项步骤和截图，本文不将其计为自动测试通过项。", "",
              "- 签到提示请求失败：经代码与默认演示库只读核对，桌游活动场次开始于 2026-11-21 09:00（北京时间）；2026-11-18 试用时尚未开始，触发时间门禁。未更改签到规则或失败提示。C01/C02 检查允许时间内签到与重复签到拒绝。",
              "- 组织者无法按活动及场次筛选报名：已增加活动、场次联动选择，保留状态组合过滤，并由 M06A 回归验证。该项是人工反馈触发、自动测试复核的功能补充。", "",
              "### 测试边界", "",
              "1. 自动 UI 测试使用 offscreen，不能证明真实鼠标在所有窗口缩放下都能点击到每个控件。已检查 393×852 窗口与页面构建；人工可结合截图复核。",
              "2. 文件保存使用 Qt 对话框完成，未对 Windows／Linux 原生文件选择器的每个内部按钮逐项测试。",
              "3. 中文输入事件测试能检查文本接收与持久化，不能单独证明真实 Fcitx5 进程始终在线；真实输入结果需以独立日志为准。",
              "4. 为触发付款过期和可签到场次，前置数据通过可注入时钟生成；真实等待数小时、跨天长期运行、系统休眠恢复未在本轮穷举。",
              "5. 定义尚未确定的特殊退款认定、外部通知送达、跨接口统一快照等未实现流程，不能作为已通过项目。",
              "6. 不声称零缺陷；新按钮、代码或需求变化后应重新运行。", "",
              "## 8. 复现命令与证据", "", "从仓库根目录运行：", "", "```bash",
              "./exp3/tools/build.sh > exp3/evidence/all-tests.log 2>&1", "source exp3/tools/environment.sh",
              "QT_QPA_PLATFORM=offscreen ./exp3/build/campus_button_tests exp3/evidence/button-tests.json",
              "./exp3/build/campus_benchmark > exp3/evidence/benchmark.txt",
              f"python3 exp3/tools/make_test_report.py --date {args.date}", "```", "",
              "真实桌面输入测试（会打开独立测试窗口，勿操作该窗口）：", "", "```bash",
              "./exp3/tools/test-real-input-method.sh", "```", "",
              "- UI 用例源码：`tests/button_tests.cpp`。",
              "- 核心业务源码：`tests/core_tests.cpp`。",
              "- 完整草稿、中文、撤回和账号编辑测试：`tests/gui_tests.cpp`。",
              "- 当前逐按钮结果：`evidence/button-tests.json`、`evidence/button-tests.log`。",
              "- 修复前结果：`evidence/button-tests-before-fixes.json`、`evidence/button-tests-before-fixes.log`。",
              "- CTest：`evidence/all-tests.log`、`evidence/ctest-details.log`；后者从构建目录的 `LastTest.log` 留存，便于 Git 托管。",
              "- Sanitizer：`evidence/button-tests-asan.json`、`evidence/button-tests-asan.log`。",
              "- 性能：`evidence/benchmark.txt`。", "",
              "当前相关源码 SHA-256（识别此次测试版本；未执行 Git 提交或推送）：", ""]
    lines += table(["文件", "SHA-256"], [(name, hashlib.sha256((ROOT/name).read_bytes()).hexdigest())
                  for name in ["src/window.cpp", "src/service.cpp", "tests/button_tests.cpp"]])
    style = json.loads((EVIDENCE / "style-check-current.json").read_text())
    lines += ["", "## 9. 本次代码风格检查", "",
              f"检查日期：{style['date']}。cpplint {style['versions']['cpplint']} 与 clang-format {style['versions']['clang-format']}，覆盖 include、src、tests 与 tools 下全部 C++ 源文件。", ""]
    lines += table(["工具", "退出码", "结果", "日志"], [
        (name, style[name]["exit_code"], "通过" if style[name]["exit_code"] == 0 else "失败",
         "evidence/" + style[name]["log"]) for name in ["cpplint", "clang-format"]])
    lines += ["", "本次修正新增报名筛选代码缺失的 <algorithm> 头文件和排版；修正后重新构建并执行全部 CTest。配置允许的例外见 CPPLINT.cfg，不代表对所有语言规则或代码异味作出完整证明。", ""]
    path = ROOT / "程序功能测试记录.md"
    path.write_text("\n".join(lines) + "\n")
    print(f"Wrote {path}: {len(cases)} cases, {len(coverage)} action/navigation entries")


if __name__ == "__main__":
    main()
