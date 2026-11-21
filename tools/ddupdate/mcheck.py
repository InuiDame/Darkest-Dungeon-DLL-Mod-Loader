"""清单变异冒烟（U1c 的 build/u1c/mcheck.py，U1d 并进 selftest）：Steam 版基线 + 离线版 exe + 故意改坏的清单副本。

离线版和 Steam 版的结构相同、RVA 不同，原清单在离线版上全部通过；把清单副本改坏 16 处（特征码里的常量、操作码、寄存器，
依据模式里的字面量，清单项的值，起点依赖值的项，值写在字面量里的 NONE 依据），模拟「游戏没变、清单和它对不上」。要求：
  - 改坏的清单上 verify 能发现：EffectDotStress 的依据模式里前后两个字段的字面量改成「前一个是 0x1B8、后一个是 0x13C」，
    模式唯一命中后一个字段（.clearDotStress）的解析，依据位置核对把它判成依据误绑定（U1d 之前 verify 发现不了）。
    U1 修复 FB：原来用的是 EffectDotBurn（模式靠「下一个字段是 0x148」区分），U1 修复 FA 收紧后它前后两个字段都带字面量，
    同样的改法只会让它命中 0 次、判不成立（运行时也能发现），不再是误绑定的样本，所以换成 EffectDotStress、两个字面量一起改；
    EffectDotBurn 那一处保留，作为「改坏的模式判不成立、修复不能修错」的一项
  - repair --apply 之后，每条特征码在离线版上的解析结果、每个清单项的值都和原清单一致，或者报「需要人工」；修错的个数为 0
  - 值写在模式字面量里的 MapNodeLeft 必须报需要人工（不能自动重新生成）

    python -B DD.ModLoader/tools/ddupdate/mcheck.py         （也由 dd_update.py selftest 调用）
"""
import json
import shutil
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from ddupdate import manifest as manifest_mod  # noqa: E402
    from ddupdate import repair as repair_mod  # noqa: E402
    from ddupdate import sigs as sigs_mod  # noqa: E402
    from ddupdate import verify as verify_mod  # noqa: E402
    from ddupdate.baseline import BASELINE_DIR, load as load_baseline  # noqa: E402
    from ddupdate.image import Image  # noqa: E402
    from ddupdate.manifest import PROJECT  # noqa: E402
else:
    from . import manifest as manifest_mod
    from . import repair as repair_mod
    from . import sigs as sigs_mod
    from . import verify as verify_mod
    from .baseline import BASELINE_DIR, load as load_baseline
    from .image import Image
    from .manifest import PROJECT

NOSTEAM = dict(verify_mod.DEFAULT_EXES)["nosteam"]
OUT_DIR = PROJECT / "build" / "u1d" / "mcheck"

# (文件, 行首, 原文, 改成, 说明)
MUTATIONS = [
    ("sig", "DD_SIG(UpdateStateInternal,", "48 81 EC E8 03 00 00", "48 81 EC F0 03 00 00", "UpdateStateInternal 栈帧立即数"),
    ("sig", "DD_SIG(RaidCurrent,", "48 8B 88 C0 52 00 00", "48 8B 88 C8 52 00 00", "RaidCurrent 位移"),
    ("sig", "DD_SIG(MonsterBlockReturn,", "41 B8 31 00 00 00", "41 B8 32 00 00 00", "MonsterBlockReturn 立即数"),
    ("sig", "DD_SIG(BuffInstanceCopy,", "48 8B 53 08 48 3B 53 10", "48 8B 53 0C 48 3B 53 10", "BuffInstanceCopy 位移"),
    ("sig", "DD_SIG(BuffGroupErase,", "48 83 EC 20 49 8B D8", "48 83 EC 28 49 8B D8", "BuffGroupErase 栈帧"),
    ("sig", "DD_SIG(ListenerAdd,", "48 89 5C 24 08 48 89 74 24 10 57", "48 89 5C 24 08 48 89 74 24 10 56", "ListenerAdd 操作码"),
    ("sig", "DD_SIG(EventTypeCount,", "3B 3D ?? ?? ?? ?? 72 ?? 48 8D 05", "3B 3D ?? ?? ?? ?? 73 ?? 48 8D 05", "EventTypeCount 操作码"),
    ("sig", "DD_SIG(StorageManager,", "45 33 ED 4C 89 6C 24 58", "45 33 F6 4C 89 6C 24 58", "StorageManager 寄存器"),
    ("sig", "DD_SIG(EffectLookup,", "48 83 EC 28 33 C9", "48 83 EC 28 33 D2", "EffectLookup 寄存器"),
    ("layout", "DD_EVIDENCE(InstDuration, BuffDurationUpkeep,", "48 81 C3 18 04 00 00", "48 81 C3 20 04 00 00",
     "InstDuration 依据模式字面量"),
    ("layout", "DD_EVIDENCE(EffectDotBurn,", "4C 8D 86 40 01 00 00", "4C 8D 86 48 01 00 00", "EffectDotBurn 依据模式字面量（收紧后命中 0 次）"),
    ("layout", "DD_EVIDENCE(EffectDotStress,", "4C 8D 86 CC 01 00 00", "4C 8D 86 B8 01 00 00", "EffectDotStress 前一个字段的字面量"),
    ("layout", "DD_EVIDENCE(EffectDotStress,", "4C 8D 86 BC 01 00 00", "4C 8D 86 3C 01 00 00",
     "EffectDotStress 后一个字段的字面量（两处一起改，唯一命中 .clearDotStress 的解析）"),
    ("layout", "DD_LAYOUT(HeroClass,", "0x1320", "0x1328", "HeroClass 值"),
    ("layout", "DD_LAYOUT(VtSlotDamage,", ", 50,", ", 52,", "VtSlotDamage 槽号"),
    ("layout", "DD_LAYOUT(MonsterBlockImmuneLeaDisp,", ", 52,", ", 48,", "MonsterBlockImmuneLeaDisp 命中偏移"),
    ("layout", "DD_LAYOUT(BattleMonsters,", "0x38", "0x40", "BattleMonsters 值（RaidBattle 的 rel）"),
    ("layout", "DD_EVIDENCE(MapNodeLeft,", "48 8B 00 80 78", "48 8B 40 80 78", "MapNodeLeft 的 NONE 依据（值在字面量里）"),
]
MUST_MANUAL = {"MapNodeLeft"}
MUST_MISBIND = {"EffectDotStress"}


