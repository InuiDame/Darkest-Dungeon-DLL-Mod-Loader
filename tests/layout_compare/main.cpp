// 布局清单运行时核对的 C++/Python 对照测试（U1b）：把 exe 按节映射到 RVA（不执行、不做重定位，和 tools/ddupdate/image.py
// 一样），跑 DDCore 的特征码解析和依据求值（core/src/game/layout_check.cpp，同一份源码），结果写成 JSON。
// tools/ddupdate/cpp_compare.py 调用它，和 dd_update.py verify --json 逐项比较。
//
// 用法：dd_layout_compare <exe> [--json <文件>] [--sig-fail <id,id...>] [--repeat <N>]
//   --sig-fail  把这些特征码当作失效（Python 只因 HOOK 偷取范围、DD_ENTRY 之类的静态检查判它失效时，cpp_compare.py
//               用它让两边的依据求值有同样的输入，见 layout_check.h 开头）
//   --repeat    每条依据重复求值 N 次，耗时取平均（开销表用），默认 1
// 另外按「已知版本」和「未知版本」各生成一次停用开关，写进 JSON 的 gate（ADR 0036 的策略在两种情况下停用什么）。

#include "../../core/src/game/layout_check.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace check = ddc::game::check;

namespace {

std::string Escape(const std::string& text)
{
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out;
}

std::string Str(const std::string& text)
{
    return "\"" + Escape(text) + "\"";
}

template <class T>
std::string Opt(bool has, T value)
{
    return has ? std::to_string(value) : "null";
}

// 按节映射：头部和每个节的原始数据放到 VirtualAddress（和 image.py 一致），其余是 0
bool MapImage(const std::wstring& path, std::vector<std::uint8_t>* mem, std::string* error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "打不开 exe";
        return false;
    }
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (raw.size() < sizeof(IMAGE_DOS_HEADER)) {
        *error = "文件太小";
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size()) {
        *error = "不是 PE 文件";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        *error = "不是 64 位 PE";
        return false;
    }
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    mem->assign(size, 0);
    const std::size_t headers = (std::min)(static_cast<std::size_t>(nt->OptionalHeader.SizeOfHeaders), (std::min)(raw.size(), size));
    std::memcpy(mem->data(), raw.data(), headers);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const std::size_t offset = section->PointerToRawData;
        if (offset >= raw.size() || section->VirtualAddress >= size) {
            continue;
        }
        std::size_t count = (std::min)(static_cast<std::size_t>(section->SizeOfRawData), raw.size() - offset);
        count = (std::min)(count, size - section->VirtualAddress);
        std::memcpy(mem->data() + section->VirtualAddress, raw.data() + offset, count);
    }
    return true;
}

