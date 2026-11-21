"""已并入 tools/dd_update.py verify（U1a）。保留这个入口给旧的文档和习惯用：

    python -B DD.ModLoader/tools/check_actor_layout.py [exe ...]

等同于 dd_update.py verify --only layout [exe ...]：core/src/game/layout.inc 里每条依据求值（原脚本的全部检查都在里面，
离线的部分在 tools/ddupdate/offline.py），加上依据位置核对（U1 修复 FB 起 --only layout 也做，误绑定和无法确认照样报，
审核 A 备注 6），没有静态依据的项单列。退出码同 verify（0 通过，1 只有可选项失效，2 必需项失效、依据误绑定或无法确认，
3 清单或参数有错）。新的用法见 dd_update.py 的说明。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dd_update  # noqa: E402

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(dd_update.main(["verify", "--only", "layout", *sys.argv[1:]]))
