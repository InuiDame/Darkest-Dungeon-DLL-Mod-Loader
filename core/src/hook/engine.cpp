#include "engine.h"

#include "prologue.h"

#include "../guard.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ddc::hook {

const char* ResultText(Result result)
{
    switch (result) {
    case Result::Ok:
        return "成功";
    case Result::BadArgument:
        return "参数无效";
    case Result::ExpectedMismatch:
        return "目标当前内容与期望不符";
    case Result::AlreadyHooked:
        return "目标已被其他代码 hook";
    case Result::UnsafePrologue:
        return "入口指令不能安全搬移";
    case Result::NoMemory:
        return "无法在目标附近分配跳板内存";
    case Result::ProtectFailed:
        return "修改内存保护失败";
    case Result::NotFound:
        return "句柄不存在";
    case Result::NotOwner:
        return "句柄不属于该插件";
    case Result::BadTiming:
        return "当前不能修改内联 hook（不在主线程，或加载线程正在运行）";
    }
    return "未知错误";
}

namespace {

constexpr std::size_t kStubSize = 16;
constexpr std::size_t kGateSize = 64;
constexpr std::uint32_t kMaxSteal = 16;  // 能搬走的入口指令总长
constexpr std::size_t kPoolSize = 0x1000;
constexpr std::uintptr_t kReach = 0x7F000000;  // 跳转可达范围，留一点余量

// jmp qword ptr [rip+2]; int3; int3; <8 字节地址>
// 地址放在 +8，8 字节对齐，改写时是原子的
void InitStub(std::uint8_t* stub, void* destination)
{
    static const std::uint8_t kBody[8] = { 0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC };
    std::memcpy(stub, kBody, sizeof(kBody));
    InterlockedExchangePointer(reinterpret_cast<PVOID*>(stub + 8), destination);
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
}

void RetargetStub(std::uint8_t* stub, void* destination)
{
    InterlockedExchangePointer(reinterpret_cast<PVOID*>(stub + 8), destination);
}

// 向目标 ±2GB 内申请一页可执行内存
std::uint8_t* AllocateNear(std::uintptr_t target, std::size_t size)
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t granularity = si.dwAllocationGranularity;
    const std::uintptr_t floor = target > kReach ? target - kReach : granularity;
    const std::uintptr_t ceiling = target + kReach;
    for (int direction = 0; direction < 2; ++direction) {
        std::uintptr_t p = (target & ~(granularity - 1)) + (direction ? granularity : 0);
        while (p > floor && p < ceiling) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) && mbi.State == MEM_FREE &&
                mbi.RegionSize >= size) {
                void* block = VirtualAlloc(reinterpret_cast<void*>(p), size, MEM_RESERVE | MEM_COMMIT,
                    PAGE_EXECUTE_READWRITE);
                if (block) {
                    return static_cast<std::uint8_t*>(block);
                }
            }
            p += direction ? granularity : static_cast<std::uintptr_t>(0) - granularity;
        }
    }
    return nullptr;
}

class CodeAllocator
{
public:
    std::uint8_t* Allocate(std::size_t size, const void* nearAddress)
    {
        size = (size + 15) & ~static_cast<std::size_t>(15);
        const auto wanted = reinterpret_cast<std::uintptr_t>(nearAddress);
        for (auto& pool : pools_) {
            const auto base = reinterpret_cast<std::uintptr_t>(pool.base);
            const auto distance = base > wanted ? base - wanted : wanted - base;
            if (distance < kReach && pool.used + size <= kPoolSize) {
                std::uint8_t* result = pool.base + pool.used;
                pool.used += size;
                return result;
            }
        }
        std::uint8_t* page = AllocateNear(wanted, kPoolSize);
        if (!page) {
            return nullptr;
        }
        pools_.push_back({ page, size });
        return page;
    }

private:
    struct Pool {
        std::uint8_t* base;
        std::size_t used;
    };
    std::vector<Pool> pools_;
};

