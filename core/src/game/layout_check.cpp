#include "layout_check.h"

#include "../../../common/pattern.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace ddc::game::check {

// ============================================================================ 清单表

namespace {

constexpr SigKind kKind_HOOK = SigKind::Hook;
constexpr SigKind kKind_GLOBAL = SigKind::Global;
constexpr SigKind kKind_GLOBAL_ANY = SigKind::GlobalAny;
constexpr SigKind kKind_CODE = SigKind::Code;
constexpr SigNeed kSigNeed_REQUIRED = SigNeed::Required;
constexpr SigNeed kSigNeed_LOADER = SigNeed::Loader;
constexpr SigNeed kSigNeed_OPT = SigNeed::Optional;
constexpr SigNeed kSigNeed_ANCHOR = SigNeed::Anchor;

constexpr ItemNeed kItemNeed_REQUIRED = ItemNeed::Required;
constexpr ItemNeed kItemNeed_OPT = ItemNeed::Optional;
constexpr ItemCheck kItemCheck_RUNTIME = ItemCheck::Runtime;
constexpr ItemCheck kItemCheck_OFFLINE = ItemCheck::Offline;
constexpr ItemCheck kItemCheck_NONE = ItemCheck::None;

constexpr Origin kOrigin_HIT = Origin::Hit;
constexpr Origin kOrigin_TARGET = Origin::Target;
constexpr Origin kOrigin_HIT_VALUE = Origin::HitValue;
constexpr Origin kOrigin_VSLOT = Origin::VSlot;
constexpr Origin kOrigin_VSLOT_VALUE = Origin::VSlotValue;
constexpr Origin kOrigin_NOSIG = Origin::NoSig;
constexpr Where kWhere_ENTRY = Where::Entry;
constexpr Where kWhere_WINDOW = Where::Window;
constexpr Where kWhere_FUNC = Where::Func;
constexpr Where kWhere_TEXT = Where::Text;
constexpr Read kRead_NONE = Read::None;
constexpr Read kRead_U8 = Read::U8;
constexpr Read kRead_U16 = Read::U16;
constexpr Read kRead_U32 = Read::U32;
constexpr Read kRead_U64 = Read::U64;
constexpr Read kRead_I8 = Read::I8;
constexpr Read kRead_I32 = Read::I32;
constexpr Read kRead_DEREF_U32 = Read::DerefU32;
constexpr Read kRead_REL32 = Read::Rel32;

// signatures.inc 的默认宏（#ifndef 定义的空宏）在第一次包含后一直存在，每一遍都先 #undef 再定义
#undef DD_SIG
#undef DD_ENTRY
#undef DD_CHAIN_TARGET
#undef DD_RUNTIME_SELECT
#undef DD_DEPS
#undef DD_KNOWN_BUILD

const SigDef kSigRows[] = {
#define DD_SIG(id, kind, req, steal, capture, pattern) { #id, kKind_##kind, kSigNeed_##req, steal, capture, pattern },
#define DD_KNOWN_BUILD(sha, label)
#define DD_ENTRY(id, bytes)
#define DD_CHAIN_TARGET(id)
#define DD_RUNTIME_SELECT(id)
#define DD_DEPS(id, deps)
#include "signatures.inc"
};
#undef DD_SIG
#undef DD_DEPS
#undef DD_RUNTIME_SELECT

struct NamedText {
    const char* id;
    const char* text;
};

const NamedText kDepsRows[] = {
#define DD_SIG(id, kind, req, steal, capture, pattern)
#define DD_RUNTIME_SELECT(id)
#define DD_DEPS(id, deps) { #id, deps },
#include "signatures.inc"
};
#undef DD_DEPS
#undef DD_RUNTIME_SELECT

const char* const kSelectRows[] = {
#define DD_RUNTIME_SELECT(id) #id,
#define DD_DEPS(id, deps)
#include "signatures.inc"
};
#undef DD_SIG
#undef DD_ENTRY
#undef DD_CHAIN_TARGET
#undef DD_RUNTIME_SELECT
#undef DD_DEPS
#undef DD_KNOWN_BUILD

#undef DD_LAYOUT
#undef DD_EVIDENCE
#undef DD_OFFLINE
#undef DD_UNVERIFIED
#undef DD_NAMES
#undef DD_VALUES

const ItemDef kItemRows[] = {
#define DD_LAYOUT(id, type, value, need, check, deps) \
    { #id, #type, static_cast<std::int64_t>(value), kItemNeed_##need, kItemCheck_##check, deps },
#define DD_EVIDENCE(id, sig, origin, arg, where, span, pattern, at, read, mul, rel, add)
#define DD_OFFLINE(id, check)
#define DD_UNVERIFIED(id, text)
#define DD_NAMES(id, ...)
#define DD_VALUES(id, ...)
#include "layout.inc"
};
#undef DD_LAYOUT
#undef DD_EVIDENCE

const EvidenceDef kEvidenceRows[] = {
#define DD_LAYOUT(id, type, value, need, check, deps)
#define DD_EVIDENCE(id, sig, origin, arg, where, span, pattern, at, read, mul, rel, add) \
    { #id, #sig, kOrigin_##origin, arg, kWhere_##where, span, pattern, at, kRead_##read, mul, #rel, add },
#include "layout.inc"
};
#undef DD_LAYOUT
#undef DD_EVIDENCE
#undef DD_OFFLINE
#undef DD_UNVERIFIED
#undef DD_NAMES
#undef DD_VALUES

struct Tables {
    std::vector<SigDef> sigs;
    std::vector<ItemDef> items;
    std::vector<EvidenceDef> evidence;
    std::unordered_map<std::string_view, int> sigIndex;
    std::unordered_map<std::string_view, int> itemIndex;

    Tables()
    {
        sigs.assign(std::begin(kSigRows), std::end(kSigRows));
        for (std::size_t i = 0; i < sigs.size(); ++i) {
            sigIndex.emplace(sigs[i].id, static_cast<int>(i));
        }
        for (const auto& row : kDepsRows) {
            if (const auto it = sigIndex.find(row.id); it != sigIndex.end()) {
                sigs[it->second].deps = row.text;
            }
        }
        for (const char* id : kSelectRows) {
            if (const auto it = sigIndex.find(id); it != sigIndex.end()) {
                sigs[it->second].runtimeSelect = true;
            }
        }
        items.assign(std::begin(kItemRows), std::end(kItemRows));
        for (std::size_t i = 0; i < items.size(); ++i) {
            itemIndex.emplace(items[i].id, static_cast<int>(i));
        }
        evidence.assign(std::begin(kEvidenceRows), std::end(kEvidenceRows));
        std::unordered_map<std::string_view, std::uint32_t> counters;
        for (auto& ev : evidence) {
            ev.index = counters[ev.item]++;
        }
    }
};

const Tables& T()
{
    static const Tables tables;
    return tables;
}

double NowMicros()
{
    static const double ticksPerMicro = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1e6;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / ticksPerMicro;
}

std::string Hex(std::int64_t value)
{
    char text[32];
    if (value < 0) {
        std::snprintf(text, sizeof(text), "-0x%llX", static_cast<unsigned long long>(-value));
    } else {
        std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    }
    return text;
}

std::string Rva(std::int64_t value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(value));
    return text;
}

bool ReadI32(const ImageView& image, std::int64_t rva, std::int32_t* out)
{
    if (rva < 0 || rva + 4 > static_cast<std::int64_t>(image.size)) {
        return false;
    }
    std::memcpy(out, image.base + rva, 4);
    return true;
}

bool ReadU32(const ImageView& image, std::int64_t rva, std::uint32_t* out)
{
    if (rva < 0 || rva + 4 > static_cast<std::int64_t>(image.size)) {
        return false;
    }
    std::memcpy(out, image.base + rva, 4);
    return true;
}

std::int64_t ItemValue(std::string_view name)
{
    const int index = FindItem(name);
    return index < 0 ? 0 : T().items[static_cast<std::size_t>(index)].value;  // 特征码名字和 NONE 按 0 算
}

}  // namespace

const std::vector<SigDef>& Signatures() { return T().sigs; }
const std::vector<ItemDef>& Items() { return T().items; }
const std::vector<EvidenceDef>& Evidence() { return T().evidence; }

int FindSig(std::string_view id)
{
    const auto& index = T().sigIndex;
    const auto it = index.find(id);
    return it == index.end() ? -1 : it->second;
}

int FindItem(std::string_view id)
{
    const auto& index = T().itemIndex;
    const auto it = index.find(id);
    return it == index.end() ? -1 : it->second;
}

std::string Describe(const EvidenceDef& ev)
{
    std::string origin;
    switch (ev.origin) {
    case Origin::Hit: origin = std::string(ev.sig) + " 的命中位置"; break;
    case Origin::Target: origin = std::string(ev.sig) + " 的解析结果"; break;
    case Origin::HitValue: origin = std::string(ev.sig) + " 的命中位置 + 常量" + (ev.arg >= 0 ? "+" : "") + std::to_string(ev.arg); break;
    case Origin::VSlot: origin = std::string(ev.sig) + " 虚表槽 " + std::to_string(ev.arg); break;
    case Origin::VSlotValue: origin = std::string(ev.sig) + " 虚表槽（常量）"; break;
    case Origin::NoSig: origin = ".text"; break;
    }
    std::string where;
    switch (ev.where) {
    case Where::Entry: where = "起点处"; break;
    case Where::Window: where = "起点后 " + Hex(ev.span) + " 字节内"; break;
    case Where::Func: where = "所在函数内"; break;
    case Where::Text: where = "整个 .text"; break;
    }
    return origin + "，" + where + "匹配 " + ev.pattern;
}

// ============================================================================ 映像

const char* ImageView::SectionOf(std::int64_t rva) const
{
    for (const auto& s : sections) {
        if (rva >= s.begin && rva < s.end) {
            return s.name.c_str();
        }
    }
    return nullptr;
}

bool DescribeImage(const std::uint8_t* mem, std::uint64_t pointerBase, ImageView* out, std::string* error)
{
    const auto fail = [&](const char* text) {
        if (error) {
            *error = text;
        }
        return false;
    };
    if (!mem) {
        return fail("映像为空");
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mem);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return fail("不是 PE 文件（MZ）");
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mem + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return fail("不是 64 位 PE");
    }
    ImageView view;
    view.base = mem;
    view.size = nt->OptionalHeader.SizeOfImage;
    view.pointerBase = pointerBase;
    view.preferredBase = nt->OptionalHeader.ImageBase;
    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
        view.exceptionRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
        view.exceptionSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    bool haveText = false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        Section s;
        s.name.assign(reinterpret_cast<const char*>(section->Name), strnlen(reinterpret_cast<const char*>(section->Name), 8));
        s.begin = section->VirtualAddress;
        s.end = section->VirtualAddress + section->Misc.VirtualSize;
        // 同名的节只认第一个（和 image.py 的 sections.setdefault 一致）
        const bool seen = std::any_of(view.sections.begin(), view.sections.end(), [&](const Section& x) { return x.name == s.name; });
        if (seen) {
            continue;
        }
        if (s.name == ".text") {
            view.textBegin = s.begin;
            view.textEnd = s.end;
            haveText = true;
        }
        view.sections.push_back(std::move(s));
    }
    if (!haveText) {
        return fail("没有 .text 节");
    }
    *out = std::move(view);
    return true;
}

