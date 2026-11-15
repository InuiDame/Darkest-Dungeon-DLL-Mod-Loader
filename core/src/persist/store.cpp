#include "store.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ddc::persist {

std::uint64_t Fnv1a64(const void* data, std::size_t size)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

bool ValidKey(std::string_view key)
{
    if (key.empty() || key.size() > DD_PERSIST_MAX_KEY || key[0] == '.') {
        return false;
    }
    for (const char ch : key) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool ValidDevice(std::string_view device)
{
    if (device.empty() || device.size() > 63) {
        return false;
    }
    for (const char ch : device) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool SameIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool ValidPluginDir(std::string_view name, std::string* why)
{
    const auto fail = [&](const char* text) {
        if (why) {
            *why = text;
        }
        return false;
    };
    if (!ValidKey(name)) {
        return fail("只能用字母数字和 _ . -，1 到 63 字节，不能以点开头");
    }
    if (name.back() == '.') {
        return fail("不能以点结尾（Windows 建目录时会去掉结尾的点，和别的名字撞目录）");
    }
    if (SameIgnoreCase(name, "backup")) {
        return fail("和选存档槽时同步的 backup 目录同名");
    }
    if (SameIgnoreCase(name, "DDCore")) {
        return fail("和 DDCore 自己的标签表目录同名");
    }
    return true;
}

bool ShouldObserve(std::string_view name, std::uint32_t size)
{
    return name.rfind("persist.", 0) == 0 && size <= kMaxObserved;
}

const FileHash* Header::Find(std::string_view name) const
{
    for (const auto& file : files) {
        if (file.name == name) {
            return &file;
        }
    }
    return nullptr;
}

namespace {

// 头里的值是一行：换行、回车换成空格，最多 255 字节
std::string OneLine(std::string_view text)
{
    std::string out(text.substr(0, 255));
    for (char& ch : out) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    return out;
}

std::string Hex16(std::uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));
    return text;
}

bool ParseU64(std::string_view text, int base, std::uint64_t* out)
{
    if (text.empty() || text.size() > 20) {
        return false;
    }
    const std::string copy(text);
    char* end = nullptr;
    *out = std::strtoull(copy.c_str(), &end, base);
    return end && *end == 0;
}

}  // namespace

std::string EncodeFile(const Header& header, std::string_view payload)
{
    std::string out;
    out.reserve(payload.size() + 512);
    out += "DDSAVE " + std::to_string(kFormat) + "\n";
    out += "plugin " + OneLine(header.plugin) + "\n";
    out += "key " + OneLine(header.key) + "\n";
    out += "version " + OneLine(header.version) + "\n";
    out += "device " + OneLine(header.device) + "\n";
    out += "account " + OneLine(header.account) + "\n";
    out += "written " + std::to_string(header.writtenMs) + "\n";
    out += std::string("in_raid ") + (header.inRaid ? "1" : "0") + "\n";
    out += std::string("in_battle ") + (header.inBattle ? "1" : "0") + "\n";
    for (const auto& file : header.files) {
        out += "file " + OneLine(file.name) + " " + Hex16(file.hash) + " " + std::to_string(file.size) + "\n";
    }
    out += "payload " + std::to_string(payload.size()) + " " + Hex16(Fnv1a64(payload.data(), payload.size())) + "\n";
    out += "\n";
    out.append(payload.data(), payload.size());
    return out;
}

