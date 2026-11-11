"""比对测试目录里运行中的 Darkest.exe 的 .text 和磁盘文件，列出被改动的区间及跳转目标所属模块。
在 guest 上运行（工作目录 D:\\workplace）：python -B DD.ModLoader/tools/rig_textdiff.py
复用 SpineCompat/tools/rig_probe.py 的实现，只把目标目录换成 DD.ModLoader 的测试目录。
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "SpineCompat" / "tools"))

import rig_probe  # noqa: E402

rig_probe.RIG_MARK = os.environ.get("DD_RIG_NAME", "DarkestDungeon_Dev")

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(rig_probe.cmd_textdiff() or 0)