// ============================================================================ 函数表

namespace {
constexpr std::uint8_t kUnwFlagChainInfo = 4;

struct RawEntry {
    std::uint32_t begin, end, unwind;
};

bool ReadEntry(const ImageView& image, std::int64_t rva, RawEntry* out)
{
    if (rva < 0 || rva + 12 > static_cast<std::int64_t>(image.size)) {
        return false;
    }
    std::memcpy(out, image.base + rva, 12);
    return true;
}
}  // namespace

std::uint32_t FunctionTable::PrimaryOf(Entry entry) const
{
    // 和 image.py 的 _primary_of 逐行对应
    const std::int64_t size = static_cast<std::int64_t>(image_.size);
    std::uint32_t begin = entry.begin;
    std::uint32_t unwind = entry.unwind;
    for (int step = 0; step < 32; ++step) {
        if (unwind & 1) {
            const std::int64_t at = unwind & ~1u;
            if (at >= size - 12) {
                break;
            }
            RawEntry next{};
            ReadEntry(image_, at, &next);
            begin = next.begin;
            unwind = next.unwind;
            continue;
        }
        if (static_cast<std::int64_t>(unwind) + 4 > size) {
            break;
        }
        const std::uint8_t flags = image_.base[unwind] >> 3;
        if (!(flags & kUnwFlagChainInfo)) {
            break;
        }
        const std::uint8_t count = image_.base[unwind + 2];
        const std::int64_t chained = static_cast<std::int64_t>(unwind) + 4 + ((count + 1) & ~1) * 2;
        if (chained + 12 > size) {
            break;
        }
        RawEntry next{};
        ReadEntry(image_, chained, &next);
        begin = next.begin;
        unwind = next.unwind;
    }
    return begin;
}

