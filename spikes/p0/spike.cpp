// DDSpike：P0 一次性验证插件，不进发布。
// 按 RVA 硬编码地址，SHA1 由加载器的早期 hook 在 DllMain 里校验（9db2d73b...）。
//
// 验证项见 docs/04-roadmap.md 和 knowledge/dd1/events.md：
//   S1 早期时机、S2 通用事件 listener、S3 压缩后继续工作、S4 虚表 hook 共存、S5 CRT 堆。
//
// 运行方式：
//   DD_SPIKE_AUTOQUIT=<秒>  自检：到点写报告后退出进程（只停留在主菜单）。
//   不设置                  完整：不退出，定期把计数和结论刷到日志，由用户走完战斗流程。
// 日志：<游戏根目录>\DDModLoader\log\DDSpike.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <malloc.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------- 地址（RVA，基准 exe SHA1 9db2d73b89aa034e2055df65a139403e18df641f） ----------
constexpr std::uintptr_t kRvaEventManager   = 0x118E028;  // EventManager*
constexpr std::uintptr_t kRvaEventTypeCount = 0x118E020;  // 已注册的 Event 类型数
constexpr std::uintptr_t kRvaClassNames     = 0x2C44360;  // 类名表，每项 0x80 字节，下标 type_id
constexpr std::uintptr_t kClassNameStride   = 0x80;
constexpr std::uintptr_t kRvaAddListener    = 0x3619D0;   // (ListenerListData*, BaseListener*)
constexpr std::uintptr_t kRvaUpdateLogic    = 0x3E1AD0;   // 每帧逻辑，主循环唯一调用点在 3D2C79
constexpr std::uintptr_t kRvaCompactList    = 0x3CB7F0;   // ListenerListData 压缩：移除 active==0 的表项，派发结束后调用
constexpr std::uintptr_t kRvaCampaignCurrent = 0x118E138;  // Campaign::Current 的全局指针 g_pCurrent（函数 556520）
constexpr std::size_t    kActorSpriteOffset = 0x10C0;     // Actor 的主 SpineSprite*（静态提取，运行时记录非空情况）
constexpr std::uintptr_t kRvaHeroVtable     = 0xE8A288;
constexpr std::uintptr_t kRvaMonsterVtable  = 0xEA9480;
constexpr std::uintptr_t kRvaActorDamage    = 0x4836A0;   // Hero 槽位 50 的原实现
constexpr std::uintptr_t kRvaMonsterDamage  = 0x5FC3E0;   // Monster 槽位 50 的原实现
constexpr int            kSlotApplyDamage   = 50;

// 列表表项布局（EventListenerDataContainer），由 3619D0/361730 的反汇编确认
constexpr std::size_t kEntrySize     = 0x28;
constexpr std::size_t kEntryListener = 0x08;
constexpr std::size_t kEntryActive   = 0x10;

// ListenerListData（0x20 字节），EventManager+0 是它的 vector，下标 type_id
struct ListenerListData {
    std::uint8_t* first;
    std::uint8_t* last;
    std::uint8_t* end;
    std::uint8_t  dirty;
    std::uint8_t  pad[7];
};

// Listener<T const> 的对象布局（0x50 字节）：
//   +0x00 vtable
//   +0x08 int priority（列表排序键，由比较函数 3CD620 读 [listener+8] 确认）
//   +0x10 std::function，实现指针在 +0x48
struct FakeListener {
    void*         vtbl;
    std::int32_t  priority;
    std::int32_t  pad0;
    std::uint8_t  funcStorage[0x38];
    void*         funcImpl;
};

// std::_Func_base<ListenerResponse, T const*> 兼容对象。
// 虚表槽位顺序（由 _Func_impl_no_alloc 的虚表 E9A4D8 等确认）：
//   0 _Copy  1 _Move  2 _Do_call  3 _Target_type  4 _Delete_this  5 _Get
// _Do_call(T const*&&)：rdx 是指向事件指针的指针（3B033A 处的调用形式）。
// 返回 ListenerResponse：0 继续派发，1 中止后续派发（3B033D 处 cmp eax,1）。
struct TypeState;

struct FakeImpl {
    void* const* vtbl;
    TypeState*   owner;
    bool         secondary;  // false=A（常驻），true=B（S3 用于注销）
};

struct TypeState {
    int            id = -1;
    std::string    tableName;
    void*          listenerVtable = nullptr;
    FakeImpl       implA{};
    FakeImpl       implB{};
    FakeListener   listenerA{};
    FakeListener   listenerB{};
    bool           registered = false;

    volatile LONG64 countA = 0;
    volatile LONG64 countB = 0;
    bool           firstLogged = false;
    std::string    rttiName;
    bool           rttiMatchesTable = false;
    DWORD          firstThread = 0;
    int            responseChecks = 0;

    // S3
    bool           bDeactivated = false;
    LONG64         countBAtDeact = 0;
    LONG64         countAAtDeact = 0;
    bool           compactionSeen = false;
    LONG64         countAAtCompact = 0;
    bool           compactedThenAlive = false;
    bool           bStayedSilent = true;
};

// ---------- 全局状态 ----------
HMODULE              g_self = nullptr;
std::uint8_t*        g_exe = nullptr;
std::size_t          g_exeSize = 0;
HANDLE               g_log = INVALID_HANDLE_VALUE;
CRITICAL_SECTION     g_cs;
ULONGLONG            g_startTick = 0;
ULONGLONG            g_lastReport = 0;
DWORD                g_autoQuitSec = 0;
bool                 g_stopping = false;
std::vector<TypeState>* g_types = nullptr;
int                  g_typeCount = 0;

