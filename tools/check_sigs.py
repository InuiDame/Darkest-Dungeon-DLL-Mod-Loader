"""已并入 tools/dd_update.py verify（U1a）。保留这个入口给旧的文档和习惯用：

    python -B DD.ModLoader/tools/check_sigs.py [exe ...]

等同于 dd_update.py verify --only sigs,identity [exe ...]：特征码的全部检查（唯一命中、GLOBAL_ANY 的规则、偷取范围、
DD_ENTRY、和 knowledge/dd1/hooks.md 不重叠）加上和基线的同一性比较。退出码同 verify（0 通过，1 只有可选项失效，
2 必需项失效或有误绑定，3 清单或参数有错）。新的用法见 dd_update.py 的说明。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dd_update  # noqa: E402

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(dd_update.main(["verify", "--only", "sigs,identity", *sys.argv[1:]]))