FunctionTable::FunctionTable(const ImageView& image) : image_(image)
{
    std::vector<RawEntry> raw;
    const std::int64_t start = image.exceptionRva;
    const std::int64_t stop = start + image.exceptionSize;  // range(start, start + size - 11, 12)
    for (std::int64_t off = start; off + 12 <= stop; off += 12) {
        RawEntry e{};
        if (!ReadEntry(image, off, &e)) {
            break;
        }
        if (e.begin != 0) {
            raw.push_back(e);
        }
    }
    std::sort(raw.begin(), raw.end(), [](const RawEntry& a, const RawEntry& b) {
        return a.begin != b.begin ? a.begin < b.begin : (a.end != b.end ? a.end < b.end : a.unwind < b.unwind);
    });
    // BeginAddress 相同的表项只留最后一个（image.py 按 begin 建字典，后面的覆盖前面的）
    entries_.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i + 1 < raw.size() && raw[i + 1].begin == raw[i].begin) {
            continue;
        }
        entries_.push_back(Entry{ raw[i].begin, raw[i].end, raw[i].unwind, 0 });
    }
    byPrimary_.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].primary = PrimaryOf(entries_[i]);
        byPrimary_.emplace_back(entries_[i].primary, static_cast<std::uint32_t>(i));
    }
    std::sort(byPrimary_.begin(), byPrimary_.end());
}

bool FunctionTable::Ranges(std::int64_t rva, std::vector<std::pair<std::uint32_t, std::uint32_t>>* out) const
{
    out->clear();
    if (rva < 0 || rva > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    const auto it = std::upper_bound(entries_.begin(), entries_.end(), static_cast<std::uint32_t>(rva),
        [](std::uint32_t value, const Entry& e) { return value < e.begin; });
    if (it == entries_.begin()) {
        return false;
    }
    const Entry& entry = *(it - 1);
    if (rva >= entry.end) {
        return false;
    }
    const auto range = std::equal_range(byPrimary_.begin(), byPrimary_.end(), std::make_pair(entry.primary, 0u),
        [](const std::pair<std::uint32_t, std::uint32_t>& a, const std::pair<std::uint32_t, std::uint32_t>& b) {
            return a.first < b.first;
        });
    for (auto p = range.first; p != range.second; ++p) {
        const Entry& chunk = entries_[p->second];
        out->emplace_back(chunk.begin, chunk.end);
    }
    return true;
}

bool FunctionTable::Enclosing(std::int64_t rva, std::uint32_t* primary) const
{
    // image.py 的 entry_of + func_primary：BeginAddress 不超过 rva 的最后一个表项，rva 要在 [begin, end) 里
    if (rva < 0 || rva > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    const auto it = std::upper_bound(entries_.begin(), entries_.end(), static_cast<std::uint32_t>(rva),
        [](std::uint32_t value, const Entry& e) { return value < e.begin; });
    if (it == entries_.begin() || rva >= (it - 1)->end) {
        return false;
    }
    *primary = (it - 1)->primary;
    return true;
}

bool FunctionTable::IsFunctionStart(std::int64_t rva) const
{
    // image.py：rva in func_ends（某个表项的 BeginAddress，不管 end）且 func_primary[rva] == rva
    if (rva < 0 || rva > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), static_cast<std::uint32_t>(rva),
        [](const Entry& e, std::uint32_t value) { return e.begin < value; });
    return it != entries_.end() && it->begin == rva && it->primary == rva;
}

// ============================================================================ 扫描

Scanner::Scanner(const ImageView& image) : image_(image)
{
    // 每 16 个字节取一个，只用来给锚点排序，不影响命中结果
    for (std::uint64_t r = image.textBegin; r < image.textEnd && r < image.size; r += 16) {
        ++freq_[image.base[r]];
    }
}

bool Scanner::Compile(const char* text, Pattern* out) const
{
    out->ok = false;
    if (!dd::common::ParsePattern(text, out->bytes)) {
        return false;
    }
    out->anchor = out->bytes.size();
    std::uint32_t best = (std::numeric_limits<std::uint32_t>::max)();
    for (std::size_t i = 0; i < out->bytes.size(); ++i) {
        const int b = out->bytes[i];
        if (b >= 0 && freq_[b] < best) {
            best = freq_[b];
            out->anchor = i;
        }
    }
    out->ok = true;
    return true;
}

void Scanner::Scan(const Pattern& pattern, std::int64_t begin, std::int64_t end, std::vector<std::uint32_t>* hits) const
{
    const std::int64_t n = static_cast<std::int64_t>(pattern.bytes.size());
    begin = (std::max)(begin, std::int64_t{ 0 });
    end = (std::min)(end, static_cast<std::int64_t>(image_.size));
    if (!pattern.ok || n == 0 || end - begin < n) {
        return;
    }
    const std::uint8_t* base = image_.base;
    const auto matches = [&](std::int64_t p) {
        for (std::int64_t j = 0; j < n; ++j) {
            const int b = pattern.bytes[static_cast<std::size_t>(j)];
            if (b >= 0 && base[p + j] != static_cast<std::uint8_t>(b)) {
                return false;
            }
        }
        return true;
    };
    if (pattern.anchor >= pattern.bytes.size()) {
        for (std::int64_t p = begin; p + n <= end; ++p) {
            hits->push_back(static_cast<std::uint32_t>(p));
        }
        return;
    }
    const std::int64_t k = static_cast<std::int64_t>(pattern.anchor);
    const auto value = static_cast<unsigned char>(pattern.bytes[pattern.anchor]);
    const std::uint8_t* q = base + begin + k;
    const std::uint8_t* const stop = base + end - n + k + 1;  // 最后一个可能的锚点位置之后
    while (q < stop) {
        q = static_cast<const std::uint8_t*>(std::memchr(q, value, static_cast<std::size_t>(stop - q)));
        if (!q) {
            break;
        }
        const std::int64_t p = (q - base) - k;
        if (matches(p)) {
            hits->push_back(static_cast<std::uint32_t>(p));
        }
        ++q;
    }
}

// ============================================================================ 特征码解析

namespace {

// Query 类名表：0x80 字节一项（ClassNameStride），最多 64 项（QueryClassCapacity），全部以 Query 结尾的那张（sigs.py 的
// select_query_class_names）。返回是否选中和项数
bool SelectQueryClassNames(const ImageView& image, std::uint32_t candidate, std::uint32_t* count)
{
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const std::int64_t rva = static_cast<std::int64_t>(candidate) + static_cast<std::int64_t>(i) * 0x80;
        if (rva < 0 || rva >= static_cast<std::int64_t>(image.size)) {
            break;
        }
        const std::size_t limit = static_cast<std::size_t>((std::min)(rva + 0x80, static_cast<std::int64_t>(image.size)) - rva);
        const char* name = reinterpret_cast<const char*>(image.base + rva);
        const std::size_t length = strnlen(name, limit);
        if (length == limit) {
            break;  // 范围内没有结尾的 0：image.cstr 返回 None
        }
        if (length == 0) {
            break;
        }
        if (length < 5 || std::strcmp(name + length - 5, "Query") != 0) {
            return false;
        }
        ++n;
    }
    *count = n;
    return n > 0;
}

}  // namespace

