#include "8080.h"
#include <string.h>

extern uint8_t cpu8080_read(CPU8080* cpu, uint16_t addr);
extern void cpu8080_write(CPU8080* cpu, uint16_t addr, uint8_t val);

// --- ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ВЫЧИСЛЕНИЯ ФЛАГОВ ---

static void SetFlagsZSP(CPU8080 *cpu, uint8_t res) {
    uint8_t ty = res;   // C89 (VS2010): объявления только в начале блока
    cpu->Z = (res == 0);
    cpu->S = ((res & 0x80) != 0);
    ty ^= ty >> 4;
    ty ^= ty >> 2;
    ty ^= ty >> 1;
    cpu->P = !(ty & 1);
}

static void SetFlagsINR(CPU8080 *cpu, uint8_t old_val, uint8_t new_val) {
    cpu->AC = ((old_val & 0x0F) + 1 > 0x0F);
    SetFlagsZSP(cpu, new_val);
}

static void SetFlagsDCR(CPU8080* cpu, uint8_t old_val, uint8_t new_val) {
    // DCR = сложение с 0xFF: перенос из бита 3 есть, если младшая тетрада не нулевая.
    cpu->AC = ((old_val & 0x0F) != 0);
    SetFlagsZSP(cpu, new_val);
}

// cin - перенос на входе (0 для ADD/ADI, CY для ADC/ACI)
static void SetFlagsAdd(CPU8080 *cpu, uint8_t r1, uint8_t r2, int cin, int res) {
    cpu->CY = (res > 0xFF);
    cpu->AC = (((r1 & 0xF) + (r2 & 0xF) + cin) > 0xF);
    SetFlagsZSP(cpu, (uint8_t)res);
}

// bin - заём на входе (0 для SUB/CMP/SUI/CPI, CY для SBB/SBI).
// На 8080 вычитание = A + ~src + !bin, AC - перенос из бита 3 этого сложения.
static void SetFlagsSub(CPU8080* cpu,
    uint8_t r1,
    uint8_t r2,
    int bin,
    int res)
{
    // CY — заём из старшего разряда.
    cpu->CY = (res < 0);

    // На 8080 вычитание = A + ~operand + !borrow, а AC - перенос из бита 3 этого СЛОЖЕНИЯ
    // (то есть AC = "нет заёма", а не "заём").
    cpu->AC = (((r1 & 0x0F) + ((~r2) & 0x0F) + (bin ? 0 : 1)) > 0x0F);

    SetFlagsZSP(cpu, (uint8_t)res);
}

// --- ФУНКЦИИ ДОСТУПА К РЕГИСТРОВЫМ ПАРАМ ---
static uint16_t GetHL(const CPU8080 *cpu) { return (cpu->H << 8) | cpu->L; }
static void SetHL(CPU8080 *cpu, uint16_t val) { cpu->H = val >> 8; cpu->L = val & 0xFF; }
static uint16_t GetBC(const CPU8080 *cpu) { return (cpu->B << 8) | cpu->C; }
static void SetBC(CPU8080 *cpu, uint16_t val) { cpu->B = val >> 8; cpu->C = val & 0xFF; }
static uint16_t GetDE(const CPU8080 *cpu) { return (cpu->D << 8) | cpu->E; }
static void SetDE(CPU8080 *cpu, uint16_t val) { cpu->D = val >> 8; cpu->E = val & 0xFF; }

// Вспомогательные функции для симуляции GetReg / SetReg для кодов инструкций Intel 8080
static uint8_t GetReg(CPU8080 *cpu, int reg_idx) {
    switch (reg_idx) {
        case 0: return cpu->B;
        case 1: return cpu->C;
        case 2: return cpu->D;
        case 3: return cpu->E;
        case 4: return cpu->H;
        case 5: return cpu->L;
        case 6: return cpu8080_read(cpu, GetHL(cpu));
        case 7: return cpu->A;
    }
    return 0;
}

