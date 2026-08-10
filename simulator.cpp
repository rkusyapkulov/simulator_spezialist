#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <commdlg.h>

#pragma comment(lib, "winmm.lib")

// --- ПАРАМЕТРЫ ЭКРАНА ПК "СПЕЦИАЛИСТ" ---
const int SPEC_SCREEN_WIDTH = 384;
const int SPEC_SCREEN_HEIGHT = 256;

#define IDM_FILE_OPEN 0x0010
#define IDM_FILE_EXIT 0x0020

// --- ГЛОБАЛЬНЫЕ БУФЕРЫ ПАМЯТИ ---
// Линейное адресное пространство компьютера "Специалист" (64 Кб)
BYTE spec_ram[65536];

// --- ГЛОБАЛЬНЫЕ БУФЕРЫ И МОДИФИКАТОРЫ КЛАВИАТУРЫ ---
// Физическая матрица: 6 строк (0..5), в каждой строке по 12 бит столбцов
// По умолчанию контакты разомкнуты (высокий уровень — 1), при нажатии биты сбрасываются в 0
WORD spec_key_matrix[6] = { 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF };

bool nr_button_pressed = false; // Клавиша НР (0 - нажата, 1 - отжата)

bool emulator_running = true;
static DWORD pixel_buffer[384 * 256];

// Переменные состояния ППА К580ВВ55 №2 (Клавиатура, адрес 0xF800)
BYTE ppi2_pa = 0xFF;   // Порт A (Младшие 8 бит из 12 столбцов)
BYTE ppi2_pb = 0xFF;   // Порт B (Биты 2-7: 6 строк; Бит 1: НР; Бит 0: Магнитофон)
BYTE ppi2_pc = 0xFF;   // Порт C (Биты 0-3: Старшие 4 бита из 12 столбцов; Бит 5: Звук)
BYTE ppi2_ctrl = 0x9B; // Управляющее слово

// Переменные состояния ППА К580ВВ55 №1 (Пользовательский, адрес 0xF000)
BYTE ppi1_pa = 0xFF;
BYTE ppi1_pb = 0xFF;
BYTE ppi1_pc = 0xFF;
BYTE ppi1_ctrl = 0x9B;

// --- ЭМУЛЯЦИЯ БИПЕРА INTE ---
bool spec_speaker_state = false; // Текущее состояние динамика от INTE

HWND hRegListBox = NULL;

// --- ПАРАМЕТРЫ ЗВУКОВОГО ПОТОКА ---
const int AUDIO_SAMPLE_RATE = 22050;
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHeader[2];
short* audioBuffers[2] = { NULL, NULL };
const int AUDIO_BUF_SIZE = 1024;

const int RING_BUF_SIZE = 16384;
short audio_ring_buffer[RING_BUF_SIZE] = { 0 };
volatile int ring_write_ptr = 0;
volatile int ring_read_ptr = 0;

#pragma pack(push, 1)
struct CPU8080 {
    BYTE A, B, C, D, E, H, L;
    WORD PC, SP;
    bool S, Z, AC, P, CY;
    bool EI;
    bool halted;
    int cycles_until_interrupt = 40000; // 2.0 МГц / 50 Гц = 40000 тактов на кадр
    bool int_pending = false;

    void Reset() {
        PC = 0xC000; // Старт ПК "Специалист" начинается из ПЗУ Загрузчика
        // Альтернативно Монитор стартует с 0xC800, но стандартный сброс идет на Загрузчик (0xC000)
        SP = 0x0000;
        A = B = C = D = E = H = L = 0;
        S = Z = AC = P = CY = false;
        EI = false;
        halted = false;

        ppi1_pa = ppi1_pb = ppi1_pc = 0xFF; ppi1_ctrl = 0x9B;
        ppi2_pa = ppi2_pb = ppi2_pc = 0xFF; ppi2_ctrl = 0x9B;
    }

    void SetFlagsZSP(BYTE res) {
        Z = (res == 0);
        S = ((res & 0x80) != 0);
        BYTE Ty = res;
        Ty ^= Ty >> 4; Ty ^= Ty >> 2; Ty ^= Ty >> 1;
        P = !(Ty & 1);
    }

    void SetFlagsINR(BYTE old_val, BYTE new_val) {
        AC = ((old_val & 0x0F) + 1 > 0x0F);
        SetFlagsZSP(new_val);
    }

    void SetFlagsDCR(BYTE old_val, BYTE new_val) {
        AC = ((old_val & 0x0F) == 0);
        SetFlagsZSP(new_val);
    }

    void SetFlagsAdd(BYTE r1, BYTE r2, int res) {
        CY = (res > 0xFF);
        AC = (((r1 & 0xF) + (r2 & 0xF)) > 0xF);
        SetFlagsZSP((BYTE)res);
    }

    void SetFlagsSub(BYTE r1, BYTE r2, int res) {
        CY = (res < 0);
        AC = (((r1 & 0xF) - (r2 & 0xF)) < 0);
        SetFlagsZSP((BYTE)res);
    }

    WORD GetHL() const { return (H << 8) | L; }
    void SetHL(WORD val) { H = val >> 8; L = val & 0xFF; }
    WORD GetBC() const { return (B << 8) | C; }
    void SetBC(WORD val) { B = val >> 8; C = val & 0xFF; }
    WORD GetDE() const { return (D << 8) | E; }
    void SetDE(WORD val) { D = val >> 8; E = val & 0xFF; }

    BYTE GetReg(int r) {
        switch (r) {
        case 0: return B; case 1: return C; case 2: return D; case 3: return E;
        case 4: return H; case 5: return L; case 6: return Read(GetHL()); default: return A;
        }
    }

    void SetReg(int r, BYTE val) {
        switch (r) {
        case 0: B = val; break; case 1: C = val; break; case 2: D = val; break; case 3: E = val; break;
        case 4: H = val; break; case 5: L = val; break; case 6: Write(GetHL(), val); break; default: A = val; break;
        }
    }

    // --- АППАРАТНЫЙ ДИСПЕТЧЕР ЧТЕНИЯ КЛАВИАТУРЫ И ПАМЯТИ "СПЕЦИАЛИСТА" ---
    BYTE Read(WORD addr) {
        // Дешифрация системного ППА №2 (Клавиатура/Звук: 0xF800-0xFFFF или зеркало 0xFF00-0xFF03)
        if ((addr >= 0xF800 && addr <= 0xFFFF) || (addr & 0xFFFC) == 0xFF00) {
            switch (addr & 3) {
            case 0: // --- ПОРТ А (Младшие 8 бит столбцов) ---
                if (ppi2_ctrl & 0x10) { // Если настроен на ввод
                    BYTE res = 0xFF;
                    for (int r = 0; r < 6; r++) {
                        // Опрашиваем по маске процессора ИЛИ принудительно инжектируем, если кнопка нажата
                        if (!(ppi2_pb & (1 << (r + 2))) || spec_key_matrix[r] != 0xFFF) {
                            res &= (spec_key_matrix[r] & 0xFF);
                        }
                    }
                    return res;
                }
                return ppi2_pa;

            case 1: // --- ПОРТ В (Строки матрицы биты 2-7 + Кн. НР бит 1) ---
                if (ppi2_ctrl & 0x02) { // Если настроен на ввод
                    BYTE res = 0xFF;
                    WORD cols = ppi2_pa | ((ppi2_pc & 0x0F) << 8);

                    if (cols == 0xFFF) { // ХАК: Если процессор завис и выдает 0xFFF, спасаем ввод
                        for (int r = 0; r < 6; r++) {
                            if (spec_key_matrix[r] != 0xFFF) res &= ~(1 << (r + 2));
                        }
                    }
                    else { // Стандартный опрос
                        for (int r = 0; r < 6; r++) {
                            for (int c = 0; c < 12; c++) {
                                if (!(cols & (1 << c)) && !(spec_key_matrix[r] & (1 << c))) {
                                    res &= ~(1 << (r + 2));
                                    break;
                                }
                            }
                        }
                    }
                    return nr_button_pressed ? (res & ~0x02) : (res | 0x02);
                }
                return nr_button_pressed ? (ppi2_pb & ~0x02) : (ppi2_pb | 0x02);

            case 2: // --- ПОРТ С (Старшие 4 бита столбцов) ---
                if (ppi2_ctrl & 0x01) { // Если младшая половина PC на ввод
                    BYTE res = 0x0F;
                    for (int r = 0; r < 6; r++) {
                        if (!(ppi2_pb & (1 << (r + 2))) || spec_key_matrix[r] != 0xFFF) {
                            res &= ((spec_key_matrix[r] >> 8) & 0x0F);
                        }
                    }
                    return (ppi2_pc & 0xF0) | res;
                }
                return ppi2_pc;

            case 3: // Управляющее слово
                return ppi2_ctrl;
            }
        }

        // Пользовательский ППА №1 (0xF000 - 0xF7FF)
        if (addr >= 0xF000 && addr <= 0xF7FF) {
            switch (addr & 3) {
            case 0: return ppi1_pa;
            case 1: return ppi1_pb;
            case 2: return ppi1_pc;
            case 3: return ppi1_ctrl;
            }
        }

        return spec_ram[addr]; // Обычное линейное ОЗУ
    }

