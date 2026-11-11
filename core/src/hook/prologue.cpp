#include "prologue.h"

namespace ddc::hook {

namespace {

const char* const kTruncated = "指令被截断";
const char* const kUnsupported = "不支持的指令";

bool ReadModRm(const std::uint8_t* code, std::uint32_t available, std::uint32_t& i, const char** reason)
{
    if (i >= available) {
        *reason = kTruncated;
        return false;
    }
    const std::uint8_t m = code[i++];
    const std::uint8_t mod = m >> 6;
    const std::uint8_t rm = m & 7;
    if (mod == 3) {
        return true;
    }
    if (mod == 0 && rm == 5) {
        *reason = "rip 相对寻址";
        return false;
    }
    if (rm == 4) {
        if (i >= available) {
            *reason = kTruncated;
            return false;
        }
        const std::uint8_t sib = code[i++];
        if (mod == 0 && (sib & 7) == 5) {
            i += 4;  // [index*scale+disp32]，没有基址
        }
    }
    if (mod == 1) {
        i += 1;
    } else if (mod == 2) {
        i += 4;
    }
    return true;
}

}  // namespace

std::uint32_t DecodeInstruction(const std::uint8_t* code, std::uint32_t available, const char** reason)
{
    const char* localReason = nullptr;
    if (!reason) {
        reason = &localReason;
    }

    std::uint32_t i = 0;
    bool operand16 = false;
    while (i < available && i < 4) {
        const std::uint8_t b = code[i];
        if (b == 0x66) {
            operand16 = true;
        } else if (!(b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 ||
                       b == 0x65 || b == 0x67)) {
            break;
        }
        ++i;
    }

    bool rexW = false;
    if (i < available && (code[i] & 0xF0) == 0x40) {
        rexW = (code[i] & 0x08) != 0;
        ++i;
    }
    if (i >= available) {
        *reason = kTruncated;
        return 0;
    }

    const std::uint8_t op = code[i++];
    std::uint32_t immediate = 0;
    bool modrm = false;
    bool twoByte = false;
    std::uint8_t op2 = 0;

    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03:  // add
    case 0x08: case 0x09: case 0x0A: case 0x0B:  // or
    case 0x10: case 0x11: case 0x12: case 0x13:  // adc
    case 0x18: case 0x19: case 0x1A: case 0x1B:  // sbb
    case 0x20: case 0x21: case 0x22: case 0x23:  // and
    case 0x28: case 0x29: case 0x2A: case 0x2B:  // sub
    case 0x30: case 0x31: case 0x32: case 0x33:  // xor
    case 0x38: case 0x39: case 0x3A: case 0x3B:  // cmp
    case 0x63:                                   // movsxd
    case 0x84: case 0x85:                        // test
    case 0x86: case 0x87:                        // xchg
    case 0x88: case 0x89: case 0x8A: case 0x8B:  // mov
    case 0x8D:                                   // lea
        modrm = true;
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:  // push
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:  // pop
    case 0x90:                                                                               // nop
        return i;
    case 0x68:  // push imm32
        immediate = 4;
        break;
    case 0x6A:  // push imm8
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:  // mov r8, imm8
        immediate = 1;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:  // mov r, imm
        immediate = rexW ? 8 : (operand16 ? 2 : 4);
        break;
    case 0x6B:  // imul r, r/m, imm8
    case 0x80: case 0x83:  // grp1 imm8
    case 0xC0: case 0xC1:  // shift imm8
    case 0xC6:             // mov r/m8, imm8
        modrm = true;
        immediate = 1;
        break;
    case 0x69:  // imul r, r/m, imm32
    case 0x81:  // grp1 imm32
    case 0xC7:  // mov r/m, imm32
        modrm = true;
        immediate = operand16 ? 2 : 4;
        break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:  // 移位
        modrm = true;
        break;
    case 0xF6:
    case 0xF7: {
        if (i >= available) {
            *reason = kTruncated;
            return 0;
        }
        const std::uint8_t reg = (code[i] >> 3) & 7;
        modrm = true;
        if (reg <= 1) {  // test r/m, imm
            immediate = (op == 0xF6) ? 1 : (operand16 ? 2 : 4);
        }
        break;
    }
    case 0xFE:
    case 0xFF: {
        if (i >= available) {
            *reason = kTruncated;
            return 0;
        }
        const std::uint8_t reg = (code[i] >> 3) & 7;
        if (op == 0xFF && (reg == 2 || reg == 3 || reg == 4 || reg == 5)) {
            *reason = "间接跳转或调用";
            return 0;
        }
        modrm = true;
        break;
    }
    case 0xC2: case 0xC3:  // ret
    case 0xCC:             // int3
    case 0xE8:             // call
    case 0xE9: case 0xEB:  // jmp
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:  // jcc
        *reason = "控制流指令";
        return 0;
    case 0x0F:
        twoByte = true;
        break;
    default:
        *reason = kUnsupported;
        return 0;
    }

    if (twoByte) {
        if (i >= available) {
            *reason = kTruncated;
            return 0;
        }
        op2 = code[i++];
        if (op2 == 0x1F || op2 == 0x1E ||                                    // nop r/m、endbr
            op2 == 0x10 || op2 == 0x11 || op2 == 0x12 || op2 == 0x13 || op2 == 0x14 || op2 == 0x15 || op2 == 0x16 ||
            op2 == 0x17 ||                                                  // mov*ps/pd
            op2 == 0x28 || op2 == 0x29 || op2 == 0x2A || op2 == 0x2C || op2 == 0x2D || op2 == 0x2E ||
            op2 == 0x2F ||                                                  // movaps、cvt、ucomi
            (op2 >= 0x40 && op2 <= 0x4F) ||                                 // cmov
            (op2 >= 0x51 && op2 <= 0x5F) ||                                 // 算术
            op2 == 0x6E || op2 == 0x6F || op2 == 0x7E || op2 == 0x7F ||     // movd/q
            (op2 >= 0x90 && op2 <= 0x9F) ||                                 // setcc
            op2 == 0xA2 || op2 == 0xAF ||                                   // cpuid、imul
            op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {     // movzx/movsx
            modrm = op2 != 0xA2;
        } else if (op2 == 0x70 || op2 == 0xC2 || op2 == 0xC6) {             // pshufd、cmpps、shufps
            modrm = true;
            immediate = 1;
        } else if (op2 >= 0x80 && op2 <= 0x8F) {
            *reason = "控制流指令";
            return 0;
        } else {
            *reason = kUnsupported;
            return 0;
        }
    }

    if (modrm && !ReadModRm(code, available, i, reason)) {
        return 0;
    }
    i += immediate;
    if (i > available) {
        *reason = kTruncated;
        return 0;
    }
    return i;
}

std::uint32_t ComputeStealLength(const std::uint8_t* code, std::uint32_t available, std::uint32_t minLength,
    const char** reason)
{
    const char* localReason = nullptr;
    if (!reason) {
        reason = &localReason;
    }
    std::uint32_t total = 0;
    while (total < minLength) {
        if (total >= available) {
            *reason = "期望字节没有覆盖到指令边界";
            return 0;
        }
        const std::uint32_t length = DecodeInstruction(code + total, available - total, reason);
        if (length == 0) {
            return 0;
        }
        total += length;
    }
    return total;
}

}  // namespace ddc::hook