// S1
DWORD                g_tidBefore = 0;
void*                g_emBefore = nullptr;
DWORD                g_tidAfter = 0;
void*                g_emAfter = nullptr;
DWORD                g_tidTick = 0;
volatile LONG64      g_frames = 0;

// S3 状态
ULONGLONG            g_deactTick = 0;
bool                 g_deactDone = false;

// S4
using ApplyDamageFn = void* (*)(void*, void*, float, const int*, unsigned, const int*, bool, void*);
ApplyDamageFn        g_origHero = nullptr;
ApplyDamageFn        g_origMonster = nullptr;
volatile LONG64      g_heroHits = 0;
volatile LONG64      g_monsterHits = 0;
bool                 g_s4Installed = false;
bool                 g_heroFirstLogged = false;
bool                 g_monsterFirstLogged = false;

// S5 结果文本
std::string          g_s5;

// ---------- 日志 ----------
void LogRaw(const char* s, int n) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(g_log, s, static_cast<DWORD>(n), &w, nullptr);
}

void Logf(const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n = (std::min)(n + m, static_cast<int>(sizeof(line)) - 3);
    line[n++] = '\r';
    line[n++] = '\n';
    EnterCriticalSection(&g_cs);
    LogRaw(line, n);
    LeaveCriticalSection(&g_cs);
}

void LogOpen() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring root(path);
    for (int i = 0; i < 3; ++i) {                  // ...\mods\DDSpike\DDSpike.dll → 游戏根目录
        auto pos = root.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return;
        root.resize(pos);
    }
    std::wstring file = root + L"\\DDModLoader\\log\\DDSpike.log";
    g_log = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log != INVALID_HANDLE_VALUE) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        LogRaw(reinterpret_cast<const char*>(bom), 3);
    }
}

// ---------- 工具 ----------
std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(g_exe); }

template <class T> T Rva(std::uintptr_t rva) { return reinterpret_cast<T>(Base() + rva); }