    // --- ДИСПЕТЧЕР ЗАПИСИ "СПЕЦИАЛИСТА" ---
    void Write(WORD addr, BYTE val) {
        if (addr >= 0xC000 && addr <= 0xCEFF) return; // ПЗУ защита

        // Обработка системного ППА №2 (Включая зеркало 0xFF00-0xFF03)
        if ((addr >= 0xF800 && addr <= 0xFFFF) || (addr & 0xFFFC) == 0xFF00) {
            WORD reg = addr & 3;
            if (reg == 0) { ppi2_pa = val; }
            else if (reg == 1) { ppi2_pb = val; }
            else if (reg == 2) {
                ppi2_pc = val;
                spec_speaker_state = (val & 0x20) != 0; // Звук бипера
            }
            else { ppi2_ctrl = val; 
                // --- ДОБАВЛЕНА ПОДДЕРЖКА РЕЖИМА BSR И ЗВУКА ЗАГРУЗЧИКА (STA 0xFF03) ---
                // Если старший бит (Бит 7) равен 0, это команда BSR (побитовое изменение Порта C)
                if ((val & 0x80) == 0) {
                    int bit_select = (val >> 1) & 7; // Выделяем номер бита (биты 1-3)
                    int bit_value = val & 1;         // Выделяем значение бита (бит 0)

                    // Если загрузчик или игра меняет Бит 5 Порта C
                    if (bit_select == 5) {
                        spec_speaker_state = (bit_value == 1);
                    }

                    // Синхронизируем изменение в самом виртуальном порту ppi2_pc
                    if (bit_value) ppi2_pc |= (1 << bit_select);
                    else           ppi2_pc &= ~(1 << bit_select);
                }
            }
            return;
        }

        // Пользовательский ППА №1
        if (addr >= 0xF000 && addr <= 0xF7FF) {
            WORD reg = addr & 3;
            if (reg == 0) ppi1_pa = val;
            else if (reg == 1) ppi1_pb = val;
            else if (reg == 2) ppi1_pc = val;
            else ppi1_ctrl = val;
            return;
        }

        spec_ram[addr] = val;
    }