std::string GateJson(const check::Gate& gate, const std::vector<check::PolicyLine>& lines)
{
    std::ostringstream o;
    o << "{\"off\": {";
    bool first = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(check::Dep::Count); ++i) {
        const auto dep = static_cast<check::Dep>(i);
        if (!gate.Off(dep)) {
            continue;
        }
        o << (first ? "" : ", ") << Str(check::InfoOf(dep).word) << ": " << Str(gate.Reason(dep));
        first = false;
    }
    o << "}, \"topics\": [";
    first = true;
    for (const auto& rule : gate.TopicRules()) {
        o << (first ? "" : ", ") << "{\"pattern\": " << Str(rule.pattern) << ", \"write_only\": " << (rule.writeOnly ? "true" : "false")
          << ", \"reason\": " << Str(rule.reason) << "}";
        first = false;
    }
    o << "], \"log\": [";
    first = true;
    for (const auto& line : lines) {
        o << (first ? "" : ", ") << "{\"level\": " << line.level << ", \"text\": " << Str(line.text) << "}";
        first = false;
    }
    o << "]}";
    return o.str();
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::fprintf(stderr, "用法：dd_layout_compare <exe> [--json <文件>] [--sig-fail <id,id...>] [--repeat <N>]\n");
        return 3;
    }
    std::wstring exe = argv[1];
    std::wstring jsonPath;
    std::vector<std::string> sigFail;
    int repeat = 1;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--json" && i + 1 < argc) {
            jsonPath = argv[++i];
        } else if (arg == L"--repeat" && i + 1 < argc) {
            repeat = (std::max)(1, _wtoi(argv[++i]));
        } else if (arg == L"--sig-fail" && i + 1 < argc) {
            const std::wstring list = argv[++i];
            std::string item;
            for (const wchar_t ch : list + L",") {
                if (ch == L',') {
                    if (!item.empty()) {
                        sigFail.push_back(item);
                    }
                    item.clear();
                } else {
                    item.push_back(static_cast<char>(ch));
                }
            }
        } else {
            std::fprintf(stderr, "不认识的参数\n");
            return 3;
        }
    }

    std::vector<std::uint8_t> mem;
    std::string error;
    if (!MapImage(exe, &mem, &error)) {
        std::fprintf(stderr, "映射失败：%s\n", error.c_str());
        return 3;
    }
    check::ImageView view;
    // 没有做重定位：虚表里的指针按 OptionalHeader.ImageBase 算
    if (!check::DescribeImage(mem.data(), 0, &view, &error)) {
        std::fprintf(stderr, "PE 头解析失败：%s\n", error.c_str());
        return 3;
    }
    view.pointerBase = view.preferredBase;

    LARGE_INTEGER frequency{}, t0{}, t1{}, t2{};
    QueryPerformanceFrequency(&frequency);
    const auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    };
    QueryPerformanceCounter(&t0);
    const check::Scanner scanner(view);
    // 函数表建一次，和 DDCore 的 game::Resolve 一样给 HOOK 的入口检查和依据求值共用
    LARGE_INTEGER tableBegin{}, tableEnd{};
    QueryPerformanceCounter(&tableBegin);
    const check::FunctionTable table(view);
    QueryPerformanceCounter(&tableEnd);
    std::vector<check::SigResult> sigs = check::ResolveSignatures(view, scanner, true, &table);
    QueryPerformanceCounter(&t1);
    for (const auto& id : sigFail) {
        const int index = check::FindSig(id);
        if (index < 0) {
            std::fprintf(stderr, "--sig-fail：没有特征码 %s\n", id.c_str());
            return 3;
        }
        sigs[static_cast<std::size_t>(index)].ok = false;
        sigs[static_cast<std::size_t>(index)].problem = "Python 的静态检查不通过（--sig-fail）";
    }
    check::CheckReport report = check::RunCheck(view, scanner, sigs, &table);
    report.tableMicros = ms(tableBegin, tableEnd) * 1000.0;
    QueryPerformanceCounter(&t2);
    // 每条依据的耗时：先完整求值一遍（把映像读进缓存，不计时），再重复求值取平均；函数表的耗时单列
    const auto& evidence = check::Evidence();
    std::vector<double> micros(evidence.size(), 0.0);
    {
        check::Evaluator evaluator(view, scanner, sigs, &table);
        for (const auto& ev : evidence) {
            evaluator.Evaluate(ev);
        }
        for (int round = 0; round < repeat; ++round) {
            for (std::size_t i = 0; i < evidence.size(); ++i) {
                micros[i] += evaluator.Evaluate(evidence[i]).micros;
            }
        }
        for (auto& m : micros) {
            m /= repeat;
        }
    }

    std::vector<check::PolicyLine> knownLog, unknownLog;
    const check::Gate known = check::BuildGate(report, sigs, true, &knownLog);
    const check::Gate unknown = check::BuildGate(report, sigs, false, &unknownLog);

    std::ostringstream o;
    o << "{\n \"tool\": \"dd_layout_compare\",\n \"image_size\": " << view.size << ",\n \"text\": [" << view.textBegin << ", "
      << view.textEnd << "],\n";
    o << " \"timing_ms\": {\"resolve\": " << ms(t0, t1) << ", \"check\": " << ms(t1, t2) << ", \"check_internal\": " << report.micros / 1000.0
      << ", \"function_table\": " << report.tableMicros / 1000.0 << ", \"repeat\": " << repeat << "},\n";
    o << " \"sigs\": {";
    const auto& defs = check::Signatures();
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const auto& r = sigs[i];
        o << (i ? ",\n  " : "\n  ") << Str(defs[i].id) << ": {\"scanned\": " << (r.scanned ? "true" : "false")
          << ", \"status\": " << Str(r.ok ? "ok" : "fail") << ", \"hits\": " << r.hits << ", \"hit\": " << Opt(r.hasHit, r.hit)
          << ", \"resolved\": " << Opt(r.hasResolved, r.resolved) << ", \"candidates\": [";
        for (std::size_t k = 0; k < r.candidates.size(); ++k) {
            o << (k ? ", " : "") << r.candidates[k];
        }
        o << "], \"problem\": " << Str(r.problem) << "}";
    }
    o << "\n },\n \"evidence\": [";
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        const auto& ev = evidence[i];
        const auto& r = report.evidence[i];
        o << (i ? ",\n  " : "\n  ") << "{\"item\": " << Str(ev.item) << ", \"index\": " << ev.index << ", \"describe\": " << Str(check::Describe(ev))
          << ", \"status\": " << Str(check::StatusText(r.status)) << ", \"origin\": " << Opt(r.hasOrigin, r.origin) << ", \"hits\": " << r.hits
          << ", \"hit\": " << Opt(r.hasHit, r.hit) << ", \"captured\": " << Opt(r.hasCaptured, r.captured)
          << ", \"expected\": " << Opt(r.hasExpected, r.expected) << ", \"implied\": " << Opt(r.hasImplied, r.implied)
          << ", \"micros\": " << micros[i] << ", \"detail\": " << Str(r.detail) << "}";
    }
    o << "\n ],\n \"items\": {";
    const auto& items = check::Items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        o << (i ? ", " : "") << Str(items[i].id) << ": " << Str(check::StateText(report.items[i]));
    }
    o << "},\n \"misbound\": [";
    for (std::size_t i = 0; i < report.misbound.size(); ++i) {
        o << (i ? ", " : "") << Str(report.misbound[i]);
    }
    o << "],\n \"gate\": {\"known\": " << GateJson(known, knownLog) << ",\n  \"unknown\": " << GateJson(unknown, unknownLog) << "}\n}\n";

    if (jsonPath.empty()) {
        std::fwrite(o.str().data(), 1, o.str().size(), stdout);
    } else {
        std::ofstream out(jsonPath, std::ios::binary);
        out << o.str();
        if (!out) {
            std::fprintf(stderr, "写不了 JSON\n");
            return 3;
        }
    }
    std::size_t ok = 0, fail = 0, blocked = 0;
    for (const auto& r : report.evidence) {
        (r.status == check::EvStatus::Ok ? ok : (r.status == check::EvStatus::Fail ? fail : blocked)) += 1;
    }
    std::fprintf(stderr, "特征码 %zu 条（扫描 %.1f ms），依据 %zu 条：成立 %zu、不成立 %zu、没法求值 %zu（求值 %.1f ms，函数表 %.1f ms）\n",
        defs.size(), ms(t0, t1), evidence.size(), ok, fail, blocked, report.micros / 1000.0, report.tableMicros / 1000.0);
    return 0;
}