bool DecodeFile(std::string_view bytes, Header* header, std::string* payload, std::string* why)
{
    const auto fail = [&](std::string text) {
        if (why) {
            *why = std::move(text);
        }
        return false;
    };
    Header h;
    std::size_t pos = 0;
    bool first = true;
    bool havePayload = false, haveDevice = false, haveWritten = false;
    std::uint64_t payloadSize = 0, payloadHash = 0;
    while (true) {
        const std::size_t end = bytes.find('\n', pos);
        if (end == std::string_view::npos) {
            return fail(first ? "不是旁路文件（没有文件头）" : "文件头没有结束（文件被截断？）");
        }
        const std::string_view line = bytes.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) {
            break;
        }
        const std::size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
        if (first) {
            first = false;
            std::uint64_t format = 0;
            if (key != "DDSAVE" || !ParseU64(value, 10, &format)) {
                return fail("不是旁路文件（第一行不是 DDSAVE）");
            }
            if (format != kFormat) {
                return fail("格式版本 " + std::string(value) + " 不认识（DDCore 是 " + std::to_string(kFormat) + "）");
            }
            h.format = static_cast<std::uint32_t>(format);
            continue;
        }
        if (key == "plugin") {
            h.plugin = value;
        } else if (key == "key") {
            h.key = value;
        } else if (key == "version") {
            h.version = value;
        } else if (key == "device") {
            h.device = value;
            haveDevice = true;
        } else if (key == "account") {
            h.account = value;
        } else if (key == "written") {
            haveWritten = ParseU64(value, 10, &h.writtenMs);
        } else if (key == "in_raid") {
            h.inRaid = value == "1";
        } else if (key == "in_battle") {
            h.inBattle = value == "1";
        } else if (key == "file") {
            // file <名字> <哈希> <大小>
            const std::size_t a = value.find(' ');
            const std::size_t b = a == std::string_view::npos ? a : value.find(' ', a + 1);
            FileHash file;
            std::uint64_t size = 0;
            if (b == std::string_view::npos || !ParseU64(value.substr(a + 1, b - a - 1), 16, &file.hash) ||
                !ParseU64(value.substr(b + 1), 10, &size) || size > 0xFFFFFFFFull) {
                return fail("文件头的 file 行格式不对：" + std::string(value));
            }
            file.name = value.substr(0, a);
            file.size = static_cast<std::uint32_t>(size);
            h.files.push_back(std::move(file));
        } else if (key == "payload") {
            const std::size_t a = value.find(' ');
            if (a == std::string_view::npos || !ParseU64(value.substr(0, a), 10, &payloadSize) ||
                !ParseU64(value.substr(a + 1), 16, &payloadHash)) {
                return fail("文件头的 payload 行格式不对");
            }
            havePayload = true;
        }
        // 不认识的键忽略
    }
    if (!havePayload || !haveDevice || !haveWritten || h.plugin.empty() || h.key.empty()) {
        return fail("文件头缺少必需的项（plugin、key、device、written、payload）");
    }
    const std::size_t rest = bytes.size() - pos;
    if (rest < payloadSize) {
        return fail("数据只有 " + std::to_string(rest) + " 字节，头里写的是 " + std::to_string(payloadSize) + "（文件被截断，写到一半？）");
    }
    if (rest > payloadSize) {
        return fail("数据比头里写的长 " + std::to_string(rest - payloadSize) + " 字节");
    }
    const std::string_view data = bytes.substr(pos);
    if (Fnv1a64(data.data(), data.size()) != payloadHash) {
        return fail("数据的校验和不符（文件损坏）");
    }
    if (header) {
        *header = std::move(h);
    }
    if (payload) {
        payload->assign(data.data(), data.size());
    }
    return true;
}

// ---- 一致性 ----

bool Matches(const Header& header, const Observed& observed, std::string* why)
{
    const auto fail = [&](std::string text) {
        if (why) {
            *why = std::move(text);
        }
        return false;
    };
    // 文件头的插件名和 key 要和请求的完全相同：只差大小写的另一个插件或 key 在 NTFS 上落到同一个文件（审核 S5）
    if (!observed.plugin.empty() && (header.plugin != observed.plugin || header.key != observed.key)) {
        return fail("文件头写的是 " + header.plugin + "/" + header.key + "，不是 " + observed.plugin + "/" + observed.key +
            "（大小写不同的另一个插件或 key 写的？）");
    }
    if (header.device != observed.device) {
        return fail("设备名不同（旁路 " + header.device + "，存档 " + observed.device + "）");
    }
    const FileHash* roster = header.Find(kRosterFile);
    if (!roster) {
        return fail("旁路没有记录名册的哈希");
    }
    if (roster->hash != observed.roster.hash || roster->size != observed.roster.size) {
        return fail("名册的哈希不同（旁路 " + Hex16(roster->hash) + "/" + std::to_string(roster->size) + " 字节，存档 " +
            Hex16(observed.roster.hash) + "/" + std::to_string(observed.roster.size) + " 字节）");
    }
    if (observed.inRaid >= 0 && header.inRaid != (observed.inRaid == 1)) {
        return fail(header.inRaid ? "旁路写于地牢里，存档在城镇" : "旁路写于城镇，存档在地牢里");
    }
    if (header.inRaid && !header.Find(kRaidFile)) {
        return fail("旁路写于地牢里，但没有记录 raid 的哈希");
    }
    if (header.inRaid && observed.inRaid == 1 && observed.raidKnown) {
        return RaidMatches(header, observed.raid, why);
    }
    return true;
}