std::string CheckHookEntry(const FunctionTable& table, std::uint32_t hit)
{
    std::uint32_t primary = 0;
    if (!table.Enclosing(hit, &primary) || table.IsFunctionStart(hit)) {
        return {};
    }
    return "命中位置不是函数入口（在 " + Rva(primary) + " 开始的函数里，+" +
        Hex(static_cast<std::int64_t>(hit) - static_cast<std::int64_t>(primary)) + "）";
}

std::vector<SigResult> ResolveSignatures(const ImageView& image, const Scanner& scanner, bool scanLoader, const FunctionTable* table)
{
    const auto& defs = Signatures();
    std::vector<SigResult> out(defs.size());
    std::unique_ptr<FunctionTable> ownTable;
    const auto functions = [&]() -> const FunctionTable& {
        if (table) {
            return *table;
        }
        if (!ownTable) {
            ownTable = std::make_unique<FunctionTable>(image);
        }
        return *ownTable;
    };
    std::vector<std::uint32_t> hits;
    const std::int64_t size = static_cast<std::int64_t>(image.size);
    const auto targetOf = [&](std::uint32_t hit, std::int32_t capture, std::int64_t* target) {
        std::int32_t disp = 0;
        const std::int64_t at = static_cast<std::int64_t>(hit) + capture;
        if (!ReadI32(image, at, &disp)) {
            return false;
        }
        *target = at + 4 + disp;
        return true;
    };
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const SigDef& def = defs[i];
        SigResult& r = out[i];
        if (def.need == SigNeed::Loader && !scanLoader) {
            r.problem = "加载器专用（入口已被加载器打了补丁），DDCore 不扫描";
            continue;
        }
        r.scanned = true;
        Pattern pattern;
        if (!scanner.Compile(def.pattern, &pattern)) {
            r.problem = "特征码格式错误";
            continue;
        }
        hits.clear();
        scanner.Scan(pattern, image.textBegin, image.textEnd, &hits);
        r.hits = static_cast<std::uint32_t>(hits.size());
        if (def.kind == SigKind::GlobalAny) {
            if (hits.empty()) {
                r.problem = "0 次命中";
                continue;
            }
            std::vector<std::int64_t> targets;
            bool bad = false;
            for (const std::uint32_t h : hits) {
                std::int64_t t = 0;
                if (!targetOf(h, def.capture, &t)) {
                    bad = true;
                    continue;
                }
                if (std::find(targets.begin(), targets.end(), t) == targets.end()) {
                    targets.push_back(t);
                }
            }
            for (const std::int64_t t : targets) {
                bad = bad || t < 0 || t >= size;
            }
            if (bad) {
                r.problem = "解析出的地址超出映像";
                continue;
            }
            r.hasHit = true;
            r.hit = hits.front();  // 扫描结果按地址升序
            for (const std::int64_t t : targets) {
                r.candidates.push_back(static_cast<std::uint32_t>(t));
            }
            if (targets.size() > 1 && !def.runtimeSelect) {
                r.problem = std::to_string(hits.size()) + " 次命中解析出 " + std::to_string(targets.size()) + " 个不同地址";
                continue;
            }
            r.resolved = r.candidates.front();
            if (targets.size() > 1 && std::strcmp(def.id, "QueryClassNames") == 0) {
                for (const std::uint32_t candidate : r.candidates) {
                    std::uint32_t count = 0;
                    if (SelectQueryClassNames(image, candidate, &count)) {
                        r.resolved = candidate;
                        r.selected = true;
                        r.selectedCount = count;
                        break;
                    }
                }
            }
            r.hasResolved = true;
            r.ok = true;
            continue;
        }
        if (hits.size() != 1) {
            r.problem = hits.empty() ? "0 次命中" : "命中 " + std::to_string(hits.size()) + " 次，要求唯一";
            continue;
        }
        r.hasHit = true;
        r.hit = hits.front();
        if (def.kind == SigKind::Global) {
            std::int64_t t = 0;
            if (!targetOf(r.hit, def.capture, &t) || t < 0 || t >= size) {
                r.problem = "解析出的全局地址超出映像";
                continue;
            }
            r.hasResolved = true;
            r.resolved = static_cast<std::uint32_t>(t);
            // 必需（REQUIRED 和 LOADER）的 GLOBAL 必须指向 .data/.rdata。Python 那边（sigs.py）写的是
            // sig.required in ("REQUIRED", "LOADER")，两边同义（审核 A 备注 2；selftest 的 gate 步骤有 LOADER GLOBAL 的用例）
            if (def.need == SigNeed::Required || def.need == SigNeed::Loader) {
                const char* section = image.SectionOf(t);
                if (!section || (std::strcmp(section, ".data") != 0 && std::strcmp(section, ".rdata") != 0)) {
                    r.problem = std::string("解析出的全局 ") + Rva(t) + " 不在 .data/.rdata（" + (section ? section : "无") + "）";
                    continue;
                }
            }
        } else {
            r.hasResolved = true;
            r.resolved = r.hit;
            // HOOK 要装在函数入口（sigs.py 的 check_hook 同一条），解析结果照样给出，和 Python 一样只把状态判为失效
            if (def.kind == SigKind::Hook) {
                std::string problem = CheckHookEntry(functions(), r.hit);
                if (!problem.empty()) {
                    r.problem = std::move(problem);
                    continue;
                }
            }
        }
        r.ok = true;
    }
    return out;
}