static void SetReg(CPU8080 *cpu, int reg_idx, uint8_t val) {
    switch (reg_idx) {
        case 0: cpu->B = val; break;
        case 1: cpu->C = val; break;
        case 2: cpu->D = val; break;
        case 3: cpu->E = val; break;
        case 4: cpu->H = val; break;
        case 5: cpu->L = val; break;
        case 6: cpu8080_write(cpu, GetHL(cpu), val); break;
        case 7: cpu->A = val; break;
    }
}

// --- ИНИЦИАЛИЗАЦИЯ И СБРОС (RESET) ---
void cpu8080_reset(CPU8080* cpu)
{
    if (!cpu) return;

    memset(cpu, 0, sizeof(*cpu));

    cpu->PC = 0xC000; // Старт ПК "Специалист" начинается из ПЗУ Загрузчика
    cpu->SP = 0x0000;

    // Состояние ППА, использовавшееся в старом эмуляторе.
    cpu->ppi0_pa = cpu->ppi0_pb = cpu->ppi0_pc = 0xFF;
    cpu->ppi0_ctrl = 0x9B;

    cpu->ppi1_pa = cpu->ppi1_pb = cpu->ppi1_pc = 0xFF;
    cpu->ppi1_ctrl = 0x9B;

    // Первый кадровый интервал — 50 000 тактов при 2,5 МГц и 50 Гц.
    cpu->cycles_until_interrupt = 50000;
    cpu->int_pending = false;

    cpu->EI = false;
    cpu->halted = false;
}