bool RaidMatches(const Header& header, const FileHash& raid, std::string* why)
{
    const FileHash* recorded = header.Find(kRaidFile);
    if (!recorded) {
        if (why) {
            *why = "旁路没有记录 raid 的哈希";
        }
        return false;
    }
    if (recorded->hash != raid.hash || recorded->size != raid.size) {
        if (why) {
            *why = "探索存档（persist.raid.json）的哈希不同（旁路 " + Hex16(recorded->hash) + "/" + std::to_string(recorded->size) + " 字节，存档 " +
                Hex16(raid.hash) + "/" + std::to_string(raid.size) + " 字节）";
        }
        return false;
    }
    return true;
}

Decision Decide(const Candidate& current, const Candidate& prev, const Observed& observed)
{
    Decision d;
    if (!observed.rosterKnown) {
        d.status = DD_PERSIST_UNAVAILABLE;
        d.reason = "这次读档没有观察到名册（persist.roster.json）的读取";
        return d;
    }
    if (!current.exists && !prev.exists) {
        d.status = DD_PERSIST_EMPTY;
        d.reason = "没有旁路文件";
        return d;
    }
    const auto judge = [&](const Candidate& c, std::string* why) {
        if (!c.exists) {
            *why = "没有文件";
            return false;
        }
        if (!c.decoded) {
            *why = "文件损坏：" + c.error;
            return false;
        }
        return Matches(c.header, observed, why);
    };
    std::string whyCurrent, whyPrev;
    if (judge(current, &whyCurrent)) {
        d.status = DD_PERSIST_LOADED;
        d.which = 0;
        return d;
    }
    if (judge(prev, &whyPrev)) {
        d.status = DD_PERSIST_PREV;
        d.which = 1;
        d.reason = "当前一代不一致（" + whyCurrent + "），用上一代";
        return d;
    }
    if (observed.newCampaign) {
        // 玩家删了存档位再开新战役：游戏删存档位不经过观察 hook，旁路里还是以前那个战役的（审核 S6）
        d.status = DD_PERSIST_EMPTY;
        d.reason = "新战役（这次读到的名册就是 scripts/starting_save 的初始名册），旁路里是这个存档位以前的战役，不用";
        d.archive = "oldcampaign";
        return d;
    }
    d.status = DD_PERSIST_MISMATCH;
    d.reason = "当前一代：" + whyCurrent + "；上一代：" + whyPrev;
    d.archive = "mismatch";
    return d;
}

bool SameContent(const std::wstring& path, const FileHash& expect)
{
    std::string bytes;
    bool exists = false;
    if (!ReadWhole(path, &bytes, &exists, nullptr, kMaxObserved) || !exists || bytes.size() != expect.size) {
        return false;
    }
    return Fnv1a64(bytes.data(), bytes.size()) == expect.hash;
}

// ---- 文件 ----