def _sub_line(text, prefix, old, new):
    lines = text.split("\n")
    for i, ln in enumerate(lines):
        if ln.startswith(prefix):
            if old not in ln:
                raise ValueError(f"{prefix} 这一行里没有 {old}")
            lines[i] = ln.replace(old, new, 1)
            return "\n".join(lines)
    raise ValueError(f"没找到 {prefix}")


def run(out_dir=OUT_DIR, log=print):
    """返回 (是否通过, 汇总 dict)。"""
    out_dir = Path(out_dir)
    tree = out_dir / "tree"
    shutil.rmtree(tree, ignore_errors=True)
    (tree / "core" / "src" / "game").mkdir(parents=True)
    (tree / "tools" / "baseline").mkdir(parents=True)
    shutil.copy(BASELINE_DIR / "steam.json", tree / "tools" / "baseline" / "steam.json")
    texts = {"sig": manifest_mod.SIGS_FILE.read_text(encoding="utf-8"),
             "layout": manifest_mod.LAYOUT_FILE.read_text(encoding="utf-8")}
    for which, prefix, old, new, _what in MUTATIONS:
        texts[which] = _sub_line(texts[which], prefix, old, new)
    sig_file = tree / "core" / "src" / "game" / "signatures.inc"
    layout_file = tree / "core" / "src" / "game" / "layout.inc"
    sig_file.write_text(texts["sig"], encoding="utf-8", newline="")
    layout_file.write_text(texts["layout"], encoding="utf-8", newline="")

    # 改坏的清单上 verify 要发现依据误绑定
    broken = manifest_mod.load(str(sig_file), str(layout_file))
    baselines = [load_baseline(tree / "tools" / "baseline" / "steam.json")]
    before = verify_mod.verify_exe(NOSTEAM, broken, baselines, verify_mod.SECTIONS, sigs_mod.foreign_patches())
    misbound = {f["name"] for f in before["failures"] if f["severity"] == "misbind"}
    problems = []
    for name in sorted(MUST_MISBIND - misbound):
        problems.append(f"改坏的清单上 verify 没有把 {name} 判成误绑定")

    result, after = repair_mod.repair_exe(NOSTEAM, tree=str(tree), apply=True, out=str(out_dir / "repair"))
    orig = manifest_mod.load()
    fixed = manifest_mod.load(str(sig_file), str(layout_file))
    image = Image(NOSTEAM)
    ro = sigs_mod.resolve_all(image, orig, "nosteam", [])
    rf = sigs_mod.resolve_all(image, fixed, "nosteam", [])
    rows, wrong = [], 0
    for sid in orig.sigs:
        st = result["sigs"][sid]["status"]
        a, b = ro[sid], rf[sid]
        same = a.status == b.status == "ok" and a.resolved == b.resolved and (orig.sigs[sid].kind == "GLOBAL_ANY" or a.hit == b.hit)
        if st != "ok" or not same:
            rows.append(f"{sid:24} {st:12} 原 {a.resolved and hex(a.resolved)} 修复后 {b.status} {b.resolved and hex(b.resolved)} "
                        f"{'一致' if same else '不一致'}")
            wrong += (not same) and st != "manual"
    manual = set()
    for iid, item in orig.items.items():
        r = result["layout"].get(iid, {})
        if r.get("status") == "manual":
            manual.add(iid)
        if r.get("status") not in ("ok", "unverified") or fixed.items[iid].value != item.value:
            same = fixed.items[iid].value == item.value
            rows.append(f"{iid:28} {r.get('status')} 原 {item.value:#x} 修复后 {fixed.items[iid].value:#x} {'一致' if same else '不一致'}"
                        + ("；" + "；".join(r.get("reasons", [])) if r.get("status") == "manual" else ""))
            wrong += (not same) and r.get("status") != "manual"
    if wrong:
        problems.append(f"错误修复 {wrong} 个")
    for name in sorted(MUST_MANUAL - manual):
        problems.append(f"{name} 应当报需要人工")
    summary = {"mutations": [m[4] for m in MUTATIONS], "verify_before": {"exit_code": before["exit_code"],
                                                                         "misbind": sorted(misbound)},
               "repair_exit": result["exit_code"], "after_verify": after["exit_code"] if after else None,
               "wrong": wrong, "manual": sorted(manual), "rows": rows, "problems": problems}
    (out_dir / "mcheck.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    log(f"  改坏 {len(MUTATIONS)} 处；改坏的清单上 verify 退出码 {before['exit_code']}，误绑定 {sorted(misbound)}")
    log(f"  repair 退出码 {result['exit_code']}，apply 后 verify 退出码 {summary['after_verify']}；修错 {wrong}；需要人工 {sorted(manual)}")
    for row in rows:
        log(f"    {row}")
    for p in problems:
        log(f"  不通过：{p}")
    return not problems, summary


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ok, _ = run()
    sys.exit(0 if ok else 1)