// ============================================================================ 依据求值

const char* StatusText(EvStatus status)
{
    switch (status) {
    case EvStatus::Ok: return "ok";
    case EvStatus::Fail: return "fail";
    case EvStatus::Blocked: return "blocked";
    }
    return "?";
}

Evaluator::Evaluator(const ImageView& image, const Scanner& scanner, const std::vector<SigResult>& sigs, const FunctionTable* table)
    : image_(image), scanner_(scanner), sigs_(sigs), shared_(table)
{
}

const FunctionTable& Evaluator::Table()
{
    if (shared_) {
        return *shared_;
    }
    if (!table_) {
        const double start = NowMicros();
        table_ = std::make_unique<FunctionTable>(image_);
        tableMicros_ = NowMicros() - start;
    }
    return *table_;
}

EvidenceResult Evaluator::Evaluate(const EvidenceDef& ev)
{
    const double start = NowMicros();
    EvidenceResult r = [&] {
        EvidenceResult out;
        const std::int64_t size = static_cast<std::int64_t>(image_.size);
        // 1. 起点
        std::int64_t origin = 0;
        if (ev.origin != Origin::NoSig) {
            const int si = FindSig(ev.sig);
            if (si < 0 || !sigs_[static_cast<std::size_t>(si)].ok) {
                out.status = EvStatus::Blocked;
                out.detail = std::string("特征码 ") + ev.sig + " 失效";
                return out;
            }
            const SigResult& s = sigs_[static_cast<std::size_t>(si)];
            switch (ev.origin) {
            case Origin::Hit: origin = s.hit; break;
            case Origin::Target: origin = s.resolved; break;
            case Origin::HitValue: origin = static_cast<std::int64_t>(s.hit) + ItemValue(ev.item) + ev.arg; break;
            case Origin::VSlot:
            case Origin::VSlotValue: {
                const std::int64_t slot = ev.origin == Origin::VSlot ? ev.arg : ItemValue(ev.item);
                const std::int64_t pos = static_cast<std::int64_t>(s.resolved) + slot * 8;
                if (pos < 0 || pos > size - 8) {
                    out.detail = "虚表槽 " + std::to_string(slot) + " 超出映像";
                    return out;
                }
                std::uint64_t pointer = 0;
                std::memcpy(&pointer, image_.base + pos, 8);
                const auto rva = static_cast<std::int64_t>(pointer - image_.pointerBase);
                if (!image_.InText(rva)) {
                    out.detail = "虚表槽 " + std::to_string(slot) + " 的指针 " + Rva(static_cast<std::int64_t>(pointer)) + " 不在 .text";
                    return out;
                }
                origin = rva;
                break;
            }
            case Origin::NoSig: break;
            }
            out.hasOrigin = true;
            out.origin = origin;
        }
        // 2. 范围和匹配
        Pattern pattern;
        if (!scanner_.Compile(ev.pattern, &pattern)) {
            out.detail = "特征码格式错误";
            return out;
        }
        const std::int64_t length = static_cast<std::int64_t>(pattern.bytes.size());
        std::vector<std::pair<std::uint32_t, std::uint32_t>> chunks;
        std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
        switch (ev.where) {
        case Where::Text: ranges.emplace_back(image_.textBegin, image_.textEnd); break;
        case Where::Entry: ranges.emplace_back(origin, origin + length); break;
        case Where::Window: ranges.emplace_back(origin, origin + ev.span); break;
        case Where::Func:
            if (!Table().Ranges(origin, &chunks)) {
                out.detail = "起点 " + Rva(origin) + " 不在异常目录记录的函数里";
                return out;
            }
            for (const auto& c : chunks) {
                ranges.emplace_back(c.first, c.second);
            }
            break;
        }
        std::vector<std::uint32_t> hits;
        for (const auto& range : ranges) {
            scanner_.Scan(pattern, range.first, range.second, &hits);
        }
        out.hits = static_cast<std::uint32_t>(hits.size());
        if (hits.size() != 1) {
            out.detail = "命中 " + std::to_string(hits.size()) + " 次，要求唯一";
            for (std::size_t i = 0; i < hits.size() && i < 4; ++i) {
                out.detail += (i == 0 ? " " : ",") + Rva(hits[i]);
            }
            return out;
        }
        out.hasHit = true;
        out.hit = hits.front();
        if (ev.read == Read::None) {
            out.status = EvStatus::Ok;
            return out;
        }
        // 3. 读取
        const std::int64_t at = static_cast<std::int64_t>(out.hit) + ev.at;
        std::int64_t captured = 0;
        bool readable = true;
        bool tooWide = false;  // U64 超出 int64 的值：和任何期望值都不相等（Python 是大整数比较）
        const auto raw = [&](int width) -> std::uint64_t {
            if (at < 0 || at + width > size) {
                readable = false;
                return 0;
            }
            std::uint64_t v = 0;
            std::memcpy(&v, image_.base + at, static_cast<std::size_t>(width));
            return v;
        };
        switch (ev.read) {
        case Read::U8: captured = static_cast<std::int64_t>(raw(1)); break;
        case Read::U16: captured = static_cast<std::int64_t>(raw(2)); break;
        case Read::U32: captured = static_cast<std::int64_t>(raw(4)); break;
        case Read::U64: {
            const std::uint64_t v = raw(8);
            tooWide = v > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
            captured = static_cast<std::int64_t>(v);
            break;
        }
        case Read::I8: captured = static_cast<std::int8_t>(raw(1)); break;
        case Read::I32: captured = static_cast<std::int32_t>(raw(4)); break;
        case Read::DerefU32:
        case Read::Rel32: {
            std::int32_t disp = 0;
            if (!ReadI32(image_, at, &disp)) {
                readable = false;
                break;
            }
            const std::int64_t target = at + 4 + disp;
            if (target < 0 || target > size - 4) {
                readable = false;
                break;
            }
            if (ev.read == Read::Rel32) {
                captured = target;
            } else {
                std::uint32_t v = 0;
                ReadU32(image_, target, &v);
                captured = v;
            }
            break;
        }
        case Read::None: break;
        }
        if (!readable) {
            out.detail = "rip 相对的目标超出映像";
            return out;
        }
        out.hasCaptured = true;
        out.captured = captured;
        // 4. 比较
        std::int64_t relValue = 0;
        if (std::strcmp(ev.rel, "NONE") != 0) {
            const int si = FindSig(ev.rel);
            if (si >= 0) {
                const SigResult& s = sigs_[static_cast<std::size_t>(si)];
                if (!s.ok) {
                    out.status = EvStatus::Blocked;
                    out.detail = std::string("特征码 ") + ev.rel + " 失效";
                    return out;
                }
                relValue = s.resolved;
            } else {
                relValue = ItemValue(ev.rel);
            }
        }
        out.hasExpected = true;
        out.expected = ItemValue(ev.item) * ev.mul + relValue + ev.add;
        if (ev.mul != 0) {
            const std::int64_t rest = captured - relValue - ev.add;
            if (rest % ev.mul == 0) {
                out.hasImplied = true;
                out.implied = rest / ev.mul;
            }
        }
        const bool same = !tooWide && captured == out.expected;
        out.status = same ? EvStatus::Ok : EvStatus::Fail;
        if (!same) {
            out.detail = "读到 " + Hex(captured) + "，期望 " + Hex(out.expected);
            if (out.hasImplied && FindItem(ev.item) >= 0) {
                out.detail += "（按读到的值，常量应为 " + Hex(out.implied) + "）";
            }
        }
        return out;
    }();
    r.micros = NowMicros() - start;
    return r;
}

