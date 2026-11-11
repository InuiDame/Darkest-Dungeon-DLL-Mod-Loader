// DD 补丁回调的最简单的桩（官方测试集用）。
// - lua_ddstep_fn / lua_ddstep_nol_fn：只计数，不检查预算
// - lua_ddswitch_fn：记下正在运行的线程，并检查进出成对（退出时报告的线程应当是进入时返回的那个）
// - lua_ddemergency_fn：phase 0 一律返回 1（照原版做紧急 GC），只计数
// - lua_ddseed_fn：系统随机数；lua_ddhashkey：进程启动时换成系统随机数
// - lua_ddatomic_fn（dd_patch 8）：计数，检查前后成对
// - lua_ddgcthreshold_fn（dd_patch 8）：默认原样返回；设了 DDSTUB_GCLIMIT=<字节> 时按 DDLua 的规则
//   min(阈值, max(上限的 7/8, 活对象 + 上限的 1/16)) 压低，让测试集走到改过的 setpause
// 设置环境变量 DDSTUB_REPORT=<文件> 时，进程退出时把计数追加到那个文件（不写 stdout/stderr，main.lua 会比较输出）
#include <lua.h>
#include <lddstep.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_sentinel;
lua_State* const kOutside = reinterpret_cast<lua_State*>(&g_sentinel);  // 「主线程/最外层」的占位
lua_State* g_running = kOutside;
std::vector<lua_State*>* g_stack = new std::vector<lua_State*>();
unsigned long long g_enter = 0, g_leave = 0, g_mismatch = 0;
unsigned long long g_steps = 0, g_nolsteps = 0;
unsigned long long g_emerg[3] = { 0, 0, 0 };
unsigned long long g_atomic = 0, g_atomicBad = 0, g_thresholdCalls = 0, g_thresholdLowered = 0;
int g_inAtomic = 0;
long long g_gcLimit = 0;

unsigned long long Random64()
{
    std::random_device device;
    return (static_cast<unsigned long long>(device()) << 32) ^ device();
}

lua_State* Switch(lua_State* L)
{
    lua_State* const previous = g_running;
    if (!g_stack->empty() && g_stack->back() == L) {  // 退出：报告回进入时返回的线程
        g_stack->pop_back();
        ++g_leave;
    } else {
        // 进入：L 开始运行。L 不可能是栈顶（恢复自己的恢复方在 resume_error 里就返回了，不经过通知）
        g_stack->push_back(previous);
        ++g_enter;
    }
    g_running = L;
    return previous;
}

int Emergency(lua_State*, int phase)
{
    if (phase >= 0 && phase <= 2) {
        ++g_emerg[phase];
    } else {
        ++g_mismatch;
    }
    return 1;
}

void Atomic(lua_State*, int phase)
{
    if (phase == 0) {
        if (g_inAtomic) {
            ++g_atomicBad;
        }
        g_inAtomic = 1;
        ++g_atomic;
    } else {
        if (!g_inAtomic) {
            ++g_atomicBad;
        }
        g_inAtomic = 0;
    }
}

long long Threshold(lua_State*, long long estimate, long long threshold)
{
    ++g_thresholdCalls;
    if (g_gcLimit <= 0) {
        return threshold;
    }
    long long cap = g_gcLimit - g_gcLimit / 8;
    if (estimate + g_gcLimit / 16 > cap) {
        cap = estimate + g_gcLimit / 16;
    }
    if (cap < threshold) {
        ++g_thresholdLowered;
        return cap;
    }
    return threshold;
}

unsigned int Seed()
{
    const unsigned long long v = Random64();
    return static_cast<unsigned int>(v ^ (v >> 32));
}

void Step(lua_State*) { ++g_steps; }
void StepNol() { ++g_nolsteps; }

void Report()
{
    const char* path = std::getenv("DDSTUB_REPORT");
    if (!path || !*path) {
        return;
    }
    if (FILE* f = std::fopen(path, "a")) {
        std::fprintf(f,
            "enter %llu leave %llu unbalanced-at-exit %zu mismatch %llu steps %llu nolsteps %llu emerg %llu/%llu/%llu "
            "atomic %llu bad %llu in-atomic-at-exit %d threshold %llu lowered %llu (limit %lld)\n",
            g_enter, g_leave, g_stack->size(), g_mismatch, g_steps, g_nolsteps, g_emerg[0], g_emerg[1], g_emerg[2], g_atomic,
            g_atomicBad, g_inAtomic, g_thresholdCalls, g_thresholdLowered, g_gcLimit);
        std::fclose(f);
    }
}

const int g_init = [] {
    for (auto& k : lua_ddhashkey) {
        unsigned long long v = 0;
        while (v == 0) {
            v = Random64();
        }
        k = v;
    }
    lua_ddstep_fn = &Step;
    lua_ddstep_nol_fn = &StepNol;
    lua_ddswitch_fn = &Switch;
    lua_ddemergency_fn = &Emergency;
    lua_ddseed_fn = &Seed;
    lua_ddatomic_fn = &Atomic;
    lua_ddgcthreshold_fn = &Threshold;
    if (const char* lim = std::getenv("DDSTUB_GCLIMIT")) {
        g_gcLimit = std::atoll(lim);
    }
    std::atexit(&Report);
    return 0;
}();

}  // namespace
