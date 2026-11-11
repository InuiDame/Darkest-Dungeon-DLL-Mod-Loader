// P0 spike 专用：在加载器里插入一个早期 hook。只由 DDModLoader_spike 目标编译（宏 DD_LOADER_SPIKE），
// 正式的 DDModLoader 不包含这段代码，行为不变。
//
// DllMain 持有加载器锁，所以这里只做内存补丁：
//   1. 校验 Darkest.exe 的 SHA1，不符就什么也不做；
//   2. 校验 CreateAppSystems 入口字节；
//   3. 在 exe 附近分配跳板页（代理 DLL 可能离 exe 超过 2GB，rel32 够不到）；
//   4. 把入口改成 jmp rel32。
// DDSpike.dll 在 hook 命中时才 LoadLibrary：此时在主线程上，已脱离加载器锁。

namespace dd_spike {

constexpr std::uintptr_t kRvaCreateAppSystems = 0x3E9320;
constexpr char kExeSha1[] = "9db2d73b89aa034e2055df65a139403e18df641f";

// mov [rsp+8], rcx：整条指令 5 字节，不含 rip 相对寻址，可以原样搬到跳板
constexpr unsigned char kEntryBytes[] = { 0x48, 0x89, 0x4C, 0x24, 0x08 };

// ---------- SHA-1 ----------
// 手写实现：DllMain 持有加载器锁，不去加载 bcrypt/advapi32
struct Sha1 {
    std::uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::uint64_t total = 0;
    std::uint8_t buf[64];
    std::size_t fill = 0;

    static std::uint32_t Rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            const std::uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void Update(const std::uint8_t* p, std::size_t n) {
        total += n;
        while (n) {
            if (fill == 0 && n >= 64) { Block(p); p += 64; n -= 64; continue; }
            const std::size_t take = (std::min)(n, 64 - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { Block(buf); fill = 0; }
        }
    }

    void Final(char* out41) {
        const std::uint64_t bits = total * 8;
        const std::uint8_t pad80 = 0x80, zero = 0;
        Update(&pad80, 1);
        while (fill != 56) Update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (56 - i * 8));
        Update(len, 8);
        for (int i = 0; i < 20; ++i) {
            std::snprintf(out41 + i * 2, 3, "%02x", std::uint8_t(h[i / 4] >> (24 - (i % 4) * 8)));
        }
        out41[40] = 0;
    }
};

// 计算当前进程 exe 的 SHA1，失败返回空串
inline bool ExeSha1(char* out41) {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    static std::uint8_t chunk[1 << 20];
    Sha1 sha;
    DWORD got = 0;
    while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got) sha.Update(chunk, got);
    CloseHandle(f);
    sha.Final(out41);
    return true;
}

// ---------- 跳板 ----------
inline std::uint8_t* AllocNear(std::uintptr_t target, std::size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    const std::uintptr_t floor = target > 0x7FFF0000 ? target - 0x7FFF0000 : gran;
    const std::uintptr_t ceil = target + 0x7FFF0000;

    // 先向低地址搜索，再向高地址，都在 ±2GB 内
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

using CreateAppSystemsFn = void*(__fastcall*)(void*);

static CreateAppSystemsFn g_gate = nullptr;     // 指向跳板里搬过去的入口指令
static std::wstring g_spikePath;
static std::uint64_t g_hookedAddress = 0;

extern "C" void* __fastcall SpikeCreateAppSystemsDetour(void* a1);

inline bool InstallEarlyHook(const std::wstring& gameRoot) {
    char sha1[41] = {};
    if (!ExeSha1(sha1)) {
        Log(L"[spike] 无法计算 Darkest.exe 的 SHA1，spike 不启用");
        return false;
    }
    if (std::strcmp(sha1, kExeSha1) != 0) {
        wchar_t wsha[41] = {};
        MultiByteToWideChar(CP_UTF8, 0, sha1, -1, wsha, 41);
        Log(std::wstring(L"[spike] Darkest.exe 的 SHA1 不符，spike 不启用：") + wsha);
        return false;
    }

    auto* target = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)) + kRvaCreateAppSystems;
    if (std::memcmp(target, kEntryBytes, sizeof(kEntryBytes)) != 0) {
        Log(L"[spike] CreateAppSystems 入口字节不符（可能已被其它补丁改过），spike 不启用");
        return false;
    }

    auto* page = AllocNear(reinterpret_cast<std::uintptr_t>(target), 0x1000);
    if (!page) {
        Log(L"[spike] 无法在 exe 附近分配跳板页，spike 不启用");
        return false;
    }

    // page+0x00：长跳到 SpikeCreateAppSystemsDetour（代理 DLL 可能在 rel32 范围外）
    //   FF 25 00 00 00 00  jmp qword ptr [rip+0]
    //   <8 字节绝对地址>
    page[0] = 0xFF; page[1] = 0x25;
    *reinterpret_cast<std::uint32_t*>(page + 2) = 0;
    *reinterpret_cast<void**>(page + 6) = &SpikeCreateAppSystemsDetour;

    // page+0x20：gate = 搬走的入口指令 + jmp 回 target+5
    auto* gate = page + 0x20;
    std::memcpy(gate, kEntryBytes, sizeof(kEntryBytes));
    gate[5] = 0xE9;
    *reinterpret_cast<std::int32_t*>(gate + 6) =
        static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target + sizeof(kEntryBytes)) - reinterpret_cast<std::intptr_t>(gate + 10));
    g_gate = reinterpret_cast<CreateAppSystemsFn>(gate);

    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log(L"[spike] VirtualProtect 失败，spike 不启用");
        return false;
    }
    target[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(target + 1) =
        static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(page) - reinterpret_cast<std::intptr_t>(target + 5));
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    g_hookedAddress = reinterpret_cast<std::uint64_t>(target);
    g_spikePath = gameRoot + L"\\mods\\DDSpike\\DDSpike.dll";
    wchar_t buf[128];
    swprintf_s(buf, L"[spike] 早期 hook 已安装：CreateAppSystems=%p 跳板页=%p", target, page);
    Log(buf);
    return true;
}

}  // namespace dd_spike

// hook 命中时已在主线程，LoadLibrary 不再受加载器锁约束
extern "C" void* __fastcall SpikeCreateAppSystemsDetour(void* a1) {
    using BeforeFn = void(__cdecl*)(unsigned, const char*);
    using AfterFn = void(__cdecl*)();

    wchar_t buf[256];
    swprintf_s(buf, L"[spike] CreateAppSystems 命中，线程 %u，开始加载 %s", GetCurrentThreadId(), dd_spike::g_spikePath.c_str());
    Log(buf);

    BeforeFn before = nullptr;
    AfterFn after = nullptr;
    HMODULE spike = LoadLibraryW(dd_spike::g_spikePath.c_str());
    if (spike) {
        before = reinterpret_cast<BeforeFn>(GetProcAddress(spike, "DDSpike_Before"));
        after = reinterpret_cast<AfterFn>(GetProcAddress(spike, "DDSpike_After"));
        Log(L"[spike] DDSpike.dll 加载成功");
    }
    else {
        Log(L"[spike] DDSpike.dll 加载失败，错误码 " + std::to_wstring(GetLastError()));
    }

    if (before) before(GetCurrentThreadId(), dd_spike::kExeSha1);
    void* result = dd_spike::g_gate(a1);
    if (after) after();
    return result;
}