bool WriteMemory(void* address, const void* source, std::size_t size)
{
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(address, source, size);
    DWORD restored = 0;
    VirtualProtect(address, size, old, &restored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

// 内存不可读时返回 false
bool SafeRead(const void* address, void* out, std::size_t size)
{
    struct Args {
        const void* address;
        void* out;
        std::size_t size;
    } args{ address, out, size };
    Fault fault;
    return GuardedInvoke(
        [](void* raw) {
            auto* a = static_cast<Args*>(raw);
            std::memcpy(a->out, a->address, a->size);
        },
        &args, &fault);
}

// 从 start 沿跳转走到不是跳转为止（最多 4 跳），返回最终目的地，途中的中继页（E9、FF 25 无模块）不用于展示
const void* FollowJumps(const void* start)
{
    const auto* current = static_cast<const std::uint8_t*>(start);
    for (int hop = 0; hop < 4; ++hop) {
        std::uint8_t code[16] = {};
        if (!SafeRead(current, code, sizeof(code))) {
            break;
        }
        if (code[0] == 0xE9) {
            std::int32_t rel = 0;
            std::memcpy(&rel, code + 1, 4);
            current = current + 5 + rel;
        } else if (code[0] == 0xFF && code[1] == 0x25) {
            std::int32_t rel = 0;
            std::memcpy(&rel, code + 2, 4);
            const void* next = nullptr;
            if (!SafeRead(current + 6 + rel, &next, sizeof(next))) {
                break;
            }
            current = static_cast<const std::uint8_t*>(next);
        } else if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
            const void* next = nullptr;
            std::memcpy(&next, code + 2, 8);
            current = static_cast<const std::uint8_t*>(next);
        } else {
            break;
        }
    }
    return current;
}

// 判断入口是否已经是一条跳转，覆盖常见的 hook 写法，并尽量算出跳转目的地
bool LooksLikeJump(const std::uint8_t* code, std::uint32_t available, const void* address, std::string* detail)
{
    const auto describe = [&](const void* destination) {
        const void* final = FollowJumps(destination);
        char text[64];
        std::snprintf(text, sizeof(text), "跳转到 %p（", final);
        if (detail) {
            *detail = std::string(text) + DescribeAddress(final) + "）";
        }
    };
    if (available >= 5 && code[0] == 0xE9) {
        std::int32_t rel = 0;
        std::memcpy(&rel, code + 1, 4);
        describe(reinterpret_cast<const std::uint8_t*>(address) + 5 + rel);
        return true;
    }
    if (available >= 6 && code[0] == 0xFF && code[1] == 0x25) {
        if (available >= 14) {
            std::int32_t rel = 0;
            std::memcpy(&rel, code + 2, 4);
            const void* slot = reinterpret_cast<const std::uint8_t*>(address) + 6 + rel;
            void* destination = nullptr;
            if (SafeRead(slot, &destination, sizeof(destination))) {
                describe(destination);
            }
        }
        return true;
    }
    if (available >= 2 && code[0] == 0xEB) {
        describe(reinterpret_cast<const std::uint8_t*>(address) + 2 + static_cast<std::int8_t>(code[1]));
        return true;
    }
    if (available >= 12 && code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
        void* destination = nullptr;
        std::memcpy(&destination, code + 2, 8);
        describe(destination);
        return true;
    }
    return false;
}

std::string HexBytes(const std::uint8_t* bytes, std::size_t count)
{
    std::string out;
    char piece[4];
    for (std::size_t i = 0; i < count; ++i) {
        std::snprintf(piece, sizeof(piece), i ? " %02X" : "%02X", bytes[i]);
        out += piece;
    }
    return out;
}

}  // namespace

struct Engine::Impl {
    struct Node {
        std::uint64_t handle = 0;
        std::uint32_t owner = 0;
        std::string name;
        std::int32_t priority = 0;
        std::uint64_t order = 0;
        void* detour = nullptr;
        std::uint8_t* nextStub = nullptr;  // *original 指向这里，内容是下一环
    };

    struct Target {
        bool isInline = true;
        std::uint8_t* address = nullptr;  // 内联：函数入口；虚表：槽位
        std::uint8_t* entryStub = nullptr;  // 目标实际跳转到这里，内容是第一环
        void* gate = nullptr;  // 链的末端：内联是搬走的入口 + 跳回，虚表是原值
        std::uint8_t stolen[kMaxSteal] = {};
        std::uint32_t stealLength = 0;
        bool orphaned = false;
        bool foreignChained = false;  // 入口原本是第三方的跳转，stolen 就是那条跳转
        std::vector<std::unique_ptr<Node>> chain;
    };