// 把 .?AVName@Ns@@ 还原成 Ns::Name
std::string DemangleClass(const char* deco) {
    if (std::strncmp(deco, ".?A", 3) != 0) return deco;
    const char* p = deco + 4;  // 跳过 .?AV / .?AU
    std::vector<std::string> parts;
    std::string cur;
    for (;; ++p) {
        if (*p == '@' || *p == 0) {
            if (cur.empty()) break;
            parts.push_back(cur);
            cur.clear();
            if (*p == 0) break;
            if (p[1] == '@') break;
        } else {
            cur.push_back(*p);
        }
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out.empty() ? deco : out;
}

// 读对象虚表 → COL → 类型描述符名（SEH 保护，函数里不能有需要析构的对象）
const char* RttiNamePtr(const void* object) {
    __try {
        auto vtbl = *reinterpret_cast<void* const* const*>(object);
        auto col = static_cast<const std::uint32_t*>(vtbl[-1]);
        const auto tdRva = static_cast<std::uintptr_t>(col[3]);
        return reinterpret_cast<const char*>(Base() + tdRva + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// 取一个多态对象的 RTTI 名（decorated），失败返回空串
std::string RttiNameOf(const void* object) {
    const char* p = RttiNamePtr(object);
    return p ? std::string(p) : std::string{};
}

// 表名和 RTTI 名是否一致：表里是不带命名空间的短名，RTTI 带命名空间，比较最后一段
bool NameMatches(const std::string& tableName, const std::string& rttiName) {
    auto lastColon = rttiName.rfind("::");
    std::string shortName = lastColon == std::string::npos ? rttiName : rttiName.substr(lastColon + 2);
    return shortName == tableName || rttiName == tableName;
}

// ---------- _Func_base 兼容实现 ----------
void*  __fastcall Impl_Copy(FakeImpl* self, void*) { return self; }
void*  __fastcall Impl_Move(FakeImpl* self, void*) { return self; }
void   __fastcall Impl_Delete(FakeImpl*, bool) {}
const void* __fastcall Impl_Target(FakeImpl*) {
    static const int kDummy = 0;
    return &kDummy;
}
const void* __fastcall Impl_Get(FakeImpl* self) { return self + 1; }

// _Do_call：事件回调。这里不能抛 C++ 异常跨越游戏栈帧。
int __fastcall Impl_DoCall(FakeImpl* self, const void* const* ppEvent) {
    TypeState* type = self->owner;
    if (self->secondary) {
        InterlockedIncrement64(&type->countB);
        return 0;
    }

    const LONG64 count = InterlockedIncrement64(&type->countA);
    const void* event = *ppEvent;

    if (!type->firstLogged) {
        type->firstLogged = true;
        type->firstThread = GetCurrentThreadId();
        const std::string deco = RttiNameOf(event);
        type->rttiName = deco.empty() ? std::string{} : DemangleClass(deco.c_str());
        type->rttiMatchesTable = !type->rttiName.empty() && NameMatches(type->tableName, type->rttiName);
        Logf("[S2] 首次收到 type=%d 表名=%s RTTI=%s 一致=%s 线程=%lu 主线程一致=%s 地址=%p",
             type->id, type->tableName.c_str(), type->rttiName.c_str(),
             type->rttiMatchesTable ? "是" : "否",
             GetCurrentThreadId(), GetCurrentThreadId() == g_tidBefore ? "是" : "否", event);
    }

    // 压缩后第一次派发到 A：说明 listener 活过了排序和压缩
    if (type->compactionSeen && !type->compactedThenAlive && count > type->countAAtCompact) {
        type->compactedThenAlive = true;
        Logf("[S3] 压缩后再次收到 %s（A 累计 %lld，B 累计 %lld）",
             type->tableName.c_str(), static_cast<long long>(count), static_cast<long long>(type->countB));
    }

    return 0;
}

void* const kImplVtable[] = {
    reinterpret_cast<void*>(&Impl_Copy),
    reinterpret_cast<void*>(&Impl_Move),
    reinterpret_cast<void*>(&Impl_DoCall),
    reinterpret_cast<void*>(&Impl_Target),
    reinterpret_cast<void*>(&Impl_Delete),
    reinterpret_cast<void*>(&Impl_Get),
};

// ---------- PE 工具 ----------
struct Section { std::uint8_t* begin; std::uint8_t* end; };

bool GetSection(const char* name, Section& out) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exe + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char buf[9] = {};
        std::memcpy(buf, sec->Name, 8);
        if (std::strcmp(buf, name) == 0) {
            out.begin = g_exe + sec->VirtualAddress;
            out.end = out.begin + sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// 运行时按 RTTI 名查找 Listener<T const> 的虚表：在 .rdata 里找 vtable[-1] 指向的 COL，
// COL 的类型描述符名以 .?AV?$Listener@$$CBV 开头。键是 RTTI 名里 $$CBV 之后的第一段（类名）。
std::unordered_map<std::string, void*> FindListenerVtables() {
    std::unordered_map<std::string, void*> map;
    Section rdata{};
    if (!GetSection(".rdata", rdata)) return map;

    static const char kPrefix[] = ".?AV?$Listener@$$CBV";
    constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;

    for (auto* p = reinterpret_cast<std::uint64_t*>(rdata.begin);
         reinterpret_cast<std::uint8_t*>(p + 1) <= rdata.end; ++p) {
        const std::uintptr_t q = *p;
        const std::uintptr_t rdBegin = reinterpret_cast<std::uintptr_t>(rdata.begin);
        const std::uintptr_t rdEnd = reinterpret_cast<std::uintptr_t>(rdata.end);
        // 注意 q 可能是 -1 之类的任意值，q + 24 会溢出，所以用减法比较
        if (q < rdBegin || q >= rdEnd || rdEnd - q < 24) continue;
        auto col = reinterpret_cast<const std::uint32_t*>(q);
        if (col[0] != 1 || col[1] != 0) continue;                         // signature=1，offset=0
        if (static_cast<std::uintptr_t>(col[5]) != q - Base()) continue;  // pSelf 自校验
        const std::uintptr_t tdRva = col[3];
        if (tdRva < 0x1000 || tdRva + 0x10 + kPrefixLen >= g_exeSize) continue;  // 排除误判的 COL
        const char* name = reinterpret_cast<const char*>(Base() + tdRva + 0x10);
        if (std::memcmp(name, kPrefix, kPrefixLen) != 0) continue;
        const char* inner = name + kPrefixLen;
        const char* at = std::strchr(inner, '@');
        if (!at) continue;
        std::string key(inner, at - inner);
        // vtable[-1] 是 p，虚表起点是 p+1
        map.emplace(std::move(key), reinterpret_cast<void*>(p + 1));
    }
    return map;
}

// ---------- 列表操作 ----------
ListenerListData* ListOf(int typeId) {
    auto* em = *Rva<std::uint8_t**>(kRvaEventManager);
    if (!em) return nullptr;
    auto first = *reinterpret_cast<std::uint8_t**>(em);
    return reinterpret_cast<ListenerListData*>(first + static_cast<std::ptrdiff_t>(typeId) * sizeof(ListenerListData));
}

using AddListenerFn = void* (*)(void* list, void* listener);

bool EntryFind(ListenerListData* list, const void* listener) {
    for (auto* e = list->first; e != list->last; e += kEntrySize) {
        if (*reinterpret_cast<void**>(e + kEntryListener) == listener) return true;
    }
    return false;
}

// 按游戏的注销方式（析构函数 361730）：active 清零并置 dirty
int EntryDeactivate(ListenerListData* list, const void* listener) {
    int hits = 0;
    for (auto* e = list->first; e != list->last; e += kEntrySize) {
        if (*reinterpret_cast<void**>(e + kEntryListener) == listener) {
            e[kEntryActive] = 0;
            list->dirty = 1;
            ++hits;
        }
    }
    return hits;
}

// ---------- S2：注册通用 listener ----------
bool RegisterListeners() {
    const auto typeCount = *Rva<int*>(kRvaEventTypeCount);
    g_typeCount = typeCount;
    Logf("[S2] 类型数 %d（118E020），类名表 2C44360", typeCount);
    if (typeCount <= 0 || typeCount > 128) {
        Logf("[S2] 类型数不合法，放弃注册");
        return false;
    }

    auto vtables = FindListenerVtables();
    Logf("[S2] 运行时按 RTTI 找到 Listener<T const> 虚表 %zu 个", vtables.size());

    g_types = new std::vector<TypeState>(typeCount);
    auto addListener = Rva<AddListenerFn>(kRvaAddListener);

    int withVtable = 0, registeredCount = 0, withNamespace = 0;
    for (int i = 0; i < typeCount; ++i) {
        TypeState& t = (*g_types)[i];
        t.id = i;
        const char* raw = reinterpret_cast<const char*>(Base() + kRvaClassNames + i * kClassNameStride);
        t.tableName = (raw && raw[0]) ? std::string(raw) : std::string();
        if (t.tableName.find("::") != std::string::npos) ++withNamespace;

        std::string key = t.tableName;
        const auto colon = key.rfind("::");
        if (colon != std::string::npos) key = key.substr(colon + 2);

        auto it = vtables.find(key);
        if (it == vtables.end()) {
            Logf("[S2] type=%d %s：游戏里没有 Listener<T const> 虚表，不注册", i, t.tableName.c_str());
            continue;
        }
        t.listenerVtable = it->second;
        ++withVtable;

        t.implA.vtbl = kImplVtable;
        t.implA.owner = &t;
        t.implA.secondary = false;
        t.implB.vtbl = kImplVtable;
        t.implB.owner = &t;
        t.implB.secondary = true;

        std::memset(&t.listenerA, 0, sizeof(FakeListener));
        t.listenerA.vtbl = t.listenerVtable;
        t.listenerA.funcImpl = &t.implA;
        std::memset(&t.listenerB, 0, sizeof(FakeListener));
        t.listenerB.vtbl = t.listenerVtable;
        t.listenerB.funcImpl = &t.implB;

        ListenerListData* list = ListOf(i);
        if (!list) {
            Logf("[S2] EventManager 为空，无法注册");
            return false;
        }
        addListener(list, &t.listenerA);
        addListener(list, &t.listenerB);
        t.registered = true;
        ++registeredCount;
    }

    Logf("[S2] 类名是否带命名空间：表中含 :: 的项 %d 个（共 %d）", withNamespace, typeCount);
    for (int i = 0; i < typeCount; ++i) {
        const TypeState& t = (*g_types)[i];
        Logf("[S2] 事件表 #%d %s (vtable=%p registered=%s)", t.id, t.tableName.c_str(), t.listenerVtable, t.registered ? "是" : "否");
    }
    Logf("[S2] 注册完成：有虚表 %d 个，成功注册 %d 个（每个类型 A/B 两个 listener）", withVtable, registeredCount);
    return true;
}

// ---------- S3：注销 B 并观察压缩 ----------
void S3Deactivate() {
    int total = 0;
    for (auto& t : *g_types) {
        if (!t.registered) continue;
        ListenerListData* list = ListOf(t.id);
        t.countBAtDeact = t.countB;
        t.countAAtDeact = t.countA;
        t.bDeactivated = true;
        total += EntryDeactivate(list, &t.listenerB);
    }
    Logf("[S3] 已注销全部 B listener（%d 个表项被置为不活跃，dirty=1），等待下一次派发时排序和压缩", total);
}

void S3CheckCompaction() {
    for (auto& t : *g_types) {
        if (!t.bDeactivated || t.compactionSeen) continue;
        ListenerListData* list = ListOf(t.id);
        if (!list) continue;
        if (!EntryFind(list, &t.listenerB)) {
            t.compactionSeen = true;
            t.countAAtCompact = t.countA;
            Logf("[S3] %s 的 B 表项已被压缩移除（A 此时 %lld，list size=%lld）",
                 t.tableName.c_str(), static_cast<long long>(t.countA),
                 static_cast<long long>((list->last - list->first) / kEntrySize));
        }
    }
}

// S3 主菜单测试：LocalizationAvailableEvent 在 LoadAppAssets 里派发一次（紧跟 CreateAppSystems 之后）。
// 这里抢在它之前注销 B，并直接调用游戏的压缩函数 3CB7F0，之后 A 必须仍能收到这次派发。
void S3EarlyCompactionTest() {
    TypeState* target = nullptr;
    for (auto& t : *g_types) {
        if (t.registered && t.tableName == "LocalizationAvailableEvent") { target = &t; break; }
    }
    if (!target) {
        Logf("[S3] 主菜单测试：没有找到 LocalizationAvailableEvent，跳过");
        return;
    }
    static const std::uint8_t kCompactEntry[] = { 0x4C, 0x8B, 0x41, 0x08 };  // mov r8, [rcx+8]
    auto* compact = Rva<std::uint8_t*>(kRvaCompactList);
    if (std::memcmp(compact, kCompactEntry, sizeof(kCompactEntry)) != 0) {
        Logf("[S3] 主菜单测试：3CB7F0 入口字节不符，跳过");
        return;
    }
    ListenerListData* list = ListOf(target->id);
    const long long before = (list->last - list->first) / kEntrySize;
    const int hits = EntryDeactivate(list, &target->listenerB);
    target->bDeactivated = true;
    target->countBAtDeact = target->countB;
    target->countAAtDeact = target->countA;

    reinterpret_cast<void(__fastcall*)(void*)>(compact)(list);

    const long long after = (list->last - list->first) / kEntrySize;
    const bool bGone = !EntryFind(list, &target->listenerB);
    const bool aKept = EntryFind(list, &target->listenerA);
    target->compactionSeen = bGone;
    target->countAAtCompact = target->countA;
    Logf("[S3] 主菜单测试：%s 注销 B（命中 %d 个表项），调用 3CB7F0 压缩，size %lld -> %lld，B 已移除=%s，A 保留=%s，dirty=%d",
         target->tableName.c_str(), hits, before, after, bGone ? "是" : "否", aKept ? "是" : "否", list->dirty);
}

// ---------- S4：虚表 hook ----------
std::string ModuleOf(const void* address) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<const char*>(address), &mod) || !mod) {
        return "(无模块)";
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    char buf[MAX_PATH + 32];
    std::snprintf(buf, sizeof(buf), "%s+0x%llX", slash ? slash + 1 : path,
                  static_cast<unsigned long long>(reinterpret_cast<const std::uint8_t*>(address) - reinterpret_cast<const std::uint8_t*>(mod)));
    return buf;
}

// 读函数入口字节并跟随跳转链，返回访问到的地址（SEH 保护，函数里不能有需要析构的对象）。
// 返回 false 表示读取时发生异常。
bool FollowFlow(const void* entry, std::uint8_t bytes[8], const std::uint8_t* chain[4], int& depth) {
    __try {
        const auto* p = static_cast<const std::uint8_t*>(entry);
        std::memcpy(bytes, p, 8);
        const std::uint8_t* cur = p;
        depth = 0;
        for (; depth < 4; ++depth) {
            if (cur[0] == 0xE9) {
                cur = cur + 5 + *reinterpret_cast<const std::int32_t*>(cur + 1);
            } else if (cur[0] == 0xFF && cur[1] == 0x25) {
                auto rip = cur + 6 + *reinterpret_cast<const std::int32_t*>(cur + 2);
                cur = *reinterpret_cast<const std::uint8_t* const*>(rip);
            } else {
                break;
            }
            chain[depth] = cur;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 描述函数入口的跳转链，用来确认内联 hook 是否在位
std::string DescribeFlow(const void* entry) {
    std::uint8_t bytes[8] = {};
    const std::uint8_t* chain[4] = {};
    int depth = 0;
    if (!FollowFlow(entry, bytes, chain, depth)) return "(读取入口时异常)";

    char hex[64];
    std::snprintf(hex, sizeof(hex), "%02X %02X %02X %02X %02X %02X %02X %02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
    std::string out = "bytes=";
    out += hex;
    for (int i = 0; i < depth; ++i) {
        char line[64];
        std::snprintf(line, sizeof(line), " -> %p ", chain[i]);
        out += line;
        out += ModuleOf(chain[i]);
    }
    return out;
}

void* HookHeroApplyDamage(void* self, void* prng, float damage, const int* dt, unsigned src, const int* st, bool flag, void* attacker) {
    const LONG64 n = InterlockedIncrement64(&g_heroHits);
    if (!g_heroFirstLogged) {
        g_heroFirstLogged = true;
        Logf("[S4] Hero 虚表 hook 首次命中 #%lld，damage=%.2f，actor=%p，原实现入口 %s", static_cast<long long>(n), damage, self,
             DescribeFlow(reinterpret_cast<const void*>(g_origHero)).c_str());
        Logf("[D] Hero actor+0x10C0 (SpineSprite*) = %p", *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kActorSpriteOffset));
    }
    return g_origHero(self, prng, damage, dt, src, st, flag, attacker);
}

void* HookMonsterApplyDamage(void* self, void* prng, float damage, const int* dt, unsigned src, const int* st, bool flag, void* attacker) {
    const LONG64 n = InterlockedIncrement64(&g_monsterHits);
    if (!g_monsterFirstLogged) {
        g_monsterFirstLogged = true;
        Logf("[S4] Monster 虚表 hook 首次命中 #%lld，damage=%.2f，actor=%p，原实现入口 %s", static_cast<long long>(n), damage, self,
             DescribeFlow(reinterpret_cast<const void*>(g_origMonster)).c_str());
        Logf("[D] Monster actor+0x10C0 (SpineSprite*) = %p", *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kActorSpriteOffset));
    }
    return g_origMonster(self, prng, damage, dt, src, st, flag, attacker);
}

ApplyDamageFn PatchVtable(std::uintptr_t vtableRva, int slot, void* hook, void* expected, const char* name) {
    auto** slotAddr = reinterpret_cast<void**>(Base() + vtableRva + slot * sizeof(void*));
    void* current = *slotAddr;
    if (current != expected) {
        Logf("[S4] %s 槽位 %d 的值 %p 与期望 %p 不符，放弃", name, slot, current, expected);
        return nullptr;
    }
    DWORD old = 0;
    if (!VirtualProtect(slotAddr, sizeof(void*), PAGE_READWRITE, &old)) {
        Logf("[S4] VirtualProtect 失败");
        return nullptr;
    }
    *slotAddr = hook;
    VirtualProtect(slotAddr, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slotAddr, sizeof(void*));
    Logf("[S4] 已改 %s 虚表槽位 %d：%p -> %p", name, slot, current, hook);
    return reinterpret_cast<ApplyDamageFn>(current);
}

void InstallVtableHooks() {
    auto heroOrig = reinterpret_cast<void*>(Base() + kRvaActorDamage);
    auto monsterOrig = reinterpret_cast<void*>(Base() + kRvaMonsterDamage);
    g_origHero = PatchVtable(kRvaHeroVtable, kSlotApplyDamage, reinterpret_cast<void*>(&HookHeroApplyDamage), heroOrig, "Hero");
    g_origMonster = PatchVtable(kRvaMonsterVtable, kSlotApplyDamage, reinterpret_cast<void*>(&HookMonsterApplyDamage), monsterOrig, "Monster");
    g_s4Installed = g_origHero && g_origMonster;
    Logf("[S4] 虚表 hook %s", g_s4Installed ? "安装完成（Hero/Monster）" : "未完整安装");
    Logf("[S4] 安装时 Actor::ApplyHealthDamageInternal 入口：%s", DescribeFlow(heroOrig).c_str());
    Logf("[S4] 安装时 Monster::ApplyHealthDamageInternal 入口：%s", DescribeFlow(monsterOrig).c_str());
}

// ---------- S5：CRT 堆 ----------
void RunS5() {
    intptr_t pluginHeap = _get_heap_handle();
    HANDLE processHeap = GetProcessHeap();
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    intptr_t ucrtHeap = 0;
    using MallocFn = void* (__cdecl*)(std::size_t);
    using FreeFn = void(__cdecl*)(void*);
    MallocFn ucrtMalloc = nullptr;
    FreeFn ucrtFree = nullptr;
    if (ucrt) {
        auto getHeap = reinterpret_cast<intptr_t(__cdecl*)()>(GetProcAddress(ucrt, "_get_heap_handle"));
        if (getHeap) ucrtHeap = getHeap();
        ucrtMalloc = reinterpret_cast<MallocFn>(GetProcAddress(ucrt, "malloc"));
        ucrtFree = reinterpret_cast<FreeFn>(GetProcAddress(ucrt, "free"));
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "插件 _get_heap_handle=%p ucrtbase _get_heap_handle=%p GetProcessHeap()=%p ucrtbase.dll=%p",
                  reinterpret_cast<void*>(pluginHeap), reinterpret_cast<void*>(ucrtHeap), processHeap, ucrt);
    g_s5 = buf;
    Logf("[S5] %s", buf);

    const bool sameHeap = ucrt && pluginHeap == ucrtHeap && ucrtHeap == reinterpret_cast<intptr_t>(processHeap);
    Logf("[S5] 三个堆句柄 %s", sameHeap ? "相同" : "不全相同");
    if (!sameHeap || !ucrtMalloc || !ucrtFree) {
        Logf("[S5] 句柄不一致，跳过跨 CRT 释放实测（不安全）");
        g_s5 += "；句柄不一致，未实测跨 CRT 释放";
        return;
    }

    // 实测：ucrtbase!malloc → 插件 free；插件 malloc → ucrtbase!free。
    __try {
        void* p1 = ucrtMalloc(256);
        if (!p1) { Logf("[S5] ucrtbase malloc 返回空"); return; }
        std::memset(p1, 0xA5, 256);
        free(p1);   // 插件静态 CRT 的 free
        BOOL ok1 = HeapValidate(processHeap, 0, nullptr);
        Logf("[S5] ucrtbase!malloc → 插件 free 完成，HeapValidate=%d", ok1);

        void* p2 = malloc(256);
        if (!p2) { Logf("[S5] 插件 malloc 返回空"); return; }
        std::memset(p2, 0x5A, 256);
        ucrtFree(p2);  // ucrtbase 的 free
        BOOL ok2 = HeapValidate(processHeap, 0, nullptr);
        Logf("[S5] 插件 malloc → ucrtbase!free 完成，HeapValidate=%d", ok2);

        g_s5 += (ok1 && ok2) ? "；跨 CRT 分配释放实测成功，堆校验通过" : "；跨 CRT 分配释放后 HeapValidate 失败";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("[S5] 跨 CRT 释放触发异常 %08X", GetExceptionCode());
        g_s5 += "；跨 CRT 释放触发异常";
    }
}

// SEH 包装：spike 内部故障不能影响游戏
EXCEPTION_RECORD g_lastException{};

int RecordException(EXCEPTION_POINTERS* info) {
    g_lastException = *info->ExceptionRecord;
    return EXCEPTION_EXECUTE_HANDLER;
}

template <class F>
void Guarded(const char* what, F&& f) {
    __try {
        f();
    } __except (RecordException(GetExceptionInformation())) {
        Logf("[spike] %s 触发异常 %08X，地址 %p，访问类型 %llu，目标 %p，跳过",
             what, g_lastException.ExceptionCode, g_lastException.ExceptionAddress,
             static_cast<unsigned long long>(g_lastException.ExceptionInformation[0]),
             reinterpret_cast<void*>(g_lastException.ExceptionInformation[1]));
    }
}

// ---------- 报告 ----------
void WriteReport(const char* title) {
    Logf("================ %s ================", title);
    ULONGLONG now = GetTickCount64();
    Logf("[报告] 运行 %.1f 秒，帧 %lld", (now - g_startTick) / 1000.0, static_cast<long long>(g_frames));

    const bool threadSame = g_tidBefore != 0 && g_tidBefore == g_tidAfter && g_tidBefore == g_tidTick;
    Logf("[S1] 入口线程=%lu 返回线程=%lu 主循环(首帧tick)线程=%lu 一致=%s；进入前 EventManager=%p 返回后=%p → %s",
         g_tidBefore, g_tidAfter, g_tidTick, threadSame ? "是" : "否", g_emBefore, g_emAfter,
         (threadSame && !g_emBefore && g_emAfter) ? "S1 已证实" : "S1 未证实");

    if (g_types) {
        int fired = 0, firstMatchFail = 0, listened = 0;
        long long totalA = 0;
        for (const auto& t : *g_types) {
            if (t.registered) ++listened;
            if (t.countA > 0) {
                ++fired;
                totalA += t.countA;
                if (!t.rttiMatchesTable) ++firstMatchFail;
            }
        }
        Logf("[S2] 已注册 %d/%d 类型；收到事件的类型 %d 个，事件总数 %lld；RTTI 名与类名表不一致 %d 个",
             listened, g_typeCount, fired, totalA, firstMatchFail);
        for (const auto& t : *g_types) {
            if (t.countA > 0 || t.countB > 0) {
                Logf("[S2] 计数 #%d %-40s A=%lld B=%lld", t.id, t.tableName.c_str(),
                     static_cast<long long>(t.countA), static_cast<long long>(t.countB));
            }
        }

        int compacted = 0, aliveAfter = 0, bSilent = 0, bLoud = 0, noDispatch = 0;
        for (const auto& t : *g_types) {
            if (!t.bDeactivated) continue;
            if (t.compactionSeen) {
                ++compacted;
                if (t.compactedThenAlive) ++aliveAfter;
            }
            // A 没再收到事件说明注销后没有派发，这类类型证明不了 B 是否被跳过
            if (t.countA <= t.countAAtDeact) ++noDispatch;
            else if (t.countB > t.countBAtDeact) ++bLoud;
            else ++bSilent;
        }
        Logf("[S3] 已注销 B：压缩观察到 %d 个类型，压缩后 A 继续收到事件 %d 个；注销后有派发的类型中 B 未被调用 %d 个、仍被调用 %d 个；注销后没有派发的类型 %d 个（不计入结论）",
             compacted, aliveAfter, bSilent, bLoud, noDispatch);
    }

    Logf("[S4] Hero 虚表 hook 命中 %lld，Monster 虚表 hook 命中 %lld，安装=%s",
         static_cast<long long>(g_heroHits), static_cast<long long>(g_monsterHits), g_s4Installed ? "是" : "否");
    Logf("[S4] 现在 Actor::ApplyHealthDamageInternal 入口：%s", DescribeFlow(reinterpret_cast<void*>(Base() + kRvaActorDamage)).c_str());
    Logf("[S4] 现在 Monster::ApplyHealthDamageInternal 入口：%s", DescribeFlow(reinterpret_cast<void*>(Base() + kRvaMonsterDamage)).c_str());

    Logf("[S5] %s", g_s5.c_str());

    // 结论摘要：各项是否已证实，未证实的说明原因
    {
        const bool s1 = g_tidBefore != 0 && g_tidBefore == g_tidAfter && g_tidBefore == g_tidTick && !g_emBefore && g_emAfter;
        int fired = 0, matched = 0, compactedAlive = 0, deact = 0, bLoud = 0;
        bool earlyOk = false;
        if (g_types) {
            for (const auto& t : *g_types) {
                if (t.countA > 0) { ++fired; if (t.rttiMatchesTable) ++matched; }
                if (t.bDeactivated) {
                    ++deact;
                    if (t.compactedThenAlive) ++compactedAlive;
                    if (t.countB > t.countBAtDeact) ++bLoud;
                    if (t.tableName == "LocalizationAvailableEvent" && t.compactedThenAlive && t.countB == t.countBAtDeact) earlyOk = true;
                }
            }
        }
        const bool heroChain = DescribeFlow(reinterpret_cast<void*>(Base() + kRvaActorDamage)).find("DDShield") != std::string::npos;
        const bool monsterChain = DescribeFlow(reinterpret_cast<void*>(Base() + kRvaMonsterDamage)).find("DDShield") != std::string::npos;
        const bool s4Done = g_heroHits > 0 && g_monsterHits > 0;
        const char* s5Text = g_s5.find("实测成功") != std::string::npos ? "本次进程内堆相同，/MT 下跨 CRT 的 malloc/free 无异常；未覆盖 /MTd、对齐分配和 realloc" : "未证实";
        Logf("[结论] S1=%s", s1 ? "已证实" : "未证实");
        Logf("[结论] S2：收到事件的类型 %d 个，其中 RTTI 名与类名表一致 %d 个 → %s（未覆盖的类型见上文，等待战斗流程扩大覆盖）",
             fired, matched, (fired > 0 && matched == fired) ? "接收到的全部一致，机制已证实" : "待更多事件");
        Logf("[结论] S3：注销 %d 个类型的 B，压缩后 A 继续收到事件 %d 个，注销后 B 仍被调用的类型 %d 个；主菜单早期测试=%s",
             deact, compactedAlive, bLoud, earlyOk ? "通过（手动压缩后 A 被真实派发调用）" : "未通过或未发生");
        Logf("[结论] S4：Hero 命中 %lld，Monster 命中 %lld，入口跳转进 DDShield：Hero=%s Monster=%s → %s",
             static_cast<long long>(g_heroHits), static_cast<long long>(g_monsterHits),
             heroChain ? "是" : "否", monsterChain ? "是" : "否",
             s4Done && heroChain && monsterChain ? "已证实" : "待用户战斗流程");
        Logf("[结论] S5：%s", s5Text);
    }
    Logf("================ 报告结束 ================");
}

// ---------- 每帧 tick ----------
void TickBody() {
    if (g_stopping) return;

    const LONG64 frame = InterlockedIncrement64(&g_frames);
    if (frame == 1) {
        g_tidTick = GetCurrentThreadId();
        Logf("[S1] 主循环首帧 tick，线程 %lu（入口线程 %lu）", g_tidTick, g_tidBefore);
    }

    const ULONGLONG now = GetTickCount64();

    // D：观察 Campaign::Current 的变化（进存档前空，进入存档后非空）
    {
        static void* last = nullptr;
        static bool logged = false;
        void* cur = *Rva<void**>(kRvaCampaignCurrent);
        if (!logged || cur != last) {
            Logf("[D] Campaign::Current(118E138) = %p（之前 %p），帧 %lld", cur, last, static_cast<long long>(frame));
            last = cur;
            logged = true;
        }
    }

    // S3：运行 10 秒后注销全部 B，让列表在下一次派发时排序和压缩
    if (g_types && !g_deactDone && now - g_startTick >= 10'000) {
        g_deactDone = true;
        S3Deactivate();
    }
    if (g_types && g_deactDone) S3CheckCompaction();

    if (g_autoQuitSec && now - g_startTick >= g_autoQuitSec * 1000ull) {
        g_stopping = true;
        WriteReport("自检报告");
        Logf("[spike] 到点退出进程");
        TerminateProcess(GetCurrentProcess(), 0);
    }

    if (now - g_lastReport >= 30'000) {
        g_lastReport = now;
        WriteReport("定期快照");
    }
}

extern "C" void DDSpike_Tick() {
    __try {
        TickBody();
    } __except (RecordException(GetExceptionInformation())) {
        static bool logged = false;  // 只记一次，避免每帧刷屏
        if (!logged) {
            logged = true;
            Logf("[spike] tick 触发异常 %08X，地址 %p，目标 %p", g_lastException.ExceptionCode,
                 g_lastException.ExceptionAddress, reinterpret_cast<void*>(g_lastException.ExceptionInformation[1]));
        }
    }
}

// ---------- tick 补丁：UpdateLogicInternal 入口的透明前置 thunk ----------
std::uint8_t* AllocNear(std::uintptr_t target, std::size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t floor = target > 0x7FFF0000 ? target - 0x7FFF0000 : gran;
    const std::uintptr_t ceil = target + 0x7FFF0000;
    for (int direction = 0; direction < 2; ++direction) {
        std::uintptr_t p = (target & ~(gran - 1)) + (direction ? gran : 0);
        while (p > floor && p < ceil) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) &&
                mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                void* block = VirtualAlloc(reinterpret_cast<void*>(p), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (block) return static_cast<std::uint8_t*>(block);
            }
            p += direction ? gran : (std::uintptr_t)0 - gran;
        }
    }
    return nullptr;
}

bool InstallTickHook() {
    auto* target = Rva<std::uint8_t*>(kRvaUpdateLogic);
    // mov rax, rsp; push rbp; push rbx; push rsi; push rdi —— 7 字节整指令，无 rip 相对寻址
    static const std::uint8_t kExpected[7] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57 };
    if (std::memcmp(target, kExpected, 7) != 0) {
        Logf("[S1] UpdateLogicInternal 入口字节不符，不安装 tick hook");
        return false;
    }
    auto* thunk = AllocNear(reinterpret_cast<std::uintptr_t>(target), 0x1000);
    if (!thunk) {
        Logf("[S1] 无法分配 tick 跳板");
        return false;
    }

    std::size_t i = 0;
    auto emit = [&](std::initializer_list<std::uint8_t> bytes) { for (auto b : bytes) thunk[i++] = b; };

    // 保存整数和浮点参数寄存器，调用 tick 后恢复；入口 rsp%16==8，4 次 push 后 %16==8，sub 0x68 后对齐
    emit({ 0x51, 0x52, 0x41, 0x50, 0x41, 0x51 });              // push rcx, rdx, r8, r9
    emit({ 0x48, 0x83, 0xEC, 0x68 });                          // sub rsp, 0x68
    emit({ 0x0F, 0x11, 0x44, 0x24, 0x28 });                    // movups [rsp+0x28], xmm0
    emit({ 0x0F, 0x11, 0x4C, 0x24, 0x38 });                    // movups [rsp+0x38], xmm1
    emit({ 0x0F, 0x11, 0x54, 0x24, 0x48 });                    // movups [rsp+0x48], xmm2
    emit({ 0x0F, 0x11, 0x5C, 0x24, 0x58 });                    // movups [rsp+0x58], xmm3
    emit({ 0x48, 0xB8 });                                      // mov rax, imm64
    *reinterpret_cast<void**>(thunk + i) = reinterpret_cast<void*>(&DDSpike_Tick);
    i += 8;
    emit({ 0xFF, 0xD0 });                                      // call rax
    emit({ 0x0F, 0x10, 0x44, 0x24, 0x28 });                    // movups xmm0, [rsp+0x28]
    emit({ 0x0F, 0x10, 0x4C, 0x24, 0x38 });
    emit({ 0x0F, 0x10, 0x54, 0x24, 0x48 });
    emit({ 0x0F, 0x10, 0x5C, 0x24, 0x58 });
    emit({ 0x48, 0x83, 0xC4, 0x68 });                          // add rsp, 0x68
    emit({ 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 });              // pop r9, r8, rdx, rcx
    for (auto b : kExpected) thunk[i++] = b;                   // 搬走的入口指令
    thunk[i++] = 0xE9;                                         // jmp target+7
    *reinterpret_cast<std::int32_t*>(thunk + i) =
        static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target + 7) - reinterpret_cast<std::intptr_t>(thunk + i + 4));
    i += 4;

    DWORD old = 0;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &old)) {
        Logf("[S1] VirtualProtect 失败");
        return false;
    }
    target[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(target + 1) =
        static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(thunk) - reinterpret_cast<std::intptr_t>(target + 5));
    target[5] = 0x90;
    target[6] = 0x90;
    VirtualProtect(target, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    Logf("[S1] tick hook 已安装：UpdateLogicInternal=%p thunk=%p（%zu 字节）", target, thunk, i);
    return true;
}

}  // namespace