// ============================================================================ 依赖表

namespace {
// 「写」按 ADR 0036 的定义：写游戏内存、调用游戏函数或改变结算；装内联 hook、改虚表槽、调用游戏函数都算（U1 修复 FA，
// 审核 A 的 S1）。只读：只读游戏内存（偏移错时读出错的值或给不出结果），不装 hook、不调用游戏函数
const DepInfo kDepInfo[] = {
    { "core", true },               // DDCore 整体：装每帧 hook。不因「不能核对」停用的例外见 BuildGate
    { "loader", true },             // 加载器的早期 hook（在加载器里判断，DDCore 不管）
    { "events.native", true },      // 调用 ListenerAdd 往游戏的 listener 列表里注册，游戏经布局调用我们的 std::function
    { "events.query", true },       // 同上，回调还能写回 Query 的结果
    { "pvp", true },                // 写操作和 .pre 主题的前提，判错会让 PvP 对局不同步
    { "game.actor", false },        // 句柄解析每次核对虚表，偏移错时只会给不出句柄
    { "game.actor_info", true },    // 调用游戏函数 ActorMaxStat 取最大生命（原型不能运行时核对）
    { "game.actor_list", false },
    { "game.battle", false },
    { "game.mods", false },
    { "combat.buff_read", false },  // 只读 Buff 库的 map 和 actor 的 buff 表，不调用 BuffLibraryIndex（它会插入）
    { "combat.buff_add", true },
    { "combat.buff_remove", true },
    { "combat.buff_define", true },
    { "combat.effect_get", false },
    { "combat.effect_apply", true },
    { "combat.effect_define", true },
    { "combat.rule", true },        // 装规则 hook，判定的结果交还游戏，改变结算
    { "combat.skill", false },
    { "combat.enum", false },       // 枚举表本身只读；写操作的参数校验按 buff_define 等词停用
    { "persist", true },            // 在存档读写等 4 个函数上装内联 hook，按固定的参数个数转发给原函数
    { "check", false },             // 只给校验用，DDCore 不用
};
static_assert(std::size(kDepInfo) == static_cast<std::size_t>(Dep::Count), "依赖表词汇和 Dep 不一致");

std::vector<std::string_view> SplitWords(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && text[i] != ' ') {
            ++i;
        }
        if (i > start) {
            out.push_back(text.substr(start, i - start));
        }
    }
    return out;
}