    mutable std::mutex mutex;
    MemoryWriter writeMemory = &WriteMemory;
    CodeAllocator allocator;
    std::vector<std::unique_ptr<Target>> targets;
    std::uint64_t nextHandle = 1;
    std::uint64_t nextOrder = 1;

    Target* FindTarget(const void* address, bool isInline)
    {
        for (auto& target : targets) {
            if (target->address == address && target->isInline == isInline) {
                return target.get();
            }
        }
        return nullptr;
    }

    // 判断内联补丁是否仍在：入口还是我们写的 jmp
    bool PatchIntact(const Target& target) const
    {
        if (target.isInline) {
            std::uint8_t expected[5] = { 0xE9 };
            const std::int32_t rel = static_cast<std::int32_t>(
                reinterpret_cast<std::intptr_t>(target.entryStub) - reinterpret_cast<std::intptr_t>(target.address + 5));
            std::memcpy(expected + 1, &rel, 4);
            std::uint8_t current[5] = {};
            return SafeRead(target.address, current, 5) && std::memcmp(current, expected, 5) == 0;
        }
        void* value = nullptr;
        return SafeRead(target.address, &value, sizeof(value)) && value == target.entryStub;
    }

    void Relink(Target& target)
    {
        // 从后往前改，并发调用者总能看到完整的链
        auto& chain = target.chain;
        for (std::size_t i = chain.size(); i-- > 0;) {
            void* next = (i + 1 < chain.size()) ? chain[i + 1]->detour : target.gate;
            RetargetStub(chain[i]->nextStub, next);
        }
        RetargetStub(target.entryStub, chain.empty() ? target.gate : chain[0]->detour);
    }

    void InsertNode(Target& target, std::unique_ptr<Node> node)
    {
        auto position = std::upper_bound(target.chain.begin(), target.chain.end(), node,
            [](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
                if (a->priority != b->priority) {
                    return a->priority < b->priority;
                }
                return a->order < b->order;
            });
        target.chain.insert(position, std::move(node));
    }

    std::unique_ptr<Node> MakeNode(std::uint32_t owner, const char* name, std::int32_t priority, void* detour,
        const void* nearAddress)
    {
        auto node = std::make_unique<Node>();
        node->handle = nextHandle++;
        node->owner = owner;
        node->name = name ? name : "";
        node->priority = priority;
        node->order = nextOrder++;
        node->detour = detour;
        node->nextStub = allocator.Allocate(kStubSize, nearAddress);
        if (!node->nextStub) {
            return nullptr;
        }
        InitStub(node->nextStub, detour);  // 下一环在 Relink 时写入
        return node;
    }

    void Unpatch(Target& target, std::vector<std::string>* details)
    {
        char text[256];
        const auto note = [&] {
            if (details) {
                details->push_back(text);
            }
        };
        if (!PatchIntact(target)) {
            target.orphaned = true;
            RetargetStub(target.entryStub, target.gate);
            std::snprintf(text, sizeof(text),
                "目标 %p 上的补丁已被他人覆盖，不恢复字节，只保留直通", target.address);
            note();
            return;
        }
        const bool restored = target.isInline ? writeMemory(target.address, target.stolen, target.stealLength)
                                              : writeMemory(target.address, &target.gate, sizeof(void*));
        if (!restored) {
            // 补丁还在，入口继续跳到 entryStub，必须改成直通，否则会跳进已回滚的 detour
            target.orphaned = true;
            RetargetStub(target.entryStub, target.gate);
            std::snprintf(text, sizeof(text), "目标 %p 的原始字节恢复失败，补丁保留并改为直通", target.address);
            note();
            return;
        }
        std::snprintf(text, sizeof(text), "目标 %p 的链已空，补丁已恢复", target.address);
        note();
    }