// ---------- 导出：由 DDModLoader_spike 的 hook 调用，运行在主线程 ----------
extern "C" __declspec(dllexport) void DDSpike_Before(unsigned tid, const char* verifiedSha1) {
    g_tidBefore = tid;
    g_exe = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_exe + dos->e_lfanew);
        g_exeSize = nt->OptionalHeader.SizeOfImage;
    }
    g_startTick = GetTickCount64();
    g_lastReport = g_startTick;
    InitializeCriticalSection(&g_cs);
    LogOpen();

    g_emBefore = *Rva<void**>(kRvaEventManager);

    char envBuf[32] = {};
    if (GetEnvironmentVariableA("DD_SPIKE_AUTOQUIT", envBuf, sizeof(envBuf)) > 0) {
        g_autoQuitSec = static_cast<DWORD>(std::strtoul(envBuf, nullptr, 10));
    }

    Logf("[spike] DDSpike 启动。exe SHA1=%s 映像基址=%p 自检退出=%lu 秒", verifiedSha1, g_exe, g_autoQuitSec);
    Logf("[S1] CreateAppSystems 入口线程 %lu，进入前 EventManager=%p", tid, g_emBefore);
}

extern "C" __declspec(dllexport) void DDSpike_After() {
    g_tidAfter = GetCurrentThreadId();
    g_emAfter = *Rva<void**>(kRvaEventManager);
    Logf("[S1] CreateAppSystems 返回线程 %lu，返回后 EventManager=%p", g_tidAfter, g_emAfter);

    Guarded("S2 注册", []() { RegisterListeners(); });
    Guarded("S3 主菜单压缩测试", []() { if (g_types) S3EarlyCompactionTest(); });
    Guarded("S4 虚表 hook", []() { InstallVtableHooks(); });
    Guarded("S5 CRT 堆", []() { RunS5(); });
    Guarded("tick hook", []() { InstallTickHook(); });

    WriteReport("启动完成快照");
    Logf("[spike] 初始化完成，等待主循环");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