    int Step() {
        static const int lut_cycles[256] = {
            4, 10, 7, 5, 5, 5, 7, 4, 4, 10, 7, 5, 5, 5, 7, 4,
            4, 10, 7, 5, 5, 5, 7, 4, 4, 10, 7, 5, 5, 5, 7, 4,
            4, 10, 16, 5, 5, 5, 7, 4, 4, 10, 16, 5, 5, 5, 7, 4,
            4, 10, 13, 5, 10, 10, 10, 4, 4, 10, 13, 5, 5, 5, 7, 4,
            5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
            5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
            5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 7, 5,
            7, 7, 7, 7, 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 7, 5,
            4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
            4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
            4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
            4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
            5, 10, 10, 10, 11, 11, 7, 11, 5, 10, 10, 10, 11, 17, 7, 11,
            5, 10, 10, 10, 11, 11, 7, 11, 5, 10, 10, 10, 11, 17, 7, 11,
            5, 10, 10, 10, 11, 11, 7, 11, 5, 10, 10, 10, 11, 17, 7, 11,
            5, 10, 10, 10, 11, 11, 7, 11, 5, 10, 10, 10, 11, 17, 7, 11
        };

        if (int_pending && EI) {
            int_pending = false;
            halted = false;
        }
        // 2. Если процессор в состоянии HLT
        if (halted) {
            return 4; // Будем возвращать по 4 такта
        }
        BYTE op = Read(PC++);
        int cycles = lut_cycles[op];
        // Группа команд MOV r1, r2 (исключая MOV M, M, которая является HLT 0x76)
        if ((op & 0xC0) == 0x40 && op != 0x76) {
            SetReg((op >> 3) & 7, GetReg(op & 7));
            return cycles;
        }
        switch (op) {
            // NOP инструкции
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28:
        case 0x30: case 0x38: return cycles;
            // Останавливаем процессор до аппаратного прерывания
        case 0x76: halted = true; return cycles;
            // MVI r, imm
        case 0x06: B = Read(PC++); return cycles;
        case 0x0E: C = Read(PC++); return cycles;
        case 0x16: D = Read(PC++); return cycles;
        case 0x1E: E = Read(PC++); return cycles;
        case 0x26: H = Read(PC++); return cycles;
        case 0x2E: L = Read(PC++); return cycles;
        case 0x36: Write(GetHL(), Read(PC++)); return cycles;
        case 0x3E: A = Read(PC++); return cycles;
            // LXI rp, imm
        case 0x01: { WORD l = Read(PC++); WORD h = Read(PC++); SetBC((h << 8) | l); return cycles; }
        case 0x11: { WORD l = Read(PC++); WORD h = Read(PC++); SetDE((h << 8) | l); return cycles; }
        case 0x21: { WORD l = Read(PC++); WORD h = Read(PC++); SetHL((h << 8) | l); return cycles; }
        case 0x31: { WORD l = Read(PC++); WORD h = Read(PC++); SP = (h << 8) | l; return cycles; }
                 // ADD r
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
        case 0x86: case 0x87: { BYTE src = GetReg(op & 7); int r = A + src; SetFlagsAdd(A, src, r); A = (BYTE)r; return cycles; }
                 // ADC r
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D:
        case 0x8E: case 0x8F: { BYTE src = GetReg(op & 7); int r = A + src + (CY ? 1 : 0); SetFlagsAdd(A, src, r); A = (BYTE)r; return cycles; }
                 // SUB r
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
        case 0x96: case 0x97: { BYTE src = GetReg(op & 7); int r = A - src; SetFlagsSub(A, src, r); A = (BYTE)r; return cycles; }
                 // SBB r
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D:
        case 0x9E: case 0x9F: { BYTE src = GetReg(op & 7); int r = A - src - (CY ? 1 : 0); SetFlagsSub(A, src, r); A = (BYTE)r; return cycles; }
                 // ANA r
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
        case 0xA6: case 0xA7: { A &= GetReg(op & 7); CY = false; AC = true; SetFlagsZSP(A); return cycles; }
                 // XRA r
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF: { A ^= GetReg(op & 7); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
                 // ORA r
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
        case 0xB6: case 0xB7: { A |= GetReg(op & 7); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
                 // CMP r
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
        case 0xBE: case 0xBF: { BYTE src = GetReg(op & 7); int r = A - src; SetFlagsSub(A, src, r); return cycles; }
                 // Операции с непосредственным значением (Иммедиат)
        case 0xC6: { BYTE imm = Read(PC++); int r = A + imm; SetFlagsAdd(A, imm, r); A = (BYTE)r; return cycles; }
        case 0xCE: { BYTE imm = Read(PC++); int r = A + imm + (CY ? 1 : 0); SetFlagsAdd(A, imm, r); A = (BYTE)r; return cycles; }
        case 0xD6: { BYTE imm = Read(PC++); int r = A - imm; SetFlagsSub(A, imm, r); A = (BYTE)r; return cycles; }
        case 0xDE: { BYTE imm = Read(PC++); int r = A - imm - (CY ? 1 : 0); SetFlagsSub(A, imm, r); A = (BYTE)r; return cycles; }
        case 0xE6: { A &= Read(PC++); CY = false; AC = true; SetFlagsZSP(A); return cycles; }
        case 0xEE: { A ^= Read(PC++); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
        case 0xF6: { A |= Read(PC++); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
        case 0xFE: { BYTE imm = Read(PC++); int r = A - imm; SetFlagsSub(A, imm, r); return cycles; }
                 // Безусловные JMP / CALL / RET
        case 0xC3: { WORD l = Read(PC++); WORD h = Read(PC++); PC = (h << 8) | l; return cycles; }
        case 0xCD: { WORD l = Read(PC++); WORD h = Read(PC++); Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; return cycles; }
        case 0xC9: { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; return cycles; }
                 // Возвраты по условию (При переходе тратится 11 тактов вместо 5 базовых)
        case 0xC0: if (!Z) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xC8: if (Z) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xD0: if (!CY) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xD8: if (CY) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xE0: if (!P) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xE8: if (P) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xF0: if (!S) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
        case 0xF8: if (S) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
            // Переходы по условию (Всегда занимают 10 тактов в 8080)
        case 0xC2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!Z) { PC = (h << 8) | l; } return cycles; }
        case 0xCA: { WORD l = Read(PC++); WORD h = Read(PC++); if (Z) { PC = (h << 8) | l; } return cycles; }
        case 0xD2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!CY) { PC = (h << 8) | l; } return cycles; }
        case 0xDA: { WORD l = Read(PC++); WORD h = Read(PC++); if (CY) { PC = (h << 8) | l; } return cycles; }
        case 0xE2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!P) { PC = (h << 8) | l; } return cycles; }
        case 0xEA: { WORD l = Read(PC++); WORD h = Read(PC++); if (P) { PC = (h << 8) | l; } return cycles; }
        case 0xF2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!S) { PC = (h << 8) | l; } return cycles; }
        case 0xFA: { WORD l = Read(PC++); WORD h = Read(PC++); if (S) { PC = (h << 8) | l; } return cycles; }
                 // Вызовы по условию (При вызове тратится 17 тактов вместо 11 базовых)
        case 0xC4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!Z) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xCC: { WORD l = Read(PC++); WORD h = Read(PC++); if (Z) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xD4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!CY) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xDC: { WORD l = Read(PC++); WORD h = Read(PC++); if (CY) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xE4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!P) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xEC: { WORD l = Read(PC++); WORD h = Read(PC++); if (P) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xF4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!S) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
        case 0xFC: { WORD l = Read(PC++); WORD h = Read(PC++); if (S) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
                 // INX / DCX
        case 0x03: SetBC(GetBC() + 1); return cycles;
        case 0x13: SetDE(GetDE() + 1); return cycles;
        case 0x23: SetHL(GetHL() + 1); return cycles;
        case 0x33: SP++; return cycles;
        case 0x0B: SetBC(GetBC() - 1); return cycles;
        case 0x1B: SetDE(GetDE() - 1); return cycles;
        case 0x2B: SetHL(GetHL() - 1); return cycles;
        case 0x3B: SP--; return cycles;
            // INR / DCR
        case 0x04: { BYTE old = B; B++; SetFlagsINR(old, B); return cycles; }
        case 0x05: { BYTE old = B; B--; SetFlagsDCR(old, B); return cycles; }
        case 0x0C: { BYTE old = C; C++; SetFlagsINR(old, C); return cycles; }
        case 0x0D: { BYTE old = C; C--; SetFlagsDCR(old, C); return cycles; }
        case 0x14: { BYTE old = D; D++; SetFlagsINR(old, D); return cycles; }
        case 0x15: { BYTE old = D; D--; SetFlagsDCR(old, D); return cycles; }
        case 0x1C: { BYTE old = E; E++; SetFlagsINR(old, E); return cycles; }
        case 0x1D: { BYTE old = E; E--; SetFlagsDCR(old, E); return cycles; }
        case 0x24: { BYTE old = H; H++; SetFlagsINR(old, H); return cycles; }
        case 0x25: { BYTE old = H; H--; SetFlagsDCR(old, H); return cycles; }
        case 0x2C: { BYTE old = L; L++; SetFlagsINR(old, L); return cycles; }
        case 0x2D: { BYTE old = L; L--; SetFlagsDCR(old, L); return cycles; }
        case 0x34: { BYTE old = Read(GetHL()); BYTE v = old + 1; Write(GetHL(), v); SetFlagsINR(old, v); return cycles; }
        case 0x35: { BYTE old = Read(GetHL()); BYTE v = old - 1; Write(GetHL(), v); SetFlagsDCR(old, v); return cycles; }
        case 0x3C: { BYTE old = A; A++; SetFlagsINR(old, A); return cycles; }
        case 0x3D: { BYTE old = A; A--; SetFlagsDCR(old, A); return cycles; }
                 // DAD rp
        case 0x09: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetBC(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
        case 0x19: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetDE(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
        case 0x29: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetHL(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
        case 0x39: { unsigned int r = (unsigned int)GetHL() + (unsigned int)SP; CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
                 // PUSH / POP
        case 0xC5: Write(--SP, B); Write(--SP, C); return cycles;
        case 0xD5: Write(--SP, D); Write(--SP, E); return cycles;
        case 0xE5: Write(--SP, H); Write(--SP, L); return cycles;
        case 0xF5: { BYTE psw = (S << 7) | (Z << 6) | (0 << 5) | (AC << 4) | (0 << 3) | (P << 2) | (2) | (CY ? 1 : 0); Write(--SP, A); Write(--SP, psw); return cycles; }
        case 0xC1: C = Read(SP++); B = Read(SP++); return cycles;
        case 0xD1: E = Read(SP++); D = Read(SP++); return cycles;
        case 0xE1: L = Read(SP++); H = Read(SP++); return cycles;
        case 0xF1: { BYTE psw = Read(SP++); A = Read(SP++); S = (psw & 0x80) != 0; Z = (psw & 0x40) != 0; AC = (psw & 0x10) != 0; P = (psw & 0x04) != 0; CY = (psw & 0x01) != 0; return cycles; }
                 // Прямая адресация (STA, LDA, SHLD, LHLD)
        case 0x32: { WORD l = Read(PC++); WORD h = Read(PC++); Write((h << 8) | l, A); return cycles; }
        case 0x3A: { WORD l = Read(PC++); WORD h = Read(PC++); A = Read((h << 8) | l); return cycles; }
        case 0x22: { WORD l = Read(PC++); WORD h = Read(PC++); WORD a = (h << 8) | l; Write(a, L); Write(a + 1, H); return cycles; }
        case 0x2A: { WORD l = Read(PC++); WORD h = Read(PC++); WORD a = (h << 8) | l; L = Read(a); H = Read(a + 1); return cycles; }
                 // Косвенная адресация
        case 0x02: Write(GetBC(), A); return cycles;
        case 0x12: Write(GetDE(), A); return cycles;
        case 0x0A: A = Read(GetBC()); return cycles;
        case 0x1A: A = Read(GetDE()); return cycles;
            // Циклические сдвиги аккумулятора
        case 0x07: { CY = (A & 0x80) != 0; A = (BYTE)(((A << 1) | (CY ? 1 : 0)) & 0xFF); return cycles; }
        case 0x0F: { CY = (A & 1) != 0; A = (BYTE)(((A >> 1) | (CY ? 0x80 : 0)) & 0xFF); return cycles; }
        case 0x17: { bool old_cy = CY; CY = (A & 0x80) != 0; A = (BYTE)(((A << 1) | (old_cy ? 1 : 0)) & 0xFF); return cycles; }
        case 0x1F: { bool old_cy = CY; CY = (A & 1) != 0; A = (BYTE)(((A >> 1) | (old_cy ? 0x80 : 0)) & 0xFF); return cycles; }
                 // RST 0 - RST 7
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (op & 0x38); return cycles;
            // OUT port
        case 0xD3: {
            BYTE port = Read(PC++);
            WORD target_addr = (port << 8) | port;
            Write(target_addr, A);
            return cycles;
        }
                 // IN port
        case 0xDB: {
            BYTE port = Read(PC++);
            WORD target_addr = (port << 8) | port;
            A = Read(target_addr);
            return cycles;
        }
                 // Десятичная коррекция аккумулятора (DAA)
        case 0x27: {
            BYTE corr = 0; bool new_cy = CY;
            if ((A & 0x0F) > 9 || AC) corr |= 0x06;
            if (A > 0x99 || CY) { corr |= 0x60; new_cy = true; }
            int res = A + corr; AC = ((A & 0x0F) + (corr & 0x0F)) > 0x0F;
            A = (BYTE)res; CY = new_cy; SetFlagsZSP(A); return cycles;
        }
                 // Специальные команды
        case 0x2F: A = (BYTE)(~A & 0xFF); return cycles;
        case 0x37: CY = true; return cycles;
        case 0x3F: CY = !CY; return cycles;
        case 0xEB: { BYTE t = D; D = H; H = t; t = E; E = L; L = t; return cycles; }
        case 0xE3: { BYTE l = Read(SP); BYTE h = Read(SP + 1); Write(SP, L); Write(SP + 1, H); L = l; H = h; return cycles; }
        case 0xF9: SP = GetHL(); return cycles;
        case 0xE9: PC = GetHL(); return cycles;
        case 0xFB:
            EI = true;
            spec_speaker_state = true;  // Включаем динамик через INTE
            return cycles;

        case 0xF3:
            EI = false;
            spec_speaker_state = false; // Выключаем динамик через INTE
            return cycles;
        }
        return cycles;
    }
};
#pragma pack(pop)

CPU8080 cpu;

// --- ЗАГРУЗКА И КОРРЕКТИРОВКА РОМОВ ---
bool LoadRawBinaryFile(const wchar_t* filepath, BYTE* target_dest, size_t max_bytes) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;
    file.read(reinterpret_cast<char*>(target_dest), max_bytes);
    file.close();
    return true;
}

void LoadRoms() {
    cpu.Reset();
    // Загрузка ПЗУ Загрузчика и Монитора Специалиста по адресам C000h и C800h соответственно
    if (!LoadRawBinaryFile(L"loader.bin", &spec_ram[0xC000], 2048)) {
        MessageBoxW(NULL, L"Не удалось открыть файл загрузчика loader.bin!", L"Предупреждение", MB_OK | MB_ICONWARNING);
    }
    if (!LoadRawBinaryFile(L"monitor.bin", &spec_ram[0xC800], 2048)) {
        MessageBoxW(NULL, L"Не удалось открыть файл монитора monitor.bin!", L"Предупреждение", MB_OK | MB_ICONWARNING);
    }
    nr_button_pressed = false; // Клавиша НР отжата по умолчанию (высокий уровень)
}

void UpdateRegisterDisplay() {
    SendMessageW(hRegListBox, LB_RESETCONTENT, 0, 0);
    wchar_t buf[64];
    swprintf(buf, 64, L" PC: %04Xh SP: %04Xh", cpu.PC, cpu.SP);
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    swprintf(buf, 64, L" REG A: %02Xh BC: %04Xh", cpu.A, cpu.GetBC());
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    swprintf(buf, 64, L" REG DE: %04Xh HL: %04Xh", cpu.GetDE(), cpu.GetHL());
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    swprintf(buf, 64, L" Флаги: S:%d Z:%d AC:%d P:%d CY:%d", cpu.S, cpu.Z, cpu.AC, cpu.P, cpu.CY);
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    swprintf(buf, 64, L" Режим экрана: ЧБ 384x256");
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
}

// =========================================================================
// ФУНКЦИЯ ПОПИКСЕЛЬНОГО ВЫВОДА ЭКРАНА СПЕЦИАЛИСТА (384х256)
// =========================================================================
void RenderScreen(HDC hdc, int xOffset, int yOffset) {
    // Очищаем буфер кадра черным цветом
    memset(pixel_buffer, 0, sizeof(pixel_buffer));

    // Отрисовка по линейной структуре видео-ОЗУ Специалиста:
    // Каждые 256 байт формируют последовательные вертикальные линии шагом в 8 пикселей.
    // Весь экран формируется из 48 колонок байт (48 * 8 = 384 пикселей ширины).
    for (int col = 0; col < 48; col++) {
        for (int y = 0; y < 256; y++) {
            // Спецификация: Базовый адрес 9000h + смещение колонок (col * 256) + Y-строка
            WORD addr = 0x9000 + (col * 256) + y;
            BYTE data_byte = spec_ram[addr];

            // Направление прохода инвертировано по вертикали для корректной развертки WinAPI
            int pixel_y = (256 - 1) - y;

            for (int bit = 0; bit < 8; bit++) {
                int pixel_x = (col * 8) + bit;
                // Спецификация: Старшие биты располагаются слева (маска 0x80 >> bit)
                bool active = (data_byte & (0x80 >> bit)) != 0;

                if (pixel_x < 384 && pixel_y >= 0 && pixel_y < 256) {
                    pixel_buffer[pixel_y * 384 + pixel_x] = active ? 0x00FFFFFF : 0x00000000;
                }
            }
        }
    }

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 384;
    bmi.bmiHeader.biHeight = 256;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    // Вывод с масштабированием x2
    StretchDIBits(hdc, xOffset, yOffset, 384 * 2, 256 * 2, 0, 0, 384, 256, pixel_buffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

#ifndef BS_NOFOCUS
#define BS_NOFOCUS 0x00008000L
#endif

// =========================================================================
// ФУНКЦИЯ ГЕНЕРАЦИИ ИНТЕРФЕЙСА КЛАВИАТУРЫ В ТОЧНОМ СООТВЕТСТВИИ С РИСУНКОМ
// =========================================================================
void CreateKeyboardUI(HWND hwndParent, HINSTANCE hInst) {
    const int BTN_W = 44;     // Ширина стандартной клавиши
    const int BTN_H = 40;     // Высота клавиши
    const int START_X = 20;   // Левый отступ клавиатуры
    const int START_Y = 540;  // Верхний отступ блока клавиатуры
    const int GAP = 6;        // Зазор между кнопками

    // Массив надписей для сетки кнопок (6 рядов)
    // Строки 1-5 имеют по 12 основных позиций (кнопка СБР вынесена отдельно).
    // Строка 6 (нижняя) обрабатывается индивидуально из-за Пробела и смещений стрелок.
    static const wchar_t* visual_grid[5][12] = {
        // Ряд 1: Функциональный (первые 12 кнопок, без СБР)
        { L"F", L"HELP", L"NEW", L"LOAD", L"SAVE", L"RUN", L"STOP", L"CONT", L"EDIT", L"■", L"▢", L"⃠" },
        // Ряд 2: Цифровой
        { L";\n+", L"1\n!", L"2\n\"", L"3\n#", L"4\n$", L"5\n%", L"6\n&", L"7\n'", L"8\n(", L"9\n)", L"0", L"-\n=" },
        // Ряд 3: Буквенный Й-Х
        { L"Й\nJ", L"Ц\nC", L"У\nU", L"К\nK", L"Е\nE", L"Н\nN", L"Г\nG", L"Ш\n[", L"Щ\n]", L"З\nZ", L"Х\nH", L":\n*" },
        // Ряд 4: Буквенный Ф-Э
        { L"Ф\nF", L"Ы\nY", L"В\nW", L"А\nA", L"П\nP", L"Р\nR", L"О\nO", L"Л\nL", L"Д\nD", L"Ж\nV", L"Э\n\\", L".\n>" },
        // Ряд 5: Буквенный Я-Ю + знаки + ЗБ
        { L"Я\nQ", L"Ч\n^", L"С\nS", L"М\nM", L"И\nI", L"Т\nT", L"Ь\nX", L"Б\nB", L"Ю\n@", L",\n<", L"/\n?", L"ЗБ" }
    };

    // 1. Отрисовка первых 5 рядов клавиатуры
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 12; c++) {
            // Рассчитываем уникальный ID для каждой кнопки (1000, 1001...)
            int current_id = 1000 + (r * 12) + c;

            // Смещение для рядов 2, 3, 4, 5 (имитация ступенчатого сдвига реальной клавиатуры)
            int row_stagger = 0;
            if (r == 2) row_stagger = 12; // Небольшой сдвиг вправо для строк букв
            if (r == 3) row_stagger = 18;
            if (r == 4) row_stagger = 28;

            int x = START_X + c * (BTN_W + GAP) + row_stagger;
            int y = START_Y + r * (BTN_H + GAP);

            CreateWindowExW(0, L"BUTTON", visual_grid[r][c],
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
                x, y, BTN_W, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
        }
    }

    // 2. Отдельная отрисовка кнопки "СБР" (справа вверху, над клавишей "- =")
    int sbr_x = START_X + 12 * (BTN_W + GAP) + 16;
    CreateWindowExW(0, L"BUTTON", L"СБР",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT | BS_NOFOCUS,
        sbr_x, START_Y, BTN_W, BTN_H, hwndParent, (HMENU)1099, hInst, NULL);

    // 3. Отрисовка 6-го (нижнего) ряда с длинным Пробелом
    int r6_y = START_Y + 5 * (BTN_H + GAP);

    // Структура нижнего ряда: имя, позиция по сетке (колонка), множитель ширины (если не 1)
    struct LowerKey {
        const wchar_t* label;
        int col_pos;
        int width_multiplier;
        int custom_id;
    } lower_keys[] = {
        { L"НР",    0, 1, 1060 },
        { L"Р/Л",   2, 1, 1061 },
        { L"↖",     3, 1, 1062 },
        { L"↑",     4, 1, 1063 },
        { L"↓",     5, 1, 1064 },
        { L"",      6, 4, 1065 }, // Пробел (занимает место 4-х обычных кнопок)
        { L"←",     10, 1, 1066 },
        { L"ПВ",    11, 1, 1067 },
        { L"→",     12, 1, 1068 },
        { L"ПС",    13, 1, 1069 },
        { L"ВК",    14, 1, 1070 }
    };

    for (const auto& key : lower_keys) {
        int x = START_X + key.col_pos * (BTN_W + GAP);
        int w = BTN_W * key.width_multiplier + GAP * (key.width_multiplier - 1);
        DWORD button_style = WS_CHILD | WS_VISIBLE | BS_FLAT | BS_NOFOCUS;

        // Если это кнопка НР (1060) — превращаем в кнопку-фиксатор
        if (key.custom_id == 1060) {
            button_style |= BS_AUTOCHECKBOX | BS_PUSHLIKE;
        }
        else {
            button_style |= BS_PUSHBUTTON;
        }

        CreateWindowExW(0, L"BUTTON", key.label,
            button_style,
            x, r6_y, w, BTN_H, hwndParent, (HMENU)(INT_PTR)key.custom_id, hInst, NULL);
    }
}

int static_virtual_key_duration = 0; // Счетчик автоотжатия кнопок мыши

// Карта соответствия экранных ID (0..59) координатам матрицы Специалиста (строка * 16 + колонка)
// Опирается строго на переданную вами спецификацию строк F1...СТР
// Точная карта соответствия экранных ID (0..59) координатам матрицы Специалиста (строка * 16 + колонка)
// Ряд GUI 0 -> Строка матрицы 0, биты 0..11
// Ряд GUI 1 -> Строка матрицы 1, биты 0..11 и т.д.
static const int spec_hardware_map[60] = {
    // Ряд 1 (Функциональный, строка матрицы 0) -> кнопки 0..11
    0 * 16 + 0, 0 * 16 + 1, 0 * 16 + 2, 0 * 16 + 3, 0 * 16 + 4, 0 * 16 + 5, 0 * 16 + 6, 0 * 16 + 7, 0 * 16 + 8, 0 * 16 + 9, 0 * 16 + 10, 0 * 16 + 11,
    // Ряд 2 (Цифровой, строка матрицы 1) -> кнопки 12..23
    1 * 16 + 0, 1 * 16 + 1, 1 * 16 + 2, 1 * 16 + 3, 1 * 16 + 4, 1 * 16 + 5, 1 * 16 + 6, 1 * 16 + 7, 1 * 16 + 8, 1 * 16 + 9, 1 * 16 + 10, 1 * 16 + 11,
    // Ряд 3 (Буквенный Й-Х, строка матрицы 2) -> кнопки 24..35
    2 * 16 + 0, 2 * 16 + 1, 2 * 16 + 2, 2 * 16 + 3, 2 * 16 + 4, 2 * 16 + 5, 2 * 16 + 6, 2 * 16 + 7, 2 * 16 + 8, 2 * 16 + 9, 2 * 16 + 10, 2 * 16 + 11,
    // Ряд 4 (Буквенный Ф-Э, строка матрицы 3) -> кнопки 36..47
    3 * 16 + 0, 3 * 16 + 1, 3 * 16 + 2, 3 * 16 + 3, 3 * 16 + 4, 3 * 16 + 5, 3 * 16 + 6, 3 * 16 + 7, 3 * 16 + 8, 3 * 16 + 9, 3 * 16 + 10, 3 * 16 + 11,
    // Ряд 5 (Буквенный Я-Ю, строка матрицы 4) -> кнопки 48..59
    4 * 16 + 0, 4 * 16 + 1, 4 * 16 + 2, 4 * 16 + 3, 4 * 16 + 4, 4 * 16 + 5, 4 * 16 + 6, 4 * 16 + 7, 4 * 16 + 8, 4 * 16 + 9, 4 * 16 + 10, 4 * 16 + 11
};
/*
bool MapVirtualKeyToSpec(UINT vkCode, int& out_row, int& out_col) {
    switch (vkCode) {
        // --- ЦИФРОВОЙ РЯД (ИСПРАВЛЕНЫ СТОЛБЦЫ ЗЕРКАЛЬНО) ---
    case 0xBB: out_row = 4; out_col = 11; return true; // ; +
    case '1':  out_row = 4; out_col = 10; return true; // 1 !
    case '2':  out_row = 4; out_col = 9;  return true; // 2 "
    case '3':  out_row = 4; out_col = 8;  return true; // 3 #
    case '4':  out_row = 4; out_col = 7;  return true; // 4 $
    case '5':  out_row = 4; out_col = 6;  return true; // 5 %
    case '6':  out_row = 4; out_col = 5;  return true; // 6 &
    case '7':  out_row = 4; out_col = 4;  return true; // 7 ,
    case '8':  out_row = 4; out_col = 3;  return true; // 8 (
    case '9':  out_row = 4; out_col = 2;  return true; // 9 )
    case '0':  out_row = 4; out_col = 1;  return true; // 0
    case 0xBD: out_row = 4; out_col = 0;  return true; // - =

        // --- РЯД Й-Х (ИСПРАВЛЕНЫ СТОЛБЦЫ ЗЕРКАЛЬНО) ---
    case 'Q':  out_row = 3; out_col = 11; return true; // Й
    case 'W':  out_row = 3; out_col = 10; return true; // Ц
    case 'E':  out_row = 3; out_col = 9;  return true; // У
    case 'R':  out_row = 3; out_col = 8;  return true; // К
    case 'T':  out_row = 3; out_col = 7;  return true; // Е
    case 'Y':  out_row = 3; out_col = 6;  return true; // Н
    case 'U':  out_row = 3; out_col = 5;  return true; // Г
    case 'I':  out_row = 3; out_col = 4;  return true; // Ш
    case 'O':  out_row = 3; out_col = 3;  return true; // Щ
    case 'P':  out_row = 3; out_col = 2;  return true; // З
    case 0xDB: out_row = 3; out_col = 1;  return true; // Х
    case 0xDD: out_row = 3; out_col = 0;  return true; // : *

        // --- РЯД Ф-Э (ИСПРАВЛЕНЫ СТОЛБЦЫ ЗЕРКАЛЬНО) ---
    case 'A':  out_row = 2; out_col = 11; return true; // Ф
    case 'S':  out_row = 2; out_col = 10; return true; // Ы
    case 'D':  out_row = 2; out_col = 9;  return true; // В
    case 'F':  out_row = 2; out_col = 8;  return true; // А
    case 'G':  out_row = 2; out_col = 7;  return true; // П
    case 'H':  out_row = 2; out_col = 6;  return true; // Р
    case 'J':  out_row = 2; out_col = 5;  return true; // О
    case 'K':  out_row = 2; out_col = 4;  return true; // Л
    case 'L':  out_row = 2; out_col = 3;  return true; // Д
    case 0xBA: out_row = 2; out_col = 2;  return true; // Ж
    case 0xDE: out_row = 2; out_col = 1;  return true; // Э
    case 0xBF: out_row = 2; out_col = 0;  return true; // . >

        // --- РЯД Я-Ю (ИСПРАВЛЕНЫ СТОЛБЦЫ ЗЕРКАЛЬНО) ---
    case 'Z':  out_row = 1; out_col = 11; return true; // Я
    case 'X':  out_row = 1; out_col = 10; return true; // Ч
    case 'C':  out_row = 1; out_col = 9;  return true; // С
    case 'V':  out_row = 1; out_col = 8;  return true; // М
    case 'B':  out_row = 1; out_col = 7;  return true; // И
    case 'N':  out_row = 1; out_col = 6;  return true; // Т
    case 'M':  out_row = 1; out_col = 5;  return true; // Ь
    case 0xBC: out_row = 1; out_col = 4;  return true; // Б
    case 0xBE: out_row = 1; out_col = 3;  return true; // Ю
    case 0xEC: out_row = 1; out_col = 0;  return true; // ЗБ (Backspace)
    }
    return false;
}
*/

bool MapVirtualKeyToSpec(UINT vkCode, int& out_row, int& out_col) {
    switch (vkCode) {
        // --- ЦИФРОВОЙ РЯД (Остается стандартным) ---
    case 0xBB: out_row = 3; out_col = 0; return true; // ; +
    case '1':  out_row = 4; out_col = 10; return true; // 1 !
    case '2':  out_row = 4; out_col = 9;  return true; // 2 "
    case '3':  out_row = 4; out_col = 8;  return true; // 3 #
    case '4':  out_row = 4; out_col = 7;  return true; // 4 $
    case '5':  out_row = 4; out_col = 6;  return true; // 5 %
    case '6':  out_row = 4; out_col = 5;  return true; // 6 &
    case '7':  out_row = 4; out_col = 4;  return true; // 7 ,
    case '8':  out_row = 4; out_col = 3;  return true; // 8 (
    case '9':  out_row = 4; out_col = 2;  return true; // 9 )
    case '0':  out_row = 4; out_col = 1;  return true; // 0
    case 0xBD: out_row = 4; out_col = 0;  return true; // - =

        // --- ЛАТИНСКИЕ БУКВЫ A-Z (Маппинг "Буква в Букву" для Специалиста) ---
    case 'A':  out_row = 2; out_col = 8;  return true; // A -> на позицию А / A
    case 'B':  out_row = 1; out_col = 4;  return true; // B -> на позицию Б / B
    case 'C':  out_row = 3; out_col = 10; return true; // C -> на позицию Ц / C
    case 'D':  out_row = 2; out_col = 3;  return true; // D -> на позицию Д / D
    case 'E':  out_row = 3; out_col = 7;  return true; // E -> на позицию Е / E
    case 'F':  out_row = 2; out_col = 11; return true; // F -> на позицию Ф / F
    case 'G':  out_row = 3; out_col = 5;  return true; // G -> на позицию Г / G
    case 'H':  out_row = 3; out_col = 1;  return true; // H -> на позицию Х / H
    case 'I':  out_row = 1; out_col = 7;  return true; // I -> на позицию И / I
    case 'J':  out_row = 3; out_col = 11; return true; // J -> на позицию Й / J
    case 'K':  out_row = 3; out_col = 8;  return true; // K -> на позицию К / K
    case 'L':  out_row = 2; out_col = 4;  return true; // L -> на позицию Л / L
    case 'M':  out_row = 1; out_col = 8;  return true; // M -> на позицию М / M
    case 'N':  out_row = 3; out_col = 6;  return true; // N -> на позицию Н / N
    case 'O':  out_row = 2; out_col = 5;  return true; // O -> на позицию О / O
    case 'P':  out_row = 2; out_col = 7;  return true; // P -> на позицию П / P
    case 'Q':  out_row = 1; out_col = 11; return true; // Q -> на позицию Я / Q
    case 'R':  out_row = 2; out_col = 6;  return true; // R -> на позицию Р / R
    case 'S':  out_row = 1; out_col = 9;  return true; // S -> на позицию С / S
    case 'T':  out_row = 1; out_col = 6;  return true; // T -> на позицию Т / T
    case 'U':  out_row = 3; out_col = 9;  return true; // U -> на позицию У / U
    case 'V':  out_row = 2; out_col = 2;  return true; // V -> на позицию Ж / V
    case 'W':  out_row = 2; out_col = 9;  return true; // W -> на позицию В / W
    case 'X':  out_row = 1; out_col = 5;  return true; // X -> на позицию Ь / X
    case 'Y':  out_row = 2; out_col = 10; return true; // Y -> на позицию Ы / Y
    case 'Z':  out_row = 3; out_col = 2;  return true; // Z -> на позицию З / Z

        // --- СПЕЦИАЛЬНЫЕ СИМВОЛЫ И КЛАВИШИ ---
    case 0xDB: out_row = 3; out_col = 4;  return true; // Клавиша [ -> на Ш / [
    case 0xDD: out_row = 3; out_col = 3;  return true; // Клавиша ] -> на Щ / ]
    case 0xBA: out_row = 1; out_col = 3;  return true; // Клавиша ; -> на : / *
    case 0xDE: out_row = 1; out_col = 10;  return true; // Клавиша ' -> на Э
    case 0xDC: out_row = 2; out_col = 1;  return true; // Клавиша ; -> на : / *
    case 0xC0: out_row = 4; out_col = 11;  return true; // Клавиша ' -> на Э
    case 0xBC: out_row = 1; out_col = 2;  return true; // Клавиша , -> на , / <
    case 0xBE: out_row = 2; out_col = 0;  return true; // Клавиша . -> на . / >
    case 0xBF: out_row = 1; out_col = 1;  return true; // Клавиша / -> на / / ?
    case VK_BACK: out_row = 1; out_col = 0; return true; // Backspace -> ЗБ
    }
    return false;
}


// Загрузка исполняемых образов программ Специалиста (.RKS или сырых .BIN)
void LoadSpecFile(HWND hwnd) {
    OPENFILENAMEW ofn;
    wchar_t szFile[260] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"Файлы Специалиста (*.rks;*.bin;*.spc)\0*.rks;*.bin;*.spc\0Все файлы (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        std::ifstream file(ofn.lpstrFile, std::ios::binary);
        if (!file.is_open()) {
            MessageBoxW(hwnd, L"Не удалось открыть файл!", L"Ошибка", MB_OK | MB_ICONERROR);
            return;
        }

        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        WORD load_address = 0x0000;
        WORD file_length = 0;
        bool has_rks_header = false;

        // Валидация стандартного RKS контейнера (2 байта старта, 2 байта конца)
        if (file_size >= 4) {
            BYTE header[4];
            file.read(reinterpret_cast<char*>(header), 4);
            WORD start_addr = header[0] | (header[1] << 8);
            WORD end_addr = header[2] | (header[3] << 8);

            if (end_addr >= start_addr && (size_t)(end_addr - start_addr + 5) <= file_size) {
                load_address = start_addr;
                file_length = end_addr - start_addr + 1;
                has_rks_header = true;
            }
        }

        if (!has_rks_header) {
            file.seekg(0, std::ios::beg);
            load_address = 0x0000;
            file_length = (file_size > 0x9000) ? 0x9000 : (WORD)file_size;
        }

        file.read(reinterpret_cast<char*>(&spec_ram[load_address]), file_length);
        file.close();

        // Инициализация CPU для запуска программы
        cpu.Reset();
        cpu.PC = load_address;
        cpu.SP = 0x8FFF; // Граница пользовательского ОЗУ перед видеообластью

        UpdateRegisterDisplay();
        InvalidateRect(hwnd, NULL, FALSE);

        wchar_t msg_buf[256];
        swprintf(msg_buf, 256, L"Файл успешно загружен!\nАдрес старта: %04Xh\nРазмер кода: %d байт", load_address, file_length);
        MessageBoxW(hwnd, msg_buf, L"Успех", MB_OK | MB_ICONINFORMATION);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HINSTANCE hInst;
    switch (msg) {
    case WM_CREATE: {
        LPCREATESTRUCTW pcs = (LPCREATESTRUCTW)lp;
        hInst = pcs->hInstance;
        HMENU hSysMenu = GetSystemMenu(hwnd, FALSE);
        if (hSysMenu) {
            AppendMenuW(hSysMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hSysMenu, MF_STRING, IDM_FILE_OPEN, L"Открыть файл Специалиста...");
        }
        CreateKeyboardUI(hwnd, hInst);
        int RIGHT_PANEL_X = 840;
        CreateWindowExW(0, L"BUTTON", L"СБРОС (RESET)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, RIGHT_PANEL_X, 20, 210, 40, hwnd, (HMENU)2000, hInst, NULL);
        hRegListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL, RIGHT_PANEL_X, 75, 210, 245, hwnd, (HMENU)2001, NULL, NULL);

        LoadRoms();
        UpdateRegisterDisplay();
        return 0;
    }
    case WM_SYSCOMMAND: {
        if ((wp & 0xFFF0) == IDM_FILE_OPEN) {
            LoadSpecFile(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_COMMAND: {
        int btn_id = LOWORD(wp);
        // Сброс эмулятора
        if (btn_id == 2000) {
            LoadRoms();
            UpdateRegisterDisplay();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Перехват нажатий с GUI-интерфейса матрицы клавиатуры
        int spec_btn_idx = btn_id - 1000;
        if (spec_btn_idx >= 0 && spec_btn_idx < 60) {
            static_virtual_key_duration = 4;

            int r = 5 - (spec_btn_idx / 12);
            // ИСПРАВЛЕНО: Инвертируем столбцы зеркально по горизонтали
            int c = 11 - (spec_btn_idx % 12);

            if (r >= 0 && r < 6 && c >= 0 && c < 12) {
                spec_key_matrix[r] &= ~(1 << c); // Замыкаем контакт
            }
            SetFocus(hwnd);
            return 0;
        }

        // Отдельная обработка нижнего ряда модификаторов, стрелок и ВК
        switch (btn_id) {
        case 1060: nr_button_pressed = !nr_button_pressed; SetFocus(hwnd); return 0; // НР
        case 1061: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 11); SetFocus(hwnd); return 0; // Р/Л
        case 1062: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 10); SetFocus(hwnd); return 0; // HOME
        case 1063: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 9);  SetFocus(hwnd); return 0; // ↑ (было 2)
        case 1064: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 8);  SetFocus(hwnd); return 0; // ↓ (было 3)
        case 1065: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 5);  SetFocus(hwnd); return 0; // Пробел (было 6)
        case 1066: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 4);  SetFocus(hwnd); return 0; // ← (было 7)
        case 1067: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 3);  SetFocus(hwnd); return 0; // ПВ (было 8)
        case 1068: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 2);  SetFocus(hwnd); return 0; // → (было 9)
        case 1069: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 1);  SetFocus(hwnd); return 0; // ПС (было 10)
        case 1070: static_virtual_key_duration = 4; spec_key_matrix[0] &= ~(1 << 0);  SetFocus(hwnd); return 0; // ВК (было 11)
        }


        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RenderScreen(hdc, 20, 20);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