    void EraseTarget(Target* target)
    {
        targets.erase(std::remove_if(targets.begin(), targets.end(),
                          [target](const std::unique_ptr<Target>& t) { return t.get() == target; }),
            targets.end());
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

InstallResult Engine::InstallInline(std::uint32_t owner, const InlineRequest& request)
{
    InstallResult out;
    if (!request.target || !request.detour || !request.original || !request.expected ||
        request.expectedLength < 5 || request.expectedLength > 32) {
        out.result = Result::BadArgument;
        out.detail = "target/detour/original/expected 不能为空，expected_len 必须在 5..32 之间";
        return out;
    }

    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    auto* address = static_cast<std::uint8_t*>(request.target);

    // 同一目标只打一次补丁，后来者串到已有的链上
    if (auto* existing = impl.FindTarget(address, true)) {
        if (!impl.PatchIntact(*existing)) {
            out.result = Result::AlreadyHooked;
            out.detail = "目标上的补丁已被他人覆盖，拒绝继续串链";
            return out;
        }
        if (existing->foreignChained && !request.chainForeignJump) {
            // 这条链接在 DDShield 等旧式 DLL 的 hook 后面，只给 DDCore 自己用
            out.result = Result::AlreadyHooked;
            out.detail = "入口已被第三方 hook（DDCore 内部已在其后串联），不接受插件串入";
            return out;
        }
        existing->orphaned = false;  // 补丁还在，说明之前只是恢复字节失败，这条链可以继续用
        auto node = impl.MakeNode(owner, request.name, request.priority, request.detour, address);
        if (!node) {
            out.result = Result::NoMemory;
            out.detail = "分配跳板失败";
            return out;
        }
        out.handle = node->handle;
        *request.original = node->nextStub;
        impl.InsertNode(*existing, std::move(node));
        impl.Relink(*existing);
        out.freshPatch = false;
        out.chainDepth = static_cast<std::uint32_t>(existing->chain.size());
        return out;
    }

    // 和已有内联目标的偷取范围重叠也不允许
    for (auto& target : impl.targets) {
        if (target->isInline && address < target->address + target->stealLength &&
            target->address < address + request.expectedLength) {
            out.result = Result::AlreadyHooked;
            out.detail = "目标与另一个 hook 的偷取范围重叠";
            return out;
        }
    }

    // 比对目标当前字节
    std::uint8_t current[32] = {};
    const std::uint32_t readLength = request.chainForeignJump ? (std::max)(request.expectedLength, 16u)
                                                              : request.expectedLength;
    if (!SafeRead(address, current, readLength)) {
        out.result = Result::ExpectedMismatch;
        out.detail = "目标地址不可读";
        return out;
    }
    std::uint32_t steal = 0;
    const std::uint8_t* stolenBytes = request.expected;
    bool foreignChained = false;
    if (std::memcmp(current, request.expected, request.expectedLength) != 0) {
        std::string jump;
        const bool isJump = LooksLikeJump(current, readLength, address, &jump);
        if (isJump && request.chainForeignJump && (current[0] == 0xE9 || (current[0] == 0xFF && current[1] == 0x25))) {
            // 把第三方的入口跳转搬进 gate，改成绝对跳转
            steal = current[0] == 0xE9 ? 5u : 6u;
            stolenBytes = current;
            foreignChained = true;
        } else if (isJump) {
            out.result = Result::AlreadyHooked;
            out.detail = "入口已被修改（" + HexBytes(current, request.expectedLength) + "），" + jump +
                "，不破坏对方，放弃安装";
            return out;
        } else {
            out.result = Result::ExpectedMismatch;
            out.detail = "入口字节 " + HexBytes(current, request.expectedLength) + " 与期望 " +
                HexBytes(request.expected, request.expectedLength) + " 不符";
            return out;
        }
    }

    if (!foreignChained) {
        const char* reason = nullptr;
        steal = ComputeStealLength(request.expected, request.expectedLength, 5, &reason);
        if (steal > kMaxSteal) {
            steal = 0;
            reason = "要搬走的入口指令超过 16 字节";
        }
        if (steal == 0) {
            out.result = Result::UnsafePrologue;
            out.detail = std::string("入口 ") + HexBytes(request.expected, request.expectedLength) +
                " 不能安全搬移：" + (reason ? reason : "未知原因");
            return out;
        }
    }

    std::uint8_t* entry = impl.allocator.Allocate(kStubSize, address);
    std::uint8_t* gate = impl.allocator.Allocate(kGateSize, address);
    auto node = impl.MakeNode(owner, request.name, request.priority, request.detour, address);
    if (!entry || !gate || !node) {
        out.result = Result::NoMemory;
        out.detail = "无法在目标 ±2GB 内分配跳板";
        return out;
    }

    if (foreignChained) {
        // 门 = 第三方跳转的目的地（绝对跳转）。E9 算出目的地；FF25 保持原来的间接方式，
        // 这样对方日后改写跳转槽时仍然生效，够不到时才退回快照目的地
        std::int32_t rel = 0;
        std::uint8_t* jmp = gate;
        jmp[0] = 0xFF;
        jmp[1] = 0x25;
        std::memset(jmp + 2, 0, 4);
        void* destination = nullptr;
        if (current[0] == 0xE9) {
            std::memcpy(&rel, current + 1, 4);
            destination = address + 5 + rel;
        } else {
            std::memcpy(&rel, current + 2, 4);
            auto* slot = address + 6 + rel;
            const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(slot)) -
                static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(jmp + 6));
            if (delta > INT32_MIN && delta < INT32_MAX) {
                const auto newDisp = static_cast<std::int32_t>(delta);
                std::memcpy(jmp + 2, &newDisp, 4);
                std::memset(jmp + 6, 0xCC, 8);
            } else {
                if (!SafeRead(slot, &destination, sizeof(destination))) {
                    out.result = Result::ExpectedMismatch;
                    out.detail = "跳转槽不可读";
                    return out;
                }
            }
        }
        if (destination) {
            std::memcpy(jmp + 6, &destination, sizeof(destination));
        }
    } else {
        // 门：搬走的入口指令 + 跳回 target+steal
        std::memcpy(gate, request.expected, steal);
        std::uint8_t* jmp = gate + steal;
        jmp[0] = 0xFF;
        jmp[1] = 0x25;
        std::memset(jmp + 2, 0, 4);
        void* back = address + steal;
        std::memcpy(jmp + 6, &back, sizeof(back));
    }
    FlushInstructionCache(GetCurrentProcess(), gate, kGateSize);

