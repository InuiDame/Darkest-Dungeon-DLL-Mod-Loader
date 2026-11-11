-- Lua 5.4.9 本地包定义和 Lua 相关依赖（ADR 0012）
-- 工程内定义的 package("lua") 优先于远程仓库；只编译静态库，不带 lua.exe/luac.exe，不开兼容选项。
--
-- DD.ModLoader 的改动（P3a）：
-- 1. Lua 按 C++ 编译（/TP /EHsc）：lua_error 变成 C++ 异常（ldo.c 的 LUAI_THROW），从 C++ 函数、计数 hook
--    里报错时，经过的 C++ 栈帧照常析构。头文件因此是 C++ 链接，不安装带 extern "C" 的 lua.hpp，
--    使用方（sol2）通过本包导出的 SOL_USING_CXX_LUA=1 直接包含 lua.h。
-- 2. 指令预算覆盖库函数内部的长循环：模式匹配（match() 的每一次 goto init 迭代、[...] 字符集在 classend 和
--    matchbracketclass 里的扫描、%f、max_expand、%b、捕获比较）、plain find、string.rep、string.pack/unpack/packsize
--    的格式串，table.insert/remove/move/concat/sort 的元素循环，按工作量调用 lddstep.h 的 lua_ddstep_fn。
--    string.rep 的结果为空串时直接返回，不空转 n 次。
--    dd_patch 4、5（第二轮审核 R2A-阻断1）：gsub 的替换串（add_s）、string.format 和 %q、upper/lower/reverse、
--    string.byte/char、plain find 的扫描和普通模式的检查、table.pack/unpack、utf8.len/codepoint/char/offset/codes、
--    next 跳过空槽也计步。原则：计数 hook 每 1000 条指令才检查一次，反复调用时两次检查之间可以有几百次 C 调用，
--    所以单次调用的工作量和数据量有关的循环都要计步（只有固定上限的不计，比如捕获数 32、对齐 16）。
--    没有 lua_State 的地方（长串比较 luaS_eqlngstr、长串的哈希、字符串转数字 l_str2d、l_strcmp、tonumber 的进制转换）
--    用 lua_ddstep_nol_fn：DDLua 发现超时后只置粘滞标记，让正在运行的线程在下一条指令触发计数 hook 报错。
-- 3. lua_closethread 重置线程时恢复 allowhook（死在 hook 里的协程，__close 也要受预算约束）。
-- 4. LUA_IDSIZE 改为 160，错误信息里的 "@<mod id>/<文件>" 不被截断。
-- 5. dd_patch 4（第二轮审核 R2A-阻断2）：lgc.c 的 traversetable 把只有弱键、没有弱值的表（ephemeron）按强引用表遍历。
--    ephemeron 链让原子阶段的 convergeephemerons 变成平方级（4 万项的链每轮 GC 卡 2.7 秒），GC 停顿不受预算约束。
--    __mode 含 v 的表照常。沙箱的语义差异写在 docs/03-lua-system.md。
-- 6. dd_patch 6、7（第三轮审核 R3A，结构性的修法）：
--    - 表的哈希（ltable.c）：整数、浮点数、指针键经过带密钥的哈希（密钥每个进程随机生成，再混入表自己的地址），
--      Mod 算不出冲突键；扩容时 reinsert 直接插入、不先查找，即使有冲突也是线性的。字符串哈希的种子改用系统随机数（lstate.c）。
--    - 正在运行的线程：lua_resume、lua_closethread 进出时通知 DDLua（lua_ddswitch_fn），没有 lua_State 的进度回调
--      对真正在运行的线程设 hook（协程开始运行、让出后回到恢复方、嵌套、coroutine.close 运行 <close> 都覆盖）。
--    - 没有 lua_State 的进度检查放在操作之后（长串比较、长串哈希、字符串转数字），跨过截止时间的那次操作当场发现；
--      入口末尾的超时由 DDLua 在入口结束时补查。l_strcmp 改成 memcmp 加长度（C locale 下等价），l_str2int 的空白、
--      十六进制数字、前导零，tonumber(s, base) 的空白，math.min/max 的参数，string.find 普通模式检查里每个 \0 分段都计步。
--    - 紧急 GC（lmem.c 的 tryagain）先问 DDLua（lua_ddemergency_fn），做完再告诉它结果：同一个入口里已经做过一次没起作用的、
--      入口已经超时、或者不久前刚做过一次没起作用的，就不再做，分配直接失败。
-- 7. dd_patch 8（第三轮核对审核的应修 1：GC 停顿算到无辜的回调头上）：
--    - lgc.c 的 setpause 把下一轮的阈值交给 DDLua（lua_ddgcthreshold_fn），DDLua 把它压到内存上限以下：活对象超过上限一半时，
--      原版的阈值（活对象的 2 倍）永远到不了，增量 GC 不再启动，每次都是分配撞上上限、做一次完整的紧急 GC。
--    - singlestep 的原子阶段前后通知 DDLua（lua_ddatomic_fn）。原子阶段和紧急 GC 一样是整段停顿，和当前入口自己的分配量无关，
--      DDLua 给它们计时，补到当前入口的截止时间上（每个入口有上限）。
--    - lddstep.h 的 LUA_DDPATCH 和 dd_patch 一致。
-- 改了补丁就把 dd_patch 加一，包的哈希随之改变，xmake 会重新构建；lddstep.h 的 LUA_DDPATCH 同时改成同一个数。
package("lua")
    set_homepage("https://www.lua.org/")
    set_description("Lua 5.4.9, static library for DDLua (compiled as C++, DD patches)")
    set_license("MIT")

    add_urls("https://www.lua.org/ftp/lua-$(version).tar.gz")
    add_versions("5.4.9", "2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6")

    add_configs("dd_patch", { description = "DD.ModLoader 补丁版本，补丁改动时加一（和 lddstep.h 的 LUA_DDPATCH 一致）", default = "8", type = "string" })

    add_includedirs("include/lua")

    on_load(function (package)
        -- Lua 是 C++ 链接，sol2 不能再用 extern "C" 包含
        package:add("defines", "SOL_USING_CXX_LUA=1")
    end)

    on_install("windows", function (package)
        -- 每个锚点必须在文件里唯一出现，否则构建失败（防止版本变化后补丁静默失效）
        local function patch(file, anchor, replacement)
            local data = io.readfile(file)
            local first, last = data:find(anchor, 1, true)
            assert(first, "lua patch: anchor not found in " .. file .. ": " .. anchor)
            assert(not data:find(anchor, last + 1, true), "lua patch: anchor not unique in " .. file .. ": " .. anchor)
            io.writefile(file, data:sub(1, first - 1) .. replacement .. data:sub(last + 1))
        end
        local function after(file, anchor, text) patch(file, anchor, anchor .. text) end
        local function before(file, anchor, text) patch(file, anchor, text .. anchor) end

        io.writefile("src/lddstep.h", [[
/*
** DD.ModLoader patch (xmake/lua.lua): long loops inside library functions that do not go
** through the instruction count hook report their work here, so DDLua's budget covers them.
** Lua is compiled as C++, so these symbols have C++ linkage.
*/
#ifndef lddstep_h
#define lddstep_h

#include <stddef.h>
#include "lua.h"

/* version of the DD patches (xmake/lua.lua dd_patch); the symbols below exist since 6, the GC
** pacing ones (lua_ddgcthreshold_fn, lua_ddatomic_fn) since 8 */
#define LUA_DDPATCH 8

/* called on the running thread every LUA_DDSTEP_QUANTUM units of work; may raise an error */
extern void (*lua_ddstep_fn)(lua_State *L);
/* same, for places without a lua_State; must not raise and must not touch any stack.
** Called after the work it reports (dd_patch 6), so the check sees the operation that crossed the deadline */
extern void (*lua_ddstep_nol_fn)(void);
extern size_t lua_ddstep_work;

/* lua_resume and lua_closethread report the thread that starts running and get back the thread
** that was running; when they return, that thread is reported again (ldo.c, lstate.c). Must not
** raise or allocate */
extern lua_State *(*lua_ddswitch_fn)(lua_State *L);
/* emergency collection (lmem.c tryagain): asked first with phase 0, returning 0 skips the
** collection (the allocation fails at once); after the collection told with phase 1 when the
** allocation still failed, phase 2 when it succeeded. Must not raise or allocate */
extern int (*lua_ddemergency_fn)(lua_State *L, int phase);
/* string hash seed of a new state (lstate.c); NULL keeps Lua's own luai_makeseed */
extern unsigned int (*lua_ddseed_fn)(void);
/* key of the hash for integer, float and pointer keys in tables (ltable.c). Set it once, before
** any state is created, and never change it while a state exists */
extern unsigned long long lua_ddhashkey[3];
/* GC pacing (lgc.c setpause): called on the main thread when a cycle ends, with the memory in use
** after it ('estimate') and Lua's threshold for the next cycle; returns the threshold to use.
** Must not raise or allocate */
extern long long (*lua_ddgcthreshold_fn)(lua_State *L, long long estimate, long long threshold);
/* atomic phase of a collection cycle (lgc.c singlestep, incremental cycles and full collections
** alike): phase 0 before, 1 after. Must not raise or allocate */
extern void (*lua_ddatomic_fn)(lua_State *L, int phase);

#define LUA_DDSTEP_QUANTUM ((size_t)1 << 16)

#define luai_ddstep(L, w) \
  do { \
    lua_ddstep_work += (size_t)(w); \
    if (lua_ddstep_work >= LUA_DDSTEP_QUANTUM) { \
      lua_ddstep_work = 0; \
      if (lua_ddstep_fn) lua_ddstep_fn(L); \
    } \
  } while (0)

#define luai_ddstep_nol(w) \
  do { \
    lua_ddstep_work += (size_t)(w); \
    if (lua_ddstep_work >= LUA_DDSTEP_QUANTUM) { \
      lua_ddstep_work = 0; \
      if (lua_ddstep_nol_fn) lua_ddstep_nol_fn(); \
    } \
  } while (0)

/* expression form (for macros such as lutf8lib's iscontp) */
static inline int luai_ddstep_e (lua_State *L, size_t w) {
  luai_ddstep(L, w);
  return 0;
}

#endif
]])

        -- lstrlib.c
        local str = "src/lstrlib.c"
        after(str, '#include "lualib.h"\n', '#include "lddstep.h"\n')
        before(str, "  else if (l_unlikely(l + lsep < l || l + lsep > MAXSIZE / n))",
            "  else if (l + lsep == 0)  /* DD: empty result, do not spin n times */\n    lua_pushliteral(L, \"\");\n")
        after(str, "    while (n-- > 1) {  /* first n-1 copies (followed by separator) */",
            "\n      luai_ddstep(L, l + lsep + 16);")
        after(str, "    int cont = 1;", "\n    luai_ddstep(ms->L, (size_t)(ms->src_end - s));")
        after(str, "  ptrdiff_t i = 0;  /* counts maximum expand for item */",
            "\n  luai_ddstep(ms->L, (size_t)(ms->src_end - s));")
        before(str, "  if (l_unlikely(ms->matchdepth-- == 0))", "  luai_ddstep(ms->L, 1);\n")
        -- match() 用 goto init 代替尾递归：字面字符和单个字符类的逐字推进、* ? - 接受空匹配、%f、%b 和回引用之后的
        -- 继续匹配都不经过上面的入口，每次迭代计 1（dd_patch 3，审核 A-A1：长度 m 的字面模式在长度 n 的串上是 n*m 次比较）
        after(str, "  init: /* using goto to optimize tail recursion */", "\n  luai_ddstep(ms->L, 1);")
        -- [...] 字符集：classend 的扫描和 matchbracketclass 的扫描都和集合长度成正比
        after(str, "        const char *ep = classend(ms, p);  /* points to optional suffix */",
            "\n        luai_ddstep(ms->L, (size_t)(ep - p));")
        after(str, "            ep = classend(ms, p);  /* points to what is next */",
            "\n            luai_ddstep(ms->L, 3 * (size_t)(ep - p));")
        patch(str, "      case '[': return matchbracketclass(c, p, ep-1);",
            "      case '[': luai_ddstep(ms->L, (size_t)(ep - p)); return matchbracketclass(c, p, ep-1);")
        -- string.pack/unpack/packsize：格式串里的空白、对齐等选项不产生输出，每个选项计 1
        after(str, "  KOption opt = getoption(h, fmt, psize);", "\n  luai_ddstep(h->L, 1);")
        -- 回引用：位置捕获的 len 是 CAP_POSITION（-2），原样加进计数等于减 2（第二轮 R2A-备注5），按 1 计
        after(str, "  len = ms->capture[l].len;", "\n  luai_ddstep(ms->L, ms->capture[l].len < 0 ? 1 : len);")
        patch(str, "static const char *lmemfind (const char *s1, size_t l1,",
            "static const char *lmemfind (lua_State *L, const char *s1, size_t l1,")
        after(str, "      init++;   /* 1st char is already checked */", "\n      luai_ddstep(L, l2 + 1);")
        -- memchr 本身的扫描：找不到时一次调用扫完整个串（约 16 字节一个单位）
        after(str, "    l1 = l1-l2;  /* 's2' cannot be found after that */", "\n    luai_ddstep(L, l1 >> 4);")
        patch(str, "lmemfind(s + init, ls - init, p, lp)", "lmemfind(L, s + init, ls - init, p, lp)")
        -- string.find 先用 strpbrk 检查模式里有没有特殊字符：100 MB 的模式每次调用扫一遍
        before(str, "  /* explicit request or no special characters? */", "  luai_ddstep(L, lp);\n")
        -- nospecials 每遇到一个 \0 调用一次 strpbrk 和 strlen：54 MB 的 \0 模式一次调用 242 ms（第三轮 R3A-应修2），每段计步
        patch(str, "static int nospecials (const char *p, size_t l) {", "static int nospecials (lua_State *L, const char *p, size_t l) {")
        patch(str, "    upto += strlen(p + upto) + 1;  /* may have more after \\0 */",
            "    {  /* DD patch: one strpbrk and one strlen per segment */\n      size_t ddseg = strlen(p + upto) + 1;\n" ..
            "      luai_ddstep(L, ddseg);\n      upto += ddseg;  /* may have more after \\0 */\n    }")
        patch(str, "nospecials(p, lp)", "nospecials(L, p, lp)")
        -- gsub 的替换串（第二轮 R2A-阻断1）：每次匹配把替换串扫一遍，'%0'、空捕获不产生输出，按扫过的字节计
        after(str, "  while ((p = (char *)memchr(news, L_ESC, l)) != NULL) {", "\n    luai_ddstep(L, (size_t)(p - news) + 2);")
        before(str, "  luaL_addlstring(b, news, l);\n}", "  luai_ddstep(L, l);\n")
        -- upper/lower/reverse：结果一次分配好，循环里不再分配，分配器的检查管不到；每 64 KB 计一次
        for _, body in ipairs({ "p[i] = s[l - i - 1];", "p[i] = tolower(uchar(s[i]));", "p[i] = toupper(uchar(s[i]));" }) do
            local head = body:find("s[l - i - 1]", 1, true) and "  for (i = 0; i < l; i++)" or "  for (i=0; i<l; i++)"
            patch(str, head .. "\n    " .. body,
                head .. " {\n    if ((i & 0xFFFF) == 0) luai_ddstep(L, 0x10000);\n    " .. body .. "\n  }")
        end
        -- string.byte / string.char：一次最多 100 万个（栈上限），反复调用时没有分配
        patch(str, "  for (i=0; i<n; i++)\n    lua_pushinteger(L, uchar(s[posi+i-1]));",
            "  for (i=0; i<n; i++) {\n    luai_ddstep(L, 1);\n    lua_pushinteger(L, uchar(s[posi+i-1]));\n  }")
        after(str, "    lua_Unsigned c = (lua_Unsigned)luaL_checkinteger(L, i);", "\n    luai_ddstep(L, 1);")
        -- string.format：每个字符计 1，每个格式项按它最多输出的字节计（%99.99f 一项要几微秒）；%q 每个字符计 1
        after(str, "  while (strfrmt < strfrmt_end) {", "\n    luai_ddstep(L, 1);")
        after(str, "      int nb = 0;  /* number of bytes in result */", "\n      luai_ddstep(L, MAX_ITEMF);")
        after(str, "  while (len--) {", "\n    luai_ddstep(b->L, 1);")
        io.writefile(str, io.readfile(str) .. "\n\n/* DD patch: see lddstep.h */\nvoid (*lua_ddstep_fn)(lua_State *L) = NULL;\n" ..
            "void (*lua_ddstep_nol_fn)(void) = NULL;\nsize_t lua_ddstep_work = 0;\n")

        -- ltablib.c
        local tab = "src/ltablib.c"
        after(tab, '#include "lualib.h"\n', '#include "lddstep.h"\n')
        after(tab, "      for (i = e; i > pos; i--) {  /* move up elements */", "\n        luai_ddstep(L, 1);")
        after(tab, "  for ( ; pos < size; pos++) {", "\n    luai_ddstep(L, 1);")
        after(tab, "      for (i = 0; i < n; i++) {", "\n        luai_ddstep(L, 1);")
        after(tab, "      for (i = n - 1; i >= 0; i--) {", "\n        luai_ddstep(L, 1);")
        after(tab, "  for (; i < last; i++) {", "\n    luai_ddstep(L, 1);")
        after(tab, "  while (lo < up) {  /* loop for tail recursion */", "\n    luai_ddstep(L, (size_t)(up - lo) + 1);")
        -- table.pack / table.unpack：一次最多 100 万个（栈上限），反复调用时没有分配（dd_patch 4）
        patch(tab, "  for (i = n; i >= 1; i--)  /* assign elements */\n    lua_seti(L, 1, i);",
            "  for (i = n; i >= 1; i--) {  /* assign elements */\n    luai_ddstep(L, 1);\n    lua_seti(L, 1, i);\n  }")
        after(tab, "  for (; i < e; i++) {  /* push arg[i..e - 1] (to avoid overflows) */", "\n    luai_ddstep(L, 1);")
        -- table.sort 的每次比较：没有比较函数时比较在 C 里进行（长串的 l_strcmp 用 nol 计步），不经过计数 hook，
        -- 这里给 nol 发现的超时一个报错的机会
        after(tab, "static int sort_comp (lua_State *L, int a, int b) {", "\n  luai_ddstep(L, 1);")

        -- lutf8lib.c（dd_patch 4）：utf8.len / codepoint / char 每个字符计 1；跳过后续字节的循环（offset、codes 的迭代函数）
        -- 都经过 iscontp，在宏里计步（用到它的函数都有 L）
        local utf = "src/lutf8lib.c"
        after(utf, '#include "lualib.h"\n', '#include "lddstep.h"\n')
        patch(utf, "#define iscontp(p)\tiscont(*(p))", "#define iscontp(p)\t(luai_ddstep_e(L, 1), iscont(*(p)))")
        after(utf, "    const char *s1 = utf8_decode(s + posi, NULL, !lax);", "\n    luai_ddstep(L, 1);")
        after(utf, "    s = utf8_decode(s, &code, !lax);", "\n    luai_ddstep(L, 1);")
        after(utf, "      pushutfchar(L, i);", "\n      luai_ddstep(L, 1);")

        -- lstring.c（dd_patch 4）：长串的比较（==、表的键）和哈希没有 lua_State，用 nol 版本。
        -- 两个等值的 32 MB 串反复比较，每次 memcmp 几毫秒，两次 hook 之间几百次。
        -- dd_patch 6：在操作之后报告（第三轮 R3A-备注1），跨过截止时间的那次比较、哈希当场发现
        local lstr = "src/lstring.c"
        after(lstr, '#include "lua.h"\n', '#include "lddstep.h"\n')
        patch(lstr, "  return (a == b) ||  /* same instance or... */\n" ..
            "    ((len == b->u.lnglen) &&  /* equal length and ... */\n" ..
            "     (memcmp(getlngstr(a), getlngstr(b), len) == 0));  /* equal contents */",
            "  if (a == b) return 1;  /* same instance */\n" ..
            "  if (len != b->u.lnglen) return 0;\n" ..
            "  {  /* DD patch: report the comparison after it is done */\n" ..
            "    int eq = (memcmp(getlngstr(a), getlngstr(b), len) == 0);\n" ..
            "    luai_ddstep_nol(len >> 4);\n" ..
            "    return eq;\n" ..
            "  }")
        after(lstr, "    ts->extra = 1;  /* now it has its hash */", "\n    luai_ddstep_nol(len);  /* DD patch: after the hash */")

        -- 这几个文件不在第二轮审核要求排查的范围里，排查时发现同样的问题（单次调用和串长成正比、反复调用时没有分配）：
        -- 字符串转数字（tonumber、算术和库函数参数的自动转换）先用 strpbrk 扫整个串；字符串的 < <=；tonumber(s, base)；
        -- next 跳过空槽
        local lobj = "src/lobject.c"
        after(lobj, '#include "lua.h"\n', '#include "lddstep.h"\n')
        -- dd_patch 6（第三轮 R3A-阻断2）：l_str2int 跳过任意多的空白、十六进制数字、前导零，成功时根本不走 l_str2d。
        -- 把整个 luaO_str2num 包起来，转换之后按串长报告（l_str2int、l_str2d 的 strpbrk 和 strtod 都在里面）
        patch(lobj, "size_t luaO_str2num (const char *s, TValue *o) {", "static size_t l_str2num (const char *s, TValue *o) {")
        after(lobj, "  return (e - s) + 1;  /* success; return string size */\n}\n",
            "\n\n/* DD patch: the conversion scans the whole string (spaces, digits, strpbrk, strtod) without\n" ..
            "** a lua_State; report it after it is done */\n" ..
            "size_t luaO_str2num (const char *s, TValue *o) {\n" ..
            "  size_t r = l_str2num(s, o);\n" ..
            "  luai_ddstep_nol(r != 0 ? r : strlen(s));\n" ..
            "  return r;\n" ..
            "}\n")
        -- l_strcmp（<、<=、table.sort）：每遇到一个 \0 调用一次 strcoll 和两次 strlen（第三轮 R3A-应修2）。DDLua 的 CRT 一直是
        -- C locale（沙箱没有 os.setlocale），strcoll 就是 strcmp，逐段比较和 memcmp 加长度比较的结果相同
        local lvm = "src/lvm.c"
        after(lvm, '#include "lua.h"\n', '#include "lddstep.h"\n')
        patch(lvm, [[
  const char *s1 = getstr(ts1);
  size_t rl1 = tsslen(ts1);  /* real length */
  const char *s2 = getstr(ts2);
  size_t rl2 = tsslen(ts2);
  for (;;) {  /* for each segment */
    int temp = strcoll(s1, s2);
    if (temp != 0)  /* not equal? */
      return temp;  /* done */
    else {  /* strings are equal up to a '\0' */
      size_t zl1 = strlen(s1);  /* index of first '\0' in 's1' */
      size_t zl2 = strlen(s2);  /* index of first '\0' in 's2' */
      if (zl2 == rl2)  /* 's2' is finished? */
        return (zl1 == rl1) ? 0 : 1;  /* check 's1' */
      else if (zl1 == rl1)  /* 's1' is finished? */
        return -1;  /* 's1' is less than 's2' ('s2' is not finished) */
      /* both strings longer than 'zl'; go on comparing after the '\0' */
      zl1++; zl2++;
      s1 += zl1; rl1 -= zl1; s2 += zl2; rl2 -= zl2;
    }
  }
}]], [[
  /* DD patch: byte order (the "C" locale of DDLua's CRT, where strcoll is strcmp); the same
  ** result as comparing segment by segment, without strcoll + 2 strlen per '\0' */
  const char *s1 = getstr(ts1);
  size_t rl1 = tsslen(ts1);  /* real length */
  const char *s2 = getstr(ts2);
  size_t rl2 = tsslen(ts2);
  size_t n = (rl1 < rl2) ? rl1 : rl2;
  int temp = memcmp(s1, s2, n);
  luai_ddstep_nol(n >> 4);  /* after the comparison */
  if (temp != 0)
    return temp;
  return (rl1 == rl2) ? 0 : (rl1 < rl2) ? -1 : 1;
}]])
        local lbase = "src/lbaselib.c"
        after(lbase, '#include "lualib.h"\n', '#include "lddstep.h"\n')
        patch(lbase, "static const char *b_str2int (const char *s, int base, lua_Integer *pn) {",
            "static const char *b_str2int (lua_State *L, const char *s, int base, lua_Integer *pn) {")
        patch(lbase, "b_str2int(s, (int)base, &n)", "b_str2int(L, s, (int)base, &n)")
        after(lbase, "    n = n * base + digit;", "\n    luai_ddstep(L, 1);")
        -- dd_patch 6：前后的空白用 strspn 一次跳过，也计步
        patch(lbase, "  s += strspn(s, SPACECHARS);  /* skip initial spaces */",
            "  { size_t ddw = strspn(s, SPACECHARS); luai_ddstep(L, ddw); s += ddw; }  /* skip initial spaces */")
        patch(lbase, "  s += strspn(s, SPACECHARS);  /* skip trailing spaces */",
            "  { size_t ddw = strspn(s, SPACECHARS); luai_ddstep(L, ddw); s += ddw; }  /* skip trailing spaces */")
        local ltab = "src/ltable.c"
        after(ltab, '#include "lua.h"\n', '#include "lddstep.h"\n')
        after(ltab, "  for (; i < asize; i++) {  /* try first array part */", "\n    luai_ddstep(L, 1);")
        after(ltab, "  for (i -= asize; cast_int(i) < sizenode(t); i++) {  /* hash part */", "\n    luai_ddstep(L, 1);")
        -- dd_patch 6（第三轮 R3A-阻断1）：整数、浮点数、指针键的哈希加密钥。原来整数键直接对 (sizenode-1)|1 取模，浮点键只看
        -- 指数和尾数的高 31 位，Mod 能算出全部落在同一个主位置的键（65537 个 i*(2^17-1) 一次扩容 3.8 秒）。
        -- 密钥 lua_ddhashkey 由 DDLua 每个进程随机生成一次，再和表自己的地址混合（每张表一个哈希函数，一张表上观察到的
        -- 遍历顺序用不到别的表上）。混合用 wyhash 的乘法折叠（64x64->128 位乘法，高低两半异或）
        patch(ltab, "#define hashpointer(t,p)\thashmod(t, point2uint(p))", [[
#include <string.h>

/* DD patch (xmake/lua.lua dd_patch 6): keyed hash for integer, float and pointer keys */
unsigned long long lua_ddhashkey[3] = {
  0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL, 0x8ebc6af09c88c6e3ULL
};

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
static unsigned long long dd_mum (unsigned long long a, unsigned long long b) {
  unsigned long long hi;
  unsigned long long lo = _umul128(a, b, &hi);
  return lo ^ hi;
}
#else
static unsigned long long dd_mum (unsigned long long a, unsigned long long b) {
  unsigned long long ha = a >> 32, hb = b >> 32;
  unsigned long long la = a & 0xffffffffULL, lb = b & 0xffffffffULL;
  unsigned long long rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
  unsigned long long t = rl + (rm0 << 32), c = t < rl;
  unsigned long long lo = t + (rm1 << 32);
  c += lo < t;
  return lo ^ (rh + (rm0 >> 32) + (rm1 >> 32) + c);
}
#endif

static Node *dd_hashpos (const Table *t, unsigned long long x) {
  /* the table's own key: its address (fixed for its whole life) mixed with the process key */
  unsigned long long tk = dd_mum((unsigned long long)(size_t)t ^ lua_ddhashkey[0], lua_ddhashkey[1]);
  unsigned long long h = dd_mum(x ^ lua_ddhashkey[1], ((x >> 32) | (x << 32)) ^ tk);
  h = dd_mum(h ^ lua_ddhashkey[2], tk ^ lua_ddhashkey[0]);
  return gnode(t, lmod(h, sizenode(t)));
}

static Node *dd_hashfloat (const Table *t, lua_Number n) {
  unsigned long long b = 0;
  /* float keys are never integral (normalized to integers), NaN or -0, so equal keys have equal bits */
  memcpy(&b, &n, sizeof(n) < sizeof(b) ? sizeof(n) : sizeof(b));
  return dd_hashpos(t, b ^ 0x9e3779b97f4a7c15ULL);
}

#define hashpointer(t,p)	dd_hashpos(t, (unsigned long long)(size_t)(p))]])
        patch(ltab, "  lua_Unsigned ui = l_castS2U(i);\n  if (ui <= cast_uint(INT_MAX))\n    return hashmod(t, cast_int(ui));\n" ..
            "  else\n    return hashmod(t, ui);\n}",
            "  return dd_hashpos(t, (unsigned long long)l_castS2U(i));  /* DD patch */\n}")
        patch(ltab, "      return hashmod(t, l_hashfloat(n));", "      return dd_hashfloat(t, n);  /* DD patch */")
        -- reinsert：旧表的键互不相同，直接插入（数组范围内的整数键放进数组部分），不先查找。原来逐个 luaH_set，每次都把冲突链
        -- 从头走一遍，n 个冲突键的扩容是 n²/2 次比较
        before(ltab, "static void reinsert (lua_State *L, Table *ot, Table *t) {",
            "static void luaH_newkey (lua_State *L, Table *t, const TValue *key, TValue *value);\n\n")
        patch(ltab, "      getnodekey(L, &k, old);\n      luaH_set(L, t, &k, gval(old));",
            "      getnodekey(L, &k, old);\n" ..
            "      /* DD patch: the keys of 'ot' are distinct, insert without searching */\n" ..
            "      if (ttisinteger(&k) && l_castS2U(ivalue(&k)) - 1u < t->alimit) {  /* array part? */\n" ..
            "        setobj2t(L, &t->array[ivalue(&k) - 1], gval(old));\n" ..
            "      }\n" ..
            "      else\n" ..
            "        luaH_newkey(L, t, &k, gval(old));")

        -- lgc.c（dd_patch 4，第二轮 R2A-阻断2）：只有弱键的表按强引用表遍历，不进 ephemeron 列表
        local lgc = "src/lgc.c"
        patch(lgc, "    else if (!weakvalue)  /* strong values? */\n      traverseephemeron(g, h, 0);",
            "    else if (!weakvalue)  /* DD patch: weak keys only -> traversed as a strong table */\n      traversestrongtable(g, h);")
        -- dd_patch 8（核对审核应修 1）：下一轮的阈值交给 DDLua（压到内存上限以下，大堆也走增量 GC）；原子阶段前后通知 DDLua 计时
        after(lgc, '#include "lua.h"\n', '#include "lddstep.h"\n')
        patch(lgc, "  debt = gettotalbytes(g) - threshold;\n  if (debt > 0) debt = 0;",
            "  if (lua_ddgcthreshold_fn != NULL)  /* DD patch: DDLua keeps it below its memory limit */\n" ..
            "    threshold = cast(l_mem, lua_ddgcthreshold_fn(g->mainthread, cast(long long, g->GCestimate),\n" ..
            "                                                 cast(long long, threshold)));\n" ..
            "  debt = gettotalbytes(g) - threshold;\n  if (debt > 0) debt = 0;")
        patch(lgc, "      work = atomic(L);  /* work is what was traversed by 'atomic' */",
            "      if (lua_ddatomic_fn != NULL) lua_ddatomic_fn(L, 0);  /* DD patch: time the atomic phase */\n" ..
            "      work = atomic(L);  /* work is what was traversed by 'atomic' */\n" ..
            "      if (lua_ddatomic_fn != NULL) lua_ddatomic_fn(L, 1);")
        io.writefile(lgc, io.readfile(lgc) .. "\n\n/* DD patch: see lddstep.h */\n" ..
            "long long (*lua_ddgcthreshold_fn)(lua_State *L, long long estimate, long long threshold) = NULL;\n" ..
            "void (*lua_ddatomic_fn)(lua_State *L, int phase) = NULL;\n")

        -- lstate.c：被计数 hook 里的错误杀死的协程，allowhook 停在 0（luaD_hook 在 hook 返回后才恢复它）。
        -- lua_closethread 重置线程时恢复成 1，否则它的 __close 在没有 hook 的情况下运行，预算管不住
        after("src/lstate.c", "  L->status = LUA_OK;  /* so it can run __close metamethods */",
            "\n  L->allowhook = 1;  /* DD patch: thread may have died inside a hook */")

        -- dd_patch 6（第三轮 R3A-阻断3）：正在运行的线程。lua_resume 进入时报告协程、返回时（结束、让出、出错）报告恢复它的线程；
        -- lua_closethread 运行 <close> 时同样。DDLua 据此在没有 lua_State 的进度回调里对真正在运行的线程设 hook
        local ldo = "src/ldo.c"
        after(ldo, '#include "lua.h"\n', '#include "lddstep.h"\n')
        after(ldo, "  luai_userstateresume(L, nargs);",
            "\n  lua_State *ddprev = lua_ddswitch_fn ? lua_ddswitch_fn(L) : NULL;  /* DD patch */")
        after(ldo, "  status = precover(L, status);", "\n  if (ddprev != NULL) lua_ddswitch_fn(ddprev);  /* DD patch */")
        io.writefile(ldo, io.readfile(ldo) .. "\n\n/* DD patch: see lddstep.h */\nlua_State *(*lua_ddswitch_fn)(lua_State *L) = NULL;\n")
        local lstate = "src/lstate.c"
        after(lstate, '#include "lua.h"\n', '#include "lddstep.h"\n')
        patch(lstate, "  status = luaE_resetthread(L, L->status);",
            "  {  /* DD patch: the thread runs its __close metamethods */\n" ..
            "    lua_State *ddprev = lua_ddswitch_fn ? lua_ddswitch_fn(L) : NULL;\n" ..
            "    status = luaE_resetthread(L, L->status);\n" ..
            "    if (ddprev != NULL) lua_ddswitch_fn(ddprev);\n" ..
            "  }")
        -- 字符串哈希的种子：原来是时间、L 的地址、栈上变量的地址、lua_newstate 的地址，Mod 用 os.time、tostring(线程)、
        -- tostring(print) 能猜出大部分。改由 DDLua 用系统随机数提供
        patch(lstate, "  g->seed = luai_makeseed(L);", "  g->seed = lua_ddseed_fn ? lua_ddseed_fn() : luai_makeseed(L);  /* DD patch */")
        io.writefile(lstate, io.readfile(lstate) .. "\n\n/* DD patch: see lddstep.h */\nunsigned int (*lua_ddseed_fn)(void) = NULL;\n")

        -- lmem.c（第三轮 R3A-应修1）：内存到上限、而且都是活对象时，每次分配失败都做一次完整的紧急 GC（128 MB 约 170 ms）。
        -- 先问 DDLua 要不要做，做完仍然失败时告诉它
        local lmem = "src/lmem.c"
        after(lmem, '#include "lua.h"\n', '#include "lddstep.h"\n')
        patch(lmem, "  if (cantryagain(g)) {\n    luaC_fullgc(L, 1);  /* try to free some memory... */\n" ..
            "    return callfrealloc(g, block, osize, nsize);  /* try again */\n  }",
            "  if (cantryagain(g) && (lua_ddemergency_fn == NULL || lua_ddemergency_fn(L, 0))) {  /* DD patch */\n" ..
            "    void *nb;\n" ..
            "    luaC_fullgc(L, 1);  /* try to free some memory... */\n" ..
            "    nb = callfrealloc(g, block, osize, nsize);  /* try again */\n" ..
            "    if (lua_ddemergency_fn != NULL)  /* DD patch: tell how it went */\n" ..
            "      lua_ddemergency_fn(L, nb == NULL ? 1 : 2);\n" ..
            "    return nb;\n" ..
            "  }")
        io.writefile(lmem, io.readfile(lmem) .. "\n\n/* DD patch: see lddstep.h */\nint (*lua_ddemergency_fn)(lua_State *L, int phase) = NULL;\n")

        -- lmathlib.c（第三轮 R3A-应修2）：math.min/max 逐个比较全部参数（40 万个参数反复调用 486 ms）
        local lmath = "src/lmathlib.c"
        after(lmath, '#include "lualib.h"\n', '#include "lddstep.h"\n')
        before(lmath, "    if (lua_compare(L, i, imin, LUA_OPLT))", "    luai_ddstep(L, 1);  /* DD patch */\n")
        before(lmath, "    if (lua_compare(L, imax, i, LUA_OPLT))", "    luai_ddstep(L, 1);  /* DD patch */\n")

        -- luaconf.h
        patch("src/luaconf.h", "#define LUA_IDSIZE\t60", "#define LUA_IDSIZE\t160")

        io.writefile("xmake.lua", [[
            add_rules("mode.release", "mode.debug")
            target("lua")
                set_kind("static")
                add_files("src/*.c|lua.c|luac.c|onelua.c")
                -- 按 C++ 编译，lua_error 走 C++ 异常
                add_cflags("/TP", "/EHsc", { force = true })
                add_headerfiles("src/lua.h", "src/luaconf.h", "src/lualib.h", "src/lauxlib.h", "src/lddstep.h", { prefixdir = "lua" })
        ]])
        import("package.tools.xmake").install(package, { kind = "static" })
    end)

    on_test(function (package)
        assert(package:has_cxxfuncs("luaL_newstate", { includes = "lauxlib.h" }))
    end)
package_end()

local lua_runtime = is_mode("debug") and "MTd" or "MT"
add_requires("lua 5.4.9", { configs = { runtimes = lua_runtime } })
-- sol2 只取头文件，Lua 由上面的本地包提供
add_requires("sol2 3.5.0", { configs = { includes_lua = false } })