void FillAudioBuffer(short* buffer, int samplesCount) {
    static short last_sample = 0;

    for (int i = 0; i < samplesCount; i++) {
        // Если в кольцевом буфере есть новые данные от процессора
        if (ring_read_ptr != ring_write_ptr) {
            last_sample = audio_ring_buffer[ring_read_ptr];
            ring_read_ptr = (ring_read_ptr + 1) % RING_BUF_SIZE;
        }
        // Записываем сэмпл в буфер waveOut
        buffer[i] = last_sample;
    }
}

void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        WAVEHDR* pHdr = (WAVEHDR*)dwParam1;
        FillAudioBuffer((short*)pHdr->lpData, AUDIO_BUF_SIZE);
        waveOutWrite(hwo, pHdr, sizeof(WAVEHDR));
    }
}

static double sound_accumulator = 0.0; // Сохраняет остаток тактов между кадрами

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Specialist_Emulator_Core";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"Specialist_Emulator_Core", L"Эмулятор ПК Специалист",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1080, 870, NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)WaveOutCallback, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
        for (int i = 0; i < 2; i++) {
            audioBuffers[i] = new short[AUDIO_BUF_SIZE];
            memset(audioBuffers[i], 0, AUDIO_BUF_SIZE * sizeof(short));
            waveHeader[i].lpData = (LPSTR)audioBuffers[i];
            waveHeader[i].dwBufferLength = AUDIO_BUF_SIZE * sizeof(short);
            waveHeader[i].dwFlags = 0;
            waveOutPrepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
            waveOutWrite(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
        }
    }

    MSG msg;
    LARGE_INTEGER frequency;
    LARGE_INTEGER last_hardware_time;
    double internal_cycles_debt = 0.0;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last_hardware_time);

    const double SYSTEM_CLOCK_HZ = 2000000.0; // Номинальная тактовая частота процессора Специалиста

    while (emulator_running) {
        MsgWaitForMultipleObjectsEx(0, NULL, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                emulator_running = false;
                break;
            }

            // --- НАЖАТИЕ ФИЗИЧЕСКИХ КЛАВИШ (WM_KEYDOWN) ---
            if (msg.message == WM_KEYDOWN) {
                if (!(msg.lParam & (1 << 30))) { // Игнорируем автоповтор Windows
                    UINT vk = (UINT)msg.wParam;
                    if (vk == VK_SHIFT) {
                        nr_button_pressed = true;
                    }
                    else if (vk == VK_CAPITAL) {
                        spec_key_matrix[0] &= ~(1 << 11);
                    }
                    if (vk == VK_SPACE) { spec_key_matrix[0] &= ~(1 << 5); } // Пробел
                    else if (vk == VK_HOME) { spec_key_matrix[0] &= ~(1 << 10); } // HOME
                    else if (vk == VK_TAB) { spec_key_matrix[0] &= ~(1 << 7); } // ТАБ
                    else if (vk == VK_ESCAPE) { spec_key_matrix[0] &= ~(1 << 6); } // АР2
                    else if (vk == VK_UP) { spec_key_matrix[0] &= ~(1 << 9); } // Стрелка Вверх
                    else if (vk == VK_DOWN) { spec_key_matrix[0] &= ~(1 << 8); } // Стрелка Вниз
                    else if (vk == VK_LEFT) { spec_key_matrix[0] &= ~(1 << 4); } // Стрелка Влево
                    else if (vk == VK_RIGHT) { spec_key_matrix[0] &= ~(1 << 2); } // Стрелка Вправо
                    else if (vk == VK_RETURN) { spec_key_matrix[0] &= ~(1 << 0); } // ВК (Enter)
                    else {
                        int target_row = -1, target_col = -1;
                        if (MapVirtualKeyToSpec(vk, target_row, target_col)) {
                            spec_key_matrix[target_row] &= ~(1 << target_col); // Замыкаем контакт
                        }
                    }
                }
            }

            // --- ОТПУСКАНИЕ ФИЗИЧЕСКИХ КЛАВИШ (WM_KEYUP) ---
            else if (msg.message == WM_KEYUP) {
                UINT vk = (UINT)msg.wParam;
                if (vk == VK_SHIFT) {
                    nr_button_pressed = false;
                }
                else if (vk == VK_CAPITAL) {
                    spec_key_matrix[0] |= (1 << 11);
                }
                if (vk == VK_SPACE) { spec_key_matrix[0] |= (1 << 5); }
                else if (vk == VK_HOME) { spec_key_matrix[0] |= (1 << 10); }
                else if (vk == VK_TAB) { spec_key_matrix[0] |= (1 << 7); }
                else if (vk == VK_ESCAPE) { spec_key_matrix[0] |= (1 << 6); }
                else if (vk == VK_UP) { spec_key_matrix[0] |= (1 << 9); }
                else if (vk == VK_DOWN) { spec_key_matrix[0] |= (1 << 8); }
                else if (vk == VK_LEFT) { spec_key_matrix[0] |= (1 << 4); }
                else if (vk == VK_RIGHT) { spec_key_matrix[0] |= (1 << 2); }
                else if (vk == VK_RETURN) { spec_key_matrix[0] |= (1 << 0); }
                else {
                    int target_row = -1, target_col = -1;
                    if (MapVirtualKeyToSpec(vk, target_row, target_col)) {
                        spec_key_matrix[target_row] |= (1 << target_col); // Размыкаем контакт
                    }
                }
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!emulator_running) break;

        LARGE_INTEGER current_hardware_time;
        QueryPerformanceCounter(&current_hardware_time);
        double elapsed_seconds = (double)(current_hardware_time.QuadPart - last_hardware_time.QuadPart) / frequency.QuadPart;

        if (elapsed_seconds > 0.1) elapsed_seconds = 0.1;
        if (elapsed_seconds > 0.0) {
            last_hardware_time = current_hardware_time;

            // Накапливаем такты, которые должен выполнить процессор
            internal_cycles_debt += elapsed_seconds * SYSTEM_CLOCK_HZ;

            // Ограничиваем максимальный долг (защита от "прыжка времени" при перетаскивании окна)
            if (internal_cycles_debt > 80000.0) {
                internal_cycles_debt = 40000.0;
            }
        }

        // Выполняем ровно столько тактов, сколько накопилось в долге
        if (internal_cycles_debt >= 4.0) { // Исполняем, если накопилось хотя бы на минимальную инструкцию
            int cycles_to_execute = (int)internal_cycles_debt;
            int cycles_actually_executed = 0;

            while (cycles_actually_executed < cycles_to_execute) {
                int elapsed_ticks = cpu.Step();
                cycles_actually_executed += elapsed_ticks;

                // Звуковой аккумулятор считает по факту выполненных тактов
                sound_accumulator += elapsed_ticks;
                while (sound_accumulator >= 90.7029) {
                    sound_accumulator -= 90.7029;
                    short sample = spec_speaker_state ? 4000 : -4000;
                    audio_ring_buffer[ring_write_ptr] = sample;
                    ring_write_ptr = (ring_write_ptr + 1) % RING_BUF_SIZE;
                }

                // Кадровое прерывание (50 Гц)
                cpu.cycles_until_interrupt -= elapsed_ticks;
                if (cpu.cycles_until_interrupt <= 0) {
                    cpu.int_pending = true;
                    cpu.cycles_until_interrupt += 40000; // Ровно 40 тыс тактов на кадр при 2 МГц

                    // Синхронизация экранной кнопки НР
                    HWND hCheckButtonHP = GetDlgItem(hwnd, 1060);
                    if (hCheckButtonHP != NULL) {
                        LRESULT curCheck = SendMessageW(hCheckButtonHP, BM_GETCHECK, 0, 0);
                        if ((curCheck == BST_CHECKED) != nr_button_pressed) {
                            SendMessageW(hCheckButtonHP, BM_SETCHECK, nr_button_pressed ? BST_CHECKED : BST_UNCHECKED, 0);
                            InvalidateRect(hCheckButtonHP, NULL, TRUE);
                        }
                    }

                    // Автоотжатие GUI кнопок мыши
                    if (static_virtual_key_duration > 0) {
                        static_virtual_key_duration--;
                        if (static_virtual_key_duration == 0) {
                            for (int i = 0; i < 6; i++) {
                                spec_key_matrix[i] = 0xFFF;
                            }
                        }
                    }

                    UpdateRegisterDisplay();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }

            // Вычитаем из глобального долга только то, что РЕАЛЬНО успели выполнить
            internal_cycles_debt -= cycles_actually_executed;
        }
        else {
            // Если процессор обогнал время или такты ещё не накопились,
            // даём Windows отдохнуть (1 мс), разгружая CPU вашего ПК
            Sleep(1);
        }
    } // Конец цикла while(emulator_running)

    if (hWaveOut) {
        for (int i = 0; i < 2; i++) {
            waveOutUnprepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
            delete[] audioBuffers[i];
        }
        waveOutClose(hWaveOut);
    }
    return (int)msg.wParam;
}