    auto target = std::make_unique<Impl::Target>();
    target->isInline = true;
    target->address = address;
    target->entryStub = entry;
    target->gate = gate;
    target->stealLength = steal;
    target->foreignChained = foreignChained;
    std::memcpy(target->stolen, stolenBytes, steal);
    InitStub(entry, gate);  // 补丁打上之前先指向门，防止并发调用走到半成品

    out.handle = node->handle;
    *request.original = node->nextStub;
    impl.InsertNode(*target, std::move(node));
    impl.Relink(*target);

    std::uint8_t patch[kMaxSteal];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const std::int32_t rel = static_cast<std::int32_t>(
        reinterpret_cast<std::intptr_t>(entry) - reinterpret_cast<std::intptr_t>(address + 5));
    std::memcpy(patch + 1, &rel, 4);
    if (!impl.writeMemory(address, patch, steal)) {
        out.result = Result::ProtectFailed;
        out.detail = "VirtualProtect 失败，未打补丁";
        out.handle = 0;
        *request.original = nullptr;
        return out;
    }

    out.freshPatch = true;
    out.chainDepth = 1;
    out.chainedForeign = foreignChained;
    impl.targets.push_back(std::move(target));
    return out;
}

InstallResult Engine::InstallVtable(std::uint32_t owner, const VtableRequest& request)
{
    InstallResult out;
    if (!request.slot || !request.detour || !request.original) {
        out.result = Result::BadArgument;
        out.detail = "slot/detour/original 不能为空";
        return out;
    }

    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    auto* address = reinterpret_cast<std::uint8_t*>(request.slot);

    if (auto* existing = impl.FindTarget(address, false)) {
        if (!impl.PatchIntact(*existing)) {
            out.result = Result::AlreadyHooked;
            out.detail = "虚表槽位已被他人改写，拒绝继续串链";
            return out;
        }
        existing->orphaned = false;
        auto node = impl.MakeNode(owner, request.name, request.priority, request.detour, address);
        if (!node) {
            out.result = Result::NoMemory;
            out.detail = "分配跳板失败";
            return out;
        }
        out.handle = node->handle;
        *request.original = node->nextStub;
        impl.InsertNode(*existing, std::move(node));
        impl.Relink(*existing);
        out.freshPatch = false;
        out.chainDepth = static_cast<std::uint32_t>(existing->chain.size());
        return out;
    }

    void* current = nullptr;
    if (!SafeRead(request.slot, &current, sizeof(current))) {
        out.result = Result::ExpectedMismatch;
        out.detail = "虚表槽位不可读";
        return out;
    }
    if (request.expectedValue && current != request.expectedValue) {
        char text[128];
        std::snprintf(text, sizeof(text), "虚表槽位当前值 %p 与期望 %p 不符", current, request.expectedValue);
        out.result = Result::ExpectedMismatch;
        out.detail = text;
        return out;
    }

    std::uint8_t* entry = impl.allocator.Allocate(kStubSize, address);
    auto node = impl.MakeNode(owner, request.name, request.priority, request.detour, address);
    if (!entry || !node) {
        out.result = Result::NoMemory;
        out.detail = "无法分配跳板";
        return out;
    }

    auto target = std::make_unique<Impl::Target>();
    target->isInline = false;
    target->address = address;
    target->entryStub = entry;
    target->gate = current;
    InitStub(entry, current);

    out.handle = node->handle;
    *request.original = node->nextStub;
    impl.InsertNode(*target, std::move(node));
    impl.Relink(*target);

    void* newValue = entry;
    if (!impl.writeMemory(request.slot, &newValue, sizeof(newValue))) {
        out.result = Result::ProtectFailed;
        out.detail = "VirtualProtect 失败，未修改虚表";
        out.handle = 0;
        *request.original = nullptr;
        return out;
    }

    out.freshPatch = true;
    out.chainDepth = 1;
    impl.targets.push_back(std::move(target));
    return out;
}