bool TopicMatches(std::string_view pattern, std::string_view name)
{
    if (!pattern.empty() && pattern.back() == '*') {
        const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
        return name.substr(0, prefix.size()) == prefix;
    }
    return pattern == name;
}
}  // namespace

const DepInfo& InfoOf(Dep dep)
{
    return kDepInfo[static_cast<std::size_t>(dep)];
}

bool ParseDep(std::string_view word, Dep* out)
{
    for (std::size_t i = 0; i < std::size(kDepInfo); ++i) {
        if (word == kDepInfo[i].word) {
            *out = static_cast<Dep>(i);
            return true;
        }
    }
    return false;
}

void Gate::Disable(Dep dep, const std::string& reason)
{
    const auto i = static_cast<std::size_t>(dep);
    if (!off_[i]) {
        off_[i] = true;
        reason_[i] = reason;
    } else if (reason_[i].find(reason) == std::string::npos) {
        reason_[i] += "；" + reason;
    }
}

void Gate::DisableTopic(const std::string& pattern, bool writeOnly, const std::string& reason)
{
    for (auto& rule : topics_) {
        if (rule.pattern == pattern && rule.writeOnly == writeOnly) {
            if (rule.reason.find(reason) == std::string::npos) {
                rule.reason += "；" + reason;
            }
            return;
        }
    }
    topics_.push_back(TopicRule{ pattern, writeOnly, reason });
}

bool Gate::TopicOff(std::string_view name, bool writes, std::string* reason) const
{
    for (const auto& rule : topics_) {
        if ((!rule.writeOnly || writes) && TopicMatches(rule.pattern, name)) {
            if (reason) {
                *reason = rule.reason;
            }
            return true;
        }
    }
    return false;
}

bool Gate::Any() const
{
    return !topics_.empty() || std::any_of(std::begin(off_), std::end(off_), [](bool b) { return b; });
}

// ============================================================================ 核对和策略

const char* StateText(ItemState state)
{
    switch (state) {
    case ItemState::Ok: return "ok";
    case ItemState::Fail: return "fail";
    case ItemState::Blocked: return "blocked";
    case ItemState::Offline: return "offline";
    case ItemState::Unverified: return "unverified";
    }
    return "?";
}

CheckReport RunCheck(const ImageView& image, const Scanner& scanner, const std::vector<SigResult>& sigs, const FunctionTable* table)
{
    CheckReport report;
    const double start = NowMicros();
    Evaluator evaluator(image, scanner, sigs, table);
    const auto& evidence = Evidence();
    report.evidence.reserve(evidence.size());
    for (const auto& ev : evidence) {
        report.evidence.push_back(evaluator.Evaluate(ev));
    }
    // 特征码的同一性依据（id 是特征码名字）不成立：这条特征码可能绑到了别的函数
    std::set<std::string_view> misbound;
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        if (FindItem(evidence[i].item) < 0 && FindSig(evidence[i].item) >= 0 && report.evidence[i].status != EvStatus::Ok) {
            misbound.insert(evidence[i].item);
        }
    }
    for (const auto& name : misbound) {
        report.misbound.emplace_back(name);
    }
    const auto& items = Items();
    report.items.assign(items.size(), ItemState::Ok);
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].check == ItemCheck::Offline) {
            report.items[i] = ItemState::Offline;
        } else if (items[i].check == ItemCheck::None) {
            report.items[i] = ItemState::Unverified;
        }
    }
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        const int item = FindItem(evidence[i].item);
        if (item < 0 || items[static_cast<std::size_t>(item)].check != ItemCheck::Runtime) {
            continue;
        }
        auto& state = report.items[static_cast<std::size_t>(item)];
        const EvStatus status = report.evidence[i].status;
        const bool onMisbound = misbound.count(evidence[i].sig) || misbound.count(evidence[i].rel);
        if (status == EvStatus::Fail) {
            state = ItemState::Fail;
        } else if ((status == EvStatus::Blocked || onMisbound) && state == ItemState::Ok) {
            state = ItemState::Blocked;
        }
    }
    report.tableMicros = evaluator.FunctionTableMicros();
    report.micros = NowMicros() - start;
    return report;
}