int cpu8080_step(CPU8080 *cpu) {
    uint8_t op;
    int cycles;
    static const int lut_cycles[256] = {
        4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7,  4,  // 00-0F
        4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7,  4,  // 10-1F
        4, 10, 16, 5,  5,  5,  7,  4,  4, 10, 16, 5,  5,  5,  7,  4,  // 20-2F
        4, 10, 13, 5,  10, 10, 10, 4,  4, 10, 13, 5,  5,  5,  7,  4,  // 30-3F
        5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 40-4F
        5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 50-5F
        5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 60-6F
        7, 7,  7,  7,  7,  7,  7,  7,  5, 5,  5,  5,  5,  5,  7,  5,  // 70-7F
        4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // 80-8F
        4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // 90-9F
        4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // A0-AF
        4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // B0-BF
        5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // C0-CF
        5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // D0-DF
        5, 10, 10, 18, 11, 11, 7,  11, 5, 5,  10, 5,  11, 17, 7,  11, // E0-EF (E3 XTHL=18, E9 PCHL=5, EB XCHG=5)
        5, 10, 10, 10, 11, 11, 7,  11, 5, 5,  10, 10, 11, 17, 7,  11  // F0-FF (F9 SPHL=5)
    };

    // Приём аппаратного прерывания: RST 7 (вектор 0x0038), EI сбрасывается
    if (cpu->int_pending && cpu->EI) {
        cpu->int_pending = false;
        cpu->EI = false;
        cpu->halted = false;
        cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8);
        cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF);
        cpu->PC = 0x0038;
        return 11;
    }

    if (cpu->halted) {
        return 4;
    }

    op = cpu8080_read(cpu, cpu->PC++);
    cycles = lut_cycles[op];

    if ((op & 0xC0) == 0x40 && op != 0x76) {
        SetReg(cpu, (op >> 3) & 7, GetReg(cpu, op & 7));
        return cycles;
    }

    switch (op) {
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28:
    case 0x30: case 0x38: return cycles;

    case 0x76: cpu->halted = true; return cycles;

    case 0x06: cpu->B = cpu8080_read(cpu, cpu->PC++); return cycles;
    case 0x0E: cpu->C = cpu8080_read(cpu, cpu->PC++); return cycles;
    case 0x16: cpu->D = cpu8080_read(cpu, cpu->PC++); return cycles;
    case 0x1E: cpu->E = cpu8080_read(cpu, cpu->PC++); return cycles;
    case 0x26: cpu->H = cpu8080_read(cpu, cpu->PC++); return cycles;
    case 0x2E: cpu->L = cpu8080_read(cpu, cpu->PC++); return cycles;
    case 0x36: cpu8080_write(cpu, GetHL(cpu), cpu8080_read(cpu, cpu->PC++)); return cycles;
    case 0x3E: cpu->A = cpu8080_read(cpu, cpu->PC++); return cycles;

    case 0x01: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); SetBC(cpu, (h << 8) | l); return cycles; }
    case 0x11: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); SetDE(cpu, (h << 8) | l); return cycles; }
    case 0x21: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); SetHL(cpu, (h << 8) | l); return cycles; }
    case 0x31: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); cpu->SP = (h << 8) | l; return cycles; }

    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: { uint8_t src = GetReg(cpu, op & 7); int r = cpu->A + src; SetFlagsAdd(cpu, cpu->A, src, 0, r); cpu->A = (uint8_t)r; return cycles; }

    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D:
    case 0x8E: case 0x8F: { uint8_t src = GetReg(cpu, op & 7); int c = cpu->CY ? 1 : 0; int r = cpu->A + src + c; SetFlagsAdd(cpu, cpu->A, src, c, r); cpu->A = (uint8_t)r; return cycles; }

    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
    case 0x96: case 0x97: { uint8_t src = GetReg(cpu, op & 7); int r = cpu->A - src; SetFlagsSub(cpu, cpu->A, src, 0, r); cpu->A = (uint8_t)r; return cycles; }

    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D:
    case 0x9E: case 0x9F: { uint8_t src = GetReg(cpu, op & 7); int c = cpu->CY ? 1 : 0; int r = cpu->A - src - c; SetFlagsSub(cpu, cpu->A, src, c, r); cpu->A = (uint8_t)r; return cycles; }

    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
    case 0xA6: case 0xA7: { uint8_t src = GetReg(cpu, op & 7); cpu->AC = ((cpu->A | src) & 0x08) != 0; cpu->A &= src; cpu->CY = false; SetFlagsZSP(cpu, cpu->A); return cycles; }

    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
    case 0xAE: case 0xAF: { cpu->A ^= GetReg(cpu, op & 7); cpu->CY = false; cpu->AC = false; SetFlagsZSP(cpu, cpu->A); return cycles; }

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
    case 0xB6: case 0xB7: { cpu->A |= GetReg(cpu, op & 7); cpu->CY = false; cpu->AC = false; SetFlagsZSP(cpu, cpu->A); return cycles; }

    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
    case 0xBE: case 0xBF: { uint8_t src = GetReg(cpu, op & 7); int r = cpu->A - src; SetFlagsSub(cpu, cpu->A, src, 0, r); return cycles; }

    case 0xC6: { uint8_t imm = cpu8080_read(cpu, cpu->PC++); int r = cpu->A + imm; SetFlagsAdd(cpu, cpu->A, imm, 0, r); cpu->A = (uint8_t)r; return cycles; }
    case 0xCE: { uint8_t imm = cpu8080_read(cpu, cpu->PC++); int c = cpu->CY ? 1 : 0; int r = cpu->A + imm + c; SetFlagsAdd(cpu, cpu->A, imm, c, r); cpu->A = (uint8_t)r; return cycles; }
    case 0xD6: { uint8_t imm = cpu8080_read(cpu, cpu->PC++); int r = cpu->A - imm; SetFlagsSub(cpu, cpu->A, imm, 0, r); cpu->A = (uint8_t)r; return cycles; }
    case 0xDE: { uint8_t imm = cpu8080_read(cpu, cpu->PC++); int c = cpu->CY ? 1 : 0; int r = cpu->A - imm - c; SetFlagsSub(cpu, cpu->A, imm, c, r); cpu->A = (uint8_t)r; return cycles; }
    case 0xE6: { uint8_t imm = cpu8080_read(cpu, cpu->PC++); cpu->AC = ((cpu->A | imm) & 0x08) != 0; cpu->A &= imm; cpu->CY = false; SetFlagsZSP(cpu, cpu->A); return cycles; }
    case 0xEE: { cpu->A ^= cpu8080_read(cpu, cpu->PC++); cpu->CY = false; cpu->AC = false; SetFlagsZSP(cpu, cpu->A); return cycles; }
    case 0xF6: { cpu->A |= cpu8080_read(cpu, cpu->PC++); cpu->CY = false; cpu->AC = false; SetFlagsZSP(cpu, cpu->A); return cycles; }
    case 0xFE: { uint8_t imm = cpu8080_read(cpu, cpu->PC++); int r = cpu->A - imm; SetFlagsSub(cpu, cpu->A, imm, 0, r); return cycles; }

    case 0xC3: case 0xCB: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); cpu->PC = (h << 8) | l; return cycles; }
    case 0xCD: case 0xDD: case 0xED: case 0xFD: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; return cycles; }
    case 0xC9: case 0xD9: { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; return cycles; }

    case 0xC0: if (!cpu->Z) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xC8: if (cpu->Z) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xD0: if (!cpu->CY) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xD8: if (cpu->CY) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xE0: if (!cpu->P) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xE8: if (cpu->P) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xF0: if (!cpu->S) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;
    case 0xF8: if (cpu->S) { uint16_t l = cpu8080_read(cpu, cpu->SP++); uint16_t h = cpu8080_read(cpu, cpu->SP++); cpu->PC = (h << 8) | l; cycles = 11; } return cycles;

    case 0xC2: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->Z) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xCA: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->Z) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xD2: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->CY) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xDA: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->CY) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xE2: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->P) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xEA: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->P) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xF2: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->S) { cpu->PC = (h << 8) | l; } return cycles; }
    case 0xFA: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->S) { cpu->PC = (h << 8) | l; } return cycles; }

    case 0xC4: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->Z) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xCC: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->Z) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xD4: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->CY) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xDC: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->CY) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xE4: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->P) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xEC: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->P) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xF4: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (!cpu->S) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }
    case 0xFC: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); if (cpu->S) { cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (h << 8) | l; cycles = 17; } return cycles; }

    case 0x03: SetBC(cpu, GetBC(cpu) + 1); return cycles;
    case 0x13: SetDE(cpu, GetDE(cpu) + 1); return cycles;
    case 0x23: SetHL(cpu, GetHL(cpu) + 1); return cycles;
    case 0x33: cpu->SP++; return cycles;
    case 0x0B: SetBC(cpu, GetBC(cpu) - 1); return cycles;
    case 0x1B: SetDE(cpu, GetDE(cpu) - 1); return cycles;
    case 0x2B: SetHL(cpu, GetHL(cpu) - 1); return cycles;
    case 0x3B: cpu->SP--; return cycles;

    case 0x04: { uint8_t old = cpu->B; cpu->B++; SetFlagsINR(cpu, old, cpu->B); return cycles; }
    case 0x05: { uint8_t old = cpu->B; cpu->B--; SetFlagsDCR(cpu, old, cpu->B); return cycles; }
    case 0x0C: { uint8_t old = cpu->C; cpu->C++; SetFlagsINR(cpu, old, cpu->C); return cycles; }
    case 0x0D: { uint8_t old = cpu->C; cpu->C--; SetFlagsDCR(cpu, old, cpu->C); return cycles; }
    case 0x14: { uint8_t old = cpu->D; cpu->D++; SetFlagsINR(cpu, old, cpu->D); return cycles; }
    case 0x15: { uint8_t old = cpu->D; cpu->D--; SetFlagsDCR(cpu, old, cpu->D); return cycles; }
    case 0x1C: { uint8_t old = cpu->E; cpu->E++; SetFlagsINR(cpu, old, cpu->E); return cycles; }
    case 0x1D: { uint8_t old = cpu->E; cpu->E--; SetFlagsDCR(cpu, old, cpu->E); return cycles; }
    case 0x24: { uint8_t old = cpu->H; cpu->H++; SetFlagsINR(cpu, old, cpu->H); return cycles; }
    case 0x25: { uint8_t old = cpu->H; cpu->H--; SetFlagsDCR(cpu, old, cpu->H); return cycles; }
    case 0x2C: { uint8_t old = cpu->L; cpu->L++; SetFlagsINR(cpu, old, cpu->L); return cycles; }
    case 0x2D: { uint8_t old = cpu->L; cpu->L--; SetFlagsDCR(cpu, old, cpu->L); return cycles; }
    case 0x34: { uint8_t old = cpu8080_read(cpu, GetHL(cpu)); uint8_t v = old + 1; cpu8080_write(cpu, GetHL(cpu), v); SetFlagsINR(cpu, old, v); return cycles; }
    case 0x35: { uint8_t old = cpu8080_read(cpu, GetHL(cpu)); uint8_t v = old - 1; cpu8080_write(cpu, GetHL(cpu), v); SetFlagsDCR(cpu, old, v); return cycles; }
    case 0x3C: { uint8_t old = cpu->A; cpu->A++; SetFlagsINR(cpu, old, cpu->A); return cycles; }
    case 0x3D: { uint8_t old = cpu->A; cpu->A--; SetFlagsDCR(cpu, old, cpu->A); return cycles; }

    case 0x09: { unsigned int r = (unsigned int)GetHL(cpu) + (unsigned int)GetBC(cpu); cpu->CY = (r > 0xFFFF); SetHL(cpu, (uint16_t)(r & 0xFFFF)); return cycles; }
    case 0x19: { unsigned int r = (unsigned int)GetHL(cpu) + (unsigned int)GetDE(cpu); cpu->CY = (r > 0xFFFF); SetHL(cpu, (uint16_t)(r & 0xFFFF)); return cycles; }
    case 0x29: { unsigned int r = (unsigned int)GetHL(cpu) + (unsigned int)GetHL(cpu); cpu->CY = (r > 0xFFFF); SetHL(cpu, (uint16_t)(r & 0xFFFF)); return cycles; }
    case 0x39: { unsigned int r = (unsigned int)GetHL(cpu) + (unsigned int)cpu->SP;    cpu->CY = (r > 0xFFFF); SetHL(cpu, (uint16_t)(r & 0xFFFF)); return cycles; }

    case 0xC5: cpu8080_write(cpu, --cpu->SP, cpu->B); cpu8080_write(cpu, --cpu->SP, cpu->C); return cycles;
    case 0xD5: cpu8080_write(cpu, --cpu->SP, cpu->D); cpu8080_write(cpu, --cpu->SP, cpu->E); return cycles;
    case 0xE5: cpu8080_write(cpu, --cpu->SP, cpu->H); cpu8080_write(cpu, --cpu->SP, cpu->L); return cycles;
    case 0xF5: { uint8_t psw = (cpu->S << 7) | (cpu->Z << 6) | (0 << 5) | (cpu->AC << 4) | (0 << 3) | (cpu->P << 2) | (2) | (cpu->CY ? 1 : 0); cpu8080_write(cpu, --cpu->SP, cpu->A); cpu8080_write(cpu, --cpu->SP, psw); return cycles; }
    case 0xC1: cpu->C = cpu8080_read(cpu, cpu->SP++); cpu->B = cpu8080_read(cpu, cpu->SP++); return cycles;
    case 0xD1: cpu->E = cpu8080_read(cpu, cpu->SP++); cpu->D = cpu8080_read(cpu, cpu->SP++); return cycles;
    case 0xE1: cpu->L = cpu8080_read(cpu, cpu->SP++); cpu->H = cpu8080_read(cpu, cpu->SP++); return cycles;
    case 0xF1: { uint8_t psw = cpu8080_read(cpu, cpu->SP++); cpu->A = cpu8080_read(cpu, cpu->SP++); cpu->S = (psw & 0x80) != 0; cpu->Z = (psw & 0x40) != 0; cpu->AC = (psw & 0x10) != 0; cpu->P = (psw & 0x04) != 0; cpu->CY = (psw & 0x01) != 0; return cycles; }

    case 0x32: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); cpu8080_write(cpu, (h << 8) | l, cpu->A); return cycles; }
    case 0x3A: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); cpu->A = cpu8080_read(cpu, (h << 8) | l); return cycles; }
    case 0x22: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); uint16_t a = (h << 8) | l; cpu8080_write(cpu, a, cpu->L); cpu8080_write(cpu, a + 1, cpu->H); return cycles; }
    case 0x2A: { uint16_t l = cpu8080_read(cpu, cpu->PC++); uint16_t h = cpu8080_read(cpu, cpu->PC++); uint16_t a = (h << 8) | l; cpu->L = cpu8080_read(cpu, a); cpu->H = cpu8080_read(cpu, a + 1); return cycles; }

    case 0x02: cpu8080_write(cpu, GetBC(cpu), cpu->A); return cycles;
    case 0x12: cpu8080_write(cpu, GetDE(cpu), cpu->A); return cycles;
    case 0x0A: cpu->A = cpu8080_read(cpu, GetBC(cpu)); return cycles;
    case 0x1A: cpu->A = cpu8080_read(cpu, GetDE(cpu)); return cycles;

    case 0x07: { cpu->CY = (cpu->A & 0x80) != 0; cpu->A = (uint8_t)(((cpu->A << 1) | (cpu->CY ? 1 : 0)) & 0xFF); return cycles; }
    case 0x0F: { cpu->CY = (cpu->A & 1) != 0; cpu->A = (uint8_t)(((cpu->A >> 1) | (cpu->CY ? 0x80 : 0)) & 0xFF); return cycles; }
    case 0x17: { bool old_cy = cpu->CY; cpu->CY = (cpu->A & 0x80) != 0; cpu->A = (uint8_t)(((cpu->A << 1) | (old_cy ? 1 : 0)) & 0xFF); return cycles; }
    case 0x1F: { bool old_cy = cpu->CY; cpu->CY = (cpu->A & 1) != 0; cpu->A = (uint8_t)(((cpu->A >> 1) | (old_cy ? 0x80 : 0)) & 0xFF); return cycles; }

    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        cpu8080_write(cpu, --cpu->SP, cpu->PC >> 8); cpu8080_write(cpu, --cpu->SP, cpu->PC & 0xFF); cpu->PC = (op & 0x38); return cycles;

    case 0xD3: {
        uint8_t port = cpu8080_read(cpu, cpu->PC++);
        uint16_t target_addr = (port << 8) | port;
        cpu8080_write(cpu, target_addr, cpu->A);
        return cycles;
    }

    case 0xDB: {
        uint8_t port = cpu8080_read(cpu, cpu->PC++);
        uint16_t target_addr = (port << 8) | port;
        cpu->A = cpu8080_read(cpu, target_addr);
        return cycles;
    }

    case 0x27: {
        uint8_t corr = 0; bool new_cy = cpu->CY; int res;
        if ((cpu->A & 0x0F) > 9 || cpu->AC) corr |= 0x06;
        if (cpu->A > 0x99 || cpu->CY) { corr |= 0x60; new_cy = true; }
        res = cpu->A + corr; cpu->AC = ((cpu->A & 0x0F) + (corr & 0x0F)) > 0x0F;
        cpu->A = (uint8_t)res; cpu->CY = new_cy; SetFlagsZSP(cpu, cpu->A); return cycles;
    }

    case 0x2F: cpu->A = (uint8_t)(~cpu->A & 0xFF); return cycles;
    case 0x37: cpu->CY = true; return cycles;
    case 0x3F: cpu->CY = !cpu->CY; return cycles;
    case 0xEB: { uint8_t t = cpu->D; cpu->D = cpu->H; cpu->H = t; t = cpu->E; cpu->E = cpu->L; cpu->L = t; return cycles; }
    case 0xE3: { uint8_t l = cpu8080_read(cpu, cpu->SP); uint8_t h = cpu8080_read(cpu, cpu->SP + 1); cpu8080_write(cpu, cpu->SP, cpu->L); cpu8080_write(cpu, cpu->SP + 1, cpu->H); cpu->L = l; cpu->H = h; return cycles; }
    case 0xF9: cpu->SP = GetHL(cpu); return cycles;
    case 0xE9: cpu->PC = GetHL(cpu); return cycles;
    case 0xFB: cpu->EI = true; return cycles;
    case 0xF3: cpu->EI = false; return cycles;
    }
    return cycles;
}