Result Engine::Remove(std::uint32_t owner, std::uint64_t handle, std::string* detail)
{
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    for (auto& targetPtr : impl.targets) {
        auto& target = *targetPtr;
        for (auto it = target.chain.begin(); it != target.chain.end(); ++it) {
            if ((*it)->handle != handle) {
                continue;
            }
            if ((*it)->owner != owner) {
                return Result::NotOwner;
            }
            // 结点本身不释放：调用者手里还留着 nextStub 指针，继续调用只会走向下一环
            target.chain.erase(it);
            if (target.chain.empty()) {
                std::vector<std::string> notes;
                impl.Unpatch(target, &notes);
                if (detail && !notes.empty()) {
                    *detail = notes[0];
                }
                if (!target.orphaned) {
                    impl.EraseTarget(&target);
                }
            } else {
                impl.Relink(target);
            }
            return Result::Ok;
        }
    }
    return Result::NotFound;
}

std::size_t Engine::RemoveOwner(std::uint32_t owner, std::vector<std::string>* details)
{
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    std::size_t removed = 0;
    for (std::size_t i = 0; i < impl.targets.size();) {
        auto& target = *impl.targets[i];
        const auto before = target.chain.size();
        target.chain.erase(
            std::remove_if(target.chain.begin(), target.chain.end(),
                [owner](const std::unique_ptr<Impl::Node>& node) { return node->owner == owner; }),
            target.chain.end());
        const auto gone = before - target.chain.size();
        removed += gone;
        if (gone == 0) {
            ++i;
            continue;
        }
        if (target.chain.empty()) {
            impl.Unpatch(target, details);
            if (!target.orphaned) {
                impl.targets.erase(impl.targets.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        } else {
            impl.Relink(target);
        }
        ++i;
    }
    return removed;
}

void Engine::RestoreOriginalBytes(const std::uint8_t* base, std::uint8_t* copy, std::size_t size) const
{
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    for (const auto& target : impl.targets) {
        if (!target->isInline || target->stealLength == 0 || target->address < base ||
            static_cast<std::size_t>(target->address - base) + target->stealLength > size) {
            continue;
        }
        std::memcpy(copy + (target->address - base), target->stolen, target->stealLength);
    }
}

void Engine::SetMemoryWriterForTest(MemoryWriter writer)
{
    std::lock_guard lock(impl_->mutex);
    impl_->writeMemory = writer ? writer : &WriteMemory;
}

std::vector<TargetInfo> Engine::Snapshot() const
{
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    std::vector<TargetInfo> out;
    for (const auto& target : impl.targets) {
        TargetInfo info;
        info.isInline = target->isInline;
        info.address = target->address;
        info.stealLength = target->stealLength;
        info.orphaned = target->orphaned;
        info.foreignChained = target->foreignChained;
        info.intact = target->orphaned ? false : impl.PatchIntact(*target);
        for (const auto& node : target->chain) {
            info.chain.push_back({ node->owner, node->name, node->priority, node->handle });
        }
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace ddc::hook