std::wstring Widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring LongPath(const std::wstring& path)
{
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + normalized.substr(2);
    }
    return L"\\\\?\\" + normalized;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(LongPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& dir)
{
    if (dir.empty()) {
        return false;
    }
    const std::wstring full = LongPath(dir);
    const DWORD attributes = GetFileAttributesW(full.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0 && dir[slash - 1] != L':') {
        if (!EnsureDir(dir.substr(0, slash))) {
            return false;
        }
    }
    return CreateDirectoryW(full.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadWhole(const std::wstring& path, std::string* out, bool* exists, std::string* why, std::size_t limit)
{
    *exists = false;
    out->clear();
    HANDLE file = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        if (why) {
            *why = "打开失败（错误码 " + std::to_string(error) + "）";
        }
        return false;
    }
    *exists = true;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || static_cast<std::uint64_t>(size.QuadPart) > limit) {
        CloseHandle(file);
        if (why) {
            *why = "文件太大或读不出大小";
        }
        return false;
    }
    out->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out->empty() || (ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    CloseHandle(file);
    if (!ok) {
        if (why) {
            *why = "读取失败";
        }
        out->clear();
        return false;
    }
    return true;
}

Candidate ReadCandidate(const std::wstring& path)
{
    Candidate c;
    std::string bytes, why;
    if (!ReadWhole(path, &bytes, &c.exists, &why)) {
        c.exists = true;
        c.error = why;
        return c;
    }
    if (!c.exists) {
        return c;
    }
    c.decoded = DecodeFile(bytes, &c.header, &c.payload, &c.error);
    return c;
}

namespace {

std::string ErrorText(const char* what, DWORD error)
{
    return std::string(what) + "（错误码 " + std::to_string(error) + "）";
}

// 去掉只读属性（别的程序或用户设的）。文件不存在时什么都不做
void ClearReadOnly(const std::wstring& longPath)
{
    const DWORD attributes = GetFileAttributesW(longPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) && (attributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(longPath.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }
}

}  // namespace

bool WriteAtomic(const std::wstring& path, std::string_view bytes, std::string* why, const FailHook& fail)
{
    const auto failWith = [&](std::string text) {
        if (why) {
            *why = std::move(text);
        }
        return false;
    };
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !EnsureDir(path.substr(0, slash))) {
        return failWith(ErrorText("创建目录失败", GetLastError()));
    }
    const std::wstring tmp = path + L".tmp";
    const std::wstring prev = path + L".prev";
    const std::wstring tmpLong = LongPath(tmp);
    const std::wstring pathLong = LongPath(path);
    const std::wstring prevLong = LongPath(prev);
    const auto dropTmp = [&] { DeleteFileW(tmpLong.c_str()); };
    // 只读的 .tmp 让 CREATE_ALWAYS 失败，只读的 .prev 或当前文件让替换失败，之后每次都失败（审核 S8）
    ClearReadOnly(tmpLong);
    ClearReadOnly(pathLong);
    ClearReadOnly(prevLong);
    if (fail && fail("create")) {
        return failWith("创建临时文件失败（测试注入）");
    }
    HANDLE file = CreateFileW(tmpLong.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return failWith(ErrorText("创建临时文件失败", GetLastError()));
    }
    // 分块写；失败注入时写一半就停（模拟写到一半崩溃或磁盘满）
    std::size_t done = 0;
    const std::size_t stopAt = fail && fail("tmp_write") ? bytes.size() / 2 : bytes.size();
    while (done < stopAt) {
        const DWORD chunk = static_cast<DWORD>((std::min)(stopAt - done, static_cast<std::size_t>(1u << 20)));
        DWORD wrote = 0;
        if (!WriteFile(file, bytes.data() + done, chunk, &wrote, nullptr) || wrote != chunk) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            dropTmp();
            return failWith(ErrorText("写临时文件失败", error));
        }
        done += chunk;
    }
    if (stopAt != bytes.size()) {
        CloseHandle(file);
        dropTmp();
        return failWith("写临时文件失败（测试注入：写到一半）");
    }
    if ((fail && fail("flush")) || !FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        dropTmp();
        return failWith(ErrorText("FlushFileBuffers 失败", error));
    }
    CloseHandle(file);
    if (!FileExists(path)) {
        if (!MoveFileExW(tmpLong.c_str(), pathLong.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            dropTmp();
            return failWith(ErrorText("改名失败", error));
        }
        return true;
    }
    const bool injected = fail && fail("replace");
    if (!injected && ReplaceFileW(pathLong.c_str(), tmpLong.c_str(), prevLong.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        return true;
    }
    // ReplaceFileW 失败（被别的进程占着、文件系统不支持等）：退回两次改名。1175 时两个文件都没动；1176 时两个文件都还是原名；
    // 1177（ERROR_UNABLE_TO_MOVE_REPLACEMENT_2）时原文件已经成了 .prev，第一次改名跳过
    const DWORD replaceError = injected ? ERROR_UNABLE_TO_REMOVE_REPLACED : GetLastError();
    const std::string replaceText = injected ? std::string("ReplaceFileW 失败（测试注入）") : ErrorText("ReplaceFileW 失败", replaceError);
    if (FileExists(path)) {
        if ((fail && fail("move_prev")) || !MoveFileExW(pathLong.c_str(), prevLong.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            dropTmp();
            return failWith(replaceText + "，把旧文件改名为 .prev 也失败" + ErrorText("", error));
        }
    }
    if ((fail && fail("move_new")) || !MoveFileExW(tmpLong.c_str(), pathLong.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        dropTmp();
        return failWith(replaceText + "，改名也失败" + ErrorText("", error) + "（原来的文件已经成了 .prev，这次的数据没写进去）");
    }
    return true;
}

namespace {

// 目录里的文件名（不含子目录）
std::vector<std::wstring> ListFiles(const std::wstring& dir);

}  // namespace

bool ArchiveGenerations(const std::wstring& path, std::string_view tag, std::string_view stamp, std::size_t keep, std::vector<std::wstring>* moved,
    std::string* why)
{
    const std::wstring suffix = L"." + Widen(tag) + L"-" + Widen(stamp);
    const std::wstring sources[2] = { path, path + L".prev" };
    const std::wstring targets[2] = { path + suffix, path + L".prev" + suffix };
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        if (!FileExists(sources[i])) {
            continue;
        }
        if (!MoveFileExW(LongPath(sources[i]).c_str(), LongPath(targets[i]).c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (why) {
                *why = ErrorText(("改名失败：" + Narrow(sources[i]) + " → " + Narrow(targets[i])).c_str(), GetLastError());
            }
            ok = false;
            break;
        }
        if (moved) {
            moved->push_back(targets[i]);
        }
    }
    // 同一 tag 只留最近 keep 份：<文件名>.<tag>-<stamp> 和 <文件名>.prev.<tag>-<stamp> 按 stamp 归成一份
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
    const std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::wstring heads[2] = { base + L"." + Widen(tag) + L"-", base + L".prev." + Widen(tag) + L"-" };
    std::map<std::wstring, std::vector<std::wstring>> byStamp;
    for (const auto& name : ListFiles(dir)) {
        for (const auto& head : heads) {
            if (name.size() > head.size() && name.compare(0, head.size(), head) == 0) {
                byStamp[name.substr(head.size())].push_back(name);
            }
        }
    }
    std::size_t kept = 0;
    for (auto it = byStamp.rbegin(); it != byStamp.rend(); ++it) {
        if (++kept <= keep) {
            continue;
        }
        for (const auto& name : it->second) {
            DeleteFileW(LongPath(dir + L"\\" + name).c_str());
        }
    }
    return ok;
}

// ---- 后台写盘 ----

Writer::Writer(LogFn log) : log_(std::move(log))
{
    thread_ = std::thread([this] { Run(); });
}

Writer::~Writer()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        paused_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Writer::Submit(std::wstring path, std::string bytes)
{
    {
        std::lock_guard lock(mutex_);
        ++stats_.submitted;
        auto it = jobs_.find(path);
        if (it != jobs_.end()) {
            // 还没写（或在等重试）：换成最新的，位置、失败次数和重试时间不变
            it->second.bytes = std::move(bytes);
            ++stats_.merged;
        } else {
            Job job;
            job.bytes = std::move(bytes);
            job.due = std::chrono::steady_clock::now();
            jobs_.emplace(path, std::move(job));
            order_.push_back(std::move(path));
        }
    }
    wake_.notify_one();
}

bool Writer::Flush(std::uint32_t timeoutMs)
{
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return order_.empty() && !busy_; });
}

std::vector<std::wstring> Writer::Failures() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::wstring> out;
    for (const auto& [path, count] : failures_) {
        out.push_back(path);
    }
    return out;
}

void Writer::SetRetryDelays(std::vector<std::uint32_t> delaysMs)
{
    {
        std::lock_guard lock(mutex_);
        delays_ = std::move(delaysMs);
    }
    wake_.notify_all();
}

void Writer::SetPaused(bool paused)
{
    {
        std::lock_guard lock(mutex_);
        paused_ = paused;
    }
    wake_.notify_all();
}

void Writer::SetFailHook(FailHook hook)
{
    std::lock_guard lock(mutex_);
    fail_ = std::move(hook);
}

Writer::Stats Writer::GetStats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t Writer::Pending() const
{
    std::lock_guard lock(mutex_);
    return order_.size() + (busy_ ? 1 : 0);
}

void Writer::Run()
{
    using Clock = std::chrono::steady_clock;
    std::unique_lock lock(mutex_);
    while (true) {
        // 第一个到期的任务；析构时（stop_）不再等重试
        const auto now = Clock::now();
        auto ready = order_.end();
        auto next = (Clock::time_point::max)();
        if (!paused_) {
            for (auto it = order_.begin(); it != order_.end(); ++it) {
                const Job& job = jobs_.at(*it);
                if (stop_ || job.due <= now) {
                    ready = it;
                    break;
                }
                next = (std::min)(next, job.due);
            }
        }
        if (ready == order_.end()) {
            if (stop_ && order_.empty()) {
                break;
            }
            if (paused_ || next == (Clock::time_point::max)()) {
                wake_.wait(lock);
            } else {
                wake_.wait_until(lock, next);
            }
            continue;
        }
        std::wstring path = std::move(*ready);
        order_.erase(ready);
        auto node = jobs_.extract(path);
        Job job = std::move(node.mapped());
        FailHook fail = fail_;
        busy_ = true;
        lock.unlock();
        std::string why;
        bool ok = false;
        try {
            ok = WriteAtomic(path, job.bytes, &why, fail);
        } catch (...) {
            why = "写盘时出了 C++ 异常";
        }
        lock.lock();
        int level = 0;
        std::string message;
        if (ok) {
            ++stats_.written;
            if (failures_.erase(path)) {
                level = DD_LOG_INFO;
                message = "旁路写盘恢复正常：" + Narrow(path);
            }
        } else if (jobs_.count(path)) {
            // 写的时候同一路径又提交了新的：新的接替这次失败的（失败次数从新的算起）
            ++stats_.retries;
            level = DD_LOG_WARN;
            message = "旁路写盘失败，同一个文件已经有更新的数据排着，改写新的：" + Narrow(path) + "：" + why;
        } else if (!stop_ && job.attempts < delays_.size()) {
            const std::uint32_t delay = delays_[job.attempts];
            ++job.attempts;
            job.due = Clock::now() + std::chrono::milliseconds(delay);
            ++stats_.retries;
            if (job.attempts == 1) {
                level = DD_LOG_WARN;
                message = "旁路写盘失败，" + std::to_string(delay) + " 毫秒后重试（最多再试 " + std::to_string(delays_.size()) + " 次）：" + Narrow(path) +
                    "：" + why;
            }
            jobs_.emplace(path, std::move(job));
            order_.push_back(path);
        } else {
            ++stats_.failed;
            const std::uint64_t times = ++failures_[path];
            // 一直写不进去的文件（只读的目录、长期占着它的程序）每次存档都会放弃一次，日志只写第一次和之后每 20 次
            if (times == 1 || times % 20 == 0) {
                level = DD_LOG_ERROR;
                message = "旁路写盘失败，重试 " + std::to_string(job.attempts) + " 次后放弃（这个文件连续放弃 " + std::to_string(times) +
                    " 次，下次存档再写）：" + Narrow(path) + "：" + why;
            }
        }
        if (!message.empty() && log_) {
            // 日志在锁外写（回调里可能再拿别的锁）；busy_ 到写完日志才清，Flush 返回时日志已经在了
            lock.unlock();
            log_(level, message);
            lock.lock();
        }
        busy_ = false;
        if (order_.empty()) {
            idle_.notify_all();
        }
    }
    idle_.notify_all();
}

// ---- backup ----

namespace {

bool IsSaveFile(const std::wstring& name)
{
    const auto ends = [&](const wchar_t* suffix) {
        const std::size_t n = std::wcslen(suffix);
        return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
    };
    return ends(L".ddsave") || ends(L".ddsave.prev");
}

template <class Fn>
void ForEachEntry(const std::wstring& dir, Fn&& fn)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(LongPath(dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        fn(name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

std::vector<std::wstring> ListFiles(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    ForEachEntry(dir, [&](const std::wstring& name, bool isDir) {
        if (!isDir) {
            out.push_back(name);
        }
    });
    return out;
}

}  // namespace

bool MirrorDir(const std::wstring& src, const std::wstring& dst, std::size_t* copied, std::string* why)
{
    std::size_t count = 0;
    bool ok = true;
    std::vector<std::wstring> plugins;
    ForEachEntry(src, [&](const std::wstring& name, bool dir) {
        if (dir && _wcsicmp(name.c_str(), L"backup") != 0) {
            plugins.push_back(name);
        }
    });
    for (const auto& plugin : plugins) {
        const std::wstring from = src + L"\\" + plugin;
        const std::wstring to = dst + L"\\" + plugin;
        std::vector<std::wstring> files;
        ForEachEntry(from, [&](const std::wstring& name, bool dir) {
            if (!dir && IsSaveFile(name)) {
                files.push_back(name);
            }
        });
        if (files.empty()) {
            continue;
        }
        if (!EnsureDir(to)) {
            ok = false;
            if (why) {
                *why = "创建 backup 目录失败：" + Narrow(to);
            }
            continue;
        }
        for (const auto& name : files) {
            if (CopyFileW(LongPath(from + L"\\" + name).c_str(), LongPath(to + L"\\" + name).c_str(), FALSE)) {
                ++count;
            } else {
                ok = false;
                if (why) {
                    *why = ErrorText(("复制失败：" + Narrow(name)).c_str(), GetLastError());
                }
            }
        }
    }
    // dst 里 src 没有的旁路文件删掉（backup 是选槽那一刻的快照）
    std::vector<std::wstring> dstPlugins;
    ForEachEntry(dst, [&](const std::wstring& name, bool dir) {
        if (dir) {
            dstPlugins.push_back(name);
        }
    });
    for (const auto& plugin : dstPlugins) {
        std::vector<std::wstring> stale;
        ForEachEntry(dst + L"\\" + plugin, [&](const std::wstring& name, bool dir) {
            if (!dir && IsSaveFile(name) && !FileExists(src + L"\\" + plugin + L"\\" + name)) {
                stale.push_back(name);
            }
        });
        for (const auto& name : stale) {
            DeleteFileW(LongPath(dst + L"\\" + plugin + L"\\" + name).c_str());
        }
    }
    if (copied) {
        *copied = count;
    }
    return ok;
}

// ---- 标签表 ----

namespace {

std::string Escape(std::string_view text)
{
    std::string out;
    for (const char ch : text) {
        if (ch == '%' || ch == '\t' || ch == '\r' || ch == '\n') {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(ch));
            out += hex;
        } else {
            out += ch;
        }
    }
    return out;
}

bool Unescape(std::string_view text, std::string* out)
{
    out->clear();
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            *out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return false;
        }
        std::uint64_t value = 0;
        if (!ParseU64(text.substr(i + 1, 2), 16, &value)) {
            return false;
        }
        *out += static_cast<char>(value);
        i += 2;
    }
    return true;
}

}  // namespace

std::string EncodeTags(const std::vector<TagLine>& tags)
{
    std::string out = "DDCORE-TAGS 1\n";
    for (const auto& t : tags) {
        out += std::to_string(t.kind) + "\t" + std::to_string(t.id) + "\t" + std::to_string(t.roster) + "\t" + std::to_string(t.guid) + "\t" +
            Escape(t.buffId) + "\t" + Escape(t.plugin) + "\t" + Escape(t.tag) + "\n";
    }
    return out;
}

bool DecodeTags(std::string_view text, std::vector<TagLine>* out, std::string* why)
{
    out->clear();
    const auto fail = [&](std::string message) {
        if (why) {
            *why = std::move(message);
        }
        out->clear();
        return false;
    };
    std::size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            return fail("最后一行没有换行（数据被截断？）");
        }
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (first) {
            first = false;
            if (line != "DDCORE-TAGS 1") {
                return fail("不是标签表（第一行不对）");
            }
            continue;
        }
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (true) {
            const std::size_t tab = line.find('\t', start);
            parts.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
            if (tab == std::string_view::npos) {
                break;
            }
            start = tab + 1;
        }
        if (parts.size() != 7) {
            return fail("标签行的列数不对：" + std::string(line));
        }
        TagLine t;
        std::uint64_t values[4] = {};
        for (int i = 0; i < 4; ++i) {
            if (!ParseU64(parts[static_cast<std::size_t>(i)], 10, &values[i]) || values[i] > 0xFFFFFFFFull) {
                return fail("标签行的数字不对：" + std::string(line));
            }
        }
        t.kind = static_cast<std::uint32_t>(values[0]);
        t.id = static_cast<std::uint32_t>(values[1]);
        t.roster = static_cast<std::uint32_t>(values[2]);
        t.guid = static_cast<std::uint32_t>(values[3]);
        if (!Unescape(parts[4], &t.buffId) || !Unescape(parts[5], &t.plugin) || !Unescape(parts[6], &t.tag)) {
            return fail("标签行的转义不对：" + std::string(line));
        }
        out->push_back(std::move(t));
    }
    if (first) {
        return fail("空的标签表");
    }
    return true;
}

}  // namespace ddc::persist