Gate BuildGate(const CheckReport& report, const std::vector<SigResult>& sigs, bool knownBuild, std::vector<PolicyLine>* log)
{
    (void)sigs;
    Gate gate;
    const auto& items = Items();
    const auto& evidence = Evidence();
    const auto say = [&](int level, std::string text) {
        if (log) {
            log->push_back(PolicyLine{ level, std::move(text) });
        }
    };
    // 不能核对的项按词汇汇总：写的能力停用，只读的警告
    std::map<Dep, std::vector<std::string>> unverifiedWrites, unverifiedReads;
    std::map<std::string, std::vector<std::string>> unverifiedTopics;
    std::vector<std::string> coreExempt, loaderItems;  // core 的例外、加载器的项（见 layout_check.h 的 BuildGate）
    std::size_t acceptedOffline = 0, acceptedNone = 0;

    const auto failDeps = [&](const ItemDef& item, const std::string& why) {
        std::string disabled;
        for (const auto word : SplitWords(item.deps)) {
            if (word.substr(0, 6) == "topic:") {
                gate.DisableTopic(std::string(word.substr(6)), false, std::string("布局项 ") + item.id + " " + why);
            } else if (Dep dep{}; ParseDep(word, &dep)) {
                if (dep == Dep::Check) {
                    continue;
                }
                if (dep == Dep::Loader) {
                    loaderItems.push_back(item.id);
                    continue;
                }
                gate.Disable(dep, std::string("布局项 ") + item.id + " " + why);
            } else {
                say(2, std::string("布局项 ") + item.id + " 的依赖表里有不认识的词 " + std::string(word));
                continue;
            }
            disabled += (disabled.empty() ? "" : " ") + std::string(word);
        }
        return disabled;
    };
    // byDesign：只能离线核对或没有静态依据（未知版本上）；false 是依据没法求值（锚点特征码失效或误绑定）
    const auto unverifiable = [&](const ItemDef& item, bool byDesign) {
        for (const auto word : SplitWords(item.deps)) {
            if (word.substr(0, 6) == "topic:") {
                unverifiedTopics[std::string(word.substr(6))].push_back(item.id);
            } else if (Dep dep{}; ParseDep(word, &dep)) {
                if (dep == Dep::Check) {
                    continue;
                }
                if (dep == Dep::Loader) {
                    loaderItems.push_back(item.id);
                    continue;
                }
                if (dep == Dep::Core && byDesign) {
                    coreExempt.push_back(item.id);
                    continue;
                }
                (InfoOf(dep).writes ? unverifiedWrites : unverifiedReads)[dep].push_back(item.id);
            } else {
                say(2, std::string("布局项 ") + item.id + " 的依赖表里有不认识的词 " + std::string(word));
            }
        }
    };

    for (std::size_t i = 0; i < items.size(); ++i) {
        const ItemDef& item = items[i];
        switch (report.items[i]) {
        case ItemState::Ok:
            break;
        case ItemState::Fail: {
            std::string why;
            for (std::size_t e = 0; e < evidence.size(); ++e) {
                if (std::string_view(evidence[e].item) == item.id && report.evidence[e].status == EvStatus::Fail) {
                    why = "的依据 " + std::to_string(evidence[e].index) + "（" + Describe(evidence[e]) + "）不成立：" +
                        report.evidence[e].detail;
                    break;
                }
            }
            const std::string disabled = failDeps(item, "的运行时依据不成立");
            say(2, std::string("布局项 ") + item.id + "（" + item.type + " " + Hex(item.value) + "）" + why + "，停用 " + disabled);
            break;
        }
        case ItemState::Blocked: {
            std::string why;
            for (std::size_t e = 0; e < evidence.size(); ++e) {
                if (std::string_view(evidence[e].item) == item.id && report.evidence[e].status != EvStatus::Ok) {
                    why = report.evidence[e].detail;
                    break;
                }
            }
            if (why.empty()) {
                why = "依据的锚点特征码同一性不成立";
            }
            say(1, std::string("布局项 ") + item.id + " 的依据没法求值（" + why + "），按不能核对处理");
            unverifiable(item, false);
            break;
        }
        case ItemState::Offline:
        case ItemState::Unverified:
            if (knownBuild) {
                (report.items[i] == ItemState::Offline ? acceptedOffline : acceptedNone) += 1;
            } else {
                unverifiable(item, true);
            }
            break;
        }
    }
    // 同一性依据不成立的特征码：它的地址不用（game::Resolve 已经置空），依赖它的能力和特征码缺失时一样停用
    for (const auto& name : report.misbound) {
        const int si = FindSig(name);
        if (si < 0) {
            continue;
        }
        std::string disabled;
        for (const auto word : SplitWords(Signatures()[static_cast<std::size_t>(si)].deps)) {
            if (word.substr(0, 6) == "topic:") {
                gate.DisableTopic(std::string(word.substr(6)), false, "特征码 " + name + " 的同一性依据不成立");
            } else if (Dep dep{}; ParseDep(word, &dep) && dep != Dep::Check && dep != Dep::Loader) {
                gate.Disable(dep, "特征码 " + name + " 的同一性依据不成立");
            } else {
                continue;
            }
            disabled += (disabled.empty() ? "" : " ") + std::string(word);
        }
        say(2, "特征码 " + name + " 的同一性依据不成立（可能绑到了别的函数），不用它的地址，停用 " + disabled);
    }

    const auto joinIds = [](const std::vector<std::string>& ids) {
        std::string text;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i == 8) {
                text += " 等 " + std::to_string(ids.size()) + " 项";
                break;
            }
            text += (i ? "、" : "") + ids[i];
        }
        return text;
    };
    const char* scope = knownBuild ? "依据没法求值" : "未知版本上不能运行时核对";
    for (const auto& [dep, ids] : unverifiedWrites) {
        const std::string reason = std::string(scope) + "：" + joinIds(ids);
        gate.Disable(dep, reason);
        say(2, std::string("停用 ") + InfoOf(dep).word + "（会写游戏内存或调用游戏函数），依赖的布局项" + reason);
    }
    for (const auto& [dep, ids] : unverifiedReads) {
        if (gate.Off(dep)) {
            continue;
        }
        say(1, std::string("只读能力 ") + InfoOf(dep).word + " 照常，依赖的布局项" + scope + "：" + joinIds(ids));
    }
    for (const auto& [pattern, ids] : unverifiedTopics) {
        gate.DisableTopic(pattern, true, std::string(scope) + "：" + joinIds(ids));
    }
    if (!unverifiedTopics.empty()) {
        // 实际停用了哪些主题要等主题都注册完才知道，由 DDCore 逐个记日志（「主题 X 停用」）
        say(1, "依赖的布局项" + std::string(scope) + "的主题规则 " + std::to_string(unverifiedTopics.size()) +
            " 条：其中可写、可取消或背后装 hook、改虚表槽的主题停用，只读的照常");
    }
    if (!coreExempt.empty()) {
        say(1, "DDCore 本身依赖的布局项" + std::string(scope) + "：" + joinIds(coreExempt) +
            "；按 ADR 0036 的例外照常（按「写」停用就等于未知版本 DDCore 整体停用）");
    }
    if (!loaderItems.empty()) {
        say(1, "加载器依赖的布局项（" + joinIds(loaderItems) +
            "）由加载器自己核对（唯一命中、DD_ENTRY、函数入口、偷取字节数），加载器不读布局清单，DDCore 不管");
    }
    if (knownBuild && (acceptedOffline || acceptedNone)) {
        say(0, "已知版本：只能离线核对的 " + std::to_string(acceptedOffline) + " 项、没有静态依据的 " + std::to_string(acceptedNone) +
            " 项按 accept 时的离线核对和自检处理，不停用");
    }
    return gate;
}

}  // namespace ddc::game::check
