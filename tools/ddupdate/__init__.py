"""游戏更新的校验与修复（U1）。入口是 tools/dd_update.py，模块分工：

  manifest     解析 signatures.inc、layout.inc（X-macro），清单的检查
  image        PE 映像：按 RVA 读、异常目录（.pdata）里的函数范围、特征码扫描
  sigs         特征码的解析（和 DDCore 的 Resolve 同义）和 check_sigs 的全部检查
  evidence     可在运行时求值的依据（DD_EVIDENCE），语义和 DDCore 的运行时核对一致
  offline      只能离线求值的依据（DD_OFFLINE），按名字注册
  fingerprint  函数指纹、相似度、调用关系（基线、同一性检查、修复共用）
  baseline     基线快照的生成和读取
  verify       verify 命令：逐项结论、依赖表、JSON 报告、退出码
  simstats     相似度度量的数据验证
  index        整个 exe 的函数指纹索引（按 SHA1 缓存到 build/ddupdate-cache/），修复的指纹搜索和调用图用
  align        解码 / 放宽模式、基线函数和新函数之间的指令对齐
  locate       函数级匹配（放宽特征码、调用图、锚点、指纹搜索）和特征码的新位置
  regen        按原条目的种类和语义重新生成特征码、依据模式，并自检
  layoutfix    布局项的修复：重新求值依据、新值、新依据模式
  patch        清单文件的改写和 unified diff
  repair       repair / accept / blind 命令、预演 verify、报告、RVA 对照表
  position     依据位置核对（依据误绑定），verify 的同一性检查和修复共用
  pepatch      改写 exe 副本：搬迁、复制函数，原地改位移和立即数，unwind 信息和异常目录（变异样本用）
  mutants      selftest 的变异样本：每个样本模拟的真实变化、真值、verify 和 repair 应有的结论
  mcheck       清单变异冒烟：Steam 版基线 + 离线版 exe + 改坏的清单副本
  selftest     selftest 命令：盲定位、变异样本、清单变异、往返、C++/Python 对照
  cpp_compare  C++/Python 对照测试（DDCore 的运行时核对和 verify 逐项比较）
"""
