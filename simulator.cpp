#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <commdlg.h>
#include "8080.h"

#pragma comment (lib, "libcmt.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment (lib, "User32.lib")
#pragma comment (lib, "Gdi32.lib")
#pragma comment (lib, "Comdlg32.lib")

extern "C" int cpu8080_step(CPU8080* cpu);
extern "C" void cpu8080_reset(CPU8080* cpu);

#define SYSTEM_CLOCK_HZ 2000000


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

void Reset(CPU8080* cpu) {
    nr_button_pressed = false;

    cpu8080_reset(cpu);
}

// --- АППАРАТНЫЙ ДИСПЕТЧЕР ЧТЕНИЯ КЛАВИАТУРЫ И ПАМЯТИ "СПЕЦИАЛИСТА" ---
extern "C" uint8_t cpu8080_read(CPU8080* cpu, uint16_t addr) {
    // Дешифрация системного ППА №2 (Клавиатура/Звук: 0xF800-0xFFFF или зеркало 0xFF00-0xFF03)
    if ((addr >= 0xF800 && addr <= 0xFFFF) || (addr & 0xFFFC) == 0xFF00) {
        switch (addr & 3) {
        case 0: // --- ПОРТ А (Младшие 8 бит столбцов) ---
            if (cpu->ppi1_ctrl & 0x10) { // Если настроен на ввод
                BYTE res = 0xFF;
                for (int r = 0; r < 6; r++) {
                    // Опрашиваем по маске процессора ИЛИ принудительно инжектируем, если кнопка нажата
                    if (!(cpu->ppi1_pb & (1 << (r + 2))) || spec_key_matrix[r] != 0xFFF) {
                        res &= (spec_key_matrix[r] & 0xFF);
                    }
                }
                return res;
            }
            return cpu->ppi1_pa;

        case 1: // --- ПОРТ В (Строки матрицы биты 2-7 + Кн. НР бит 1) ---
            if (cpu->ppi1_ctrl & 0x02) { // Если настроен на ввод
                BYTE res = 0xFF;
                WORD cols = cpu->ppi1_pa | ((cpu->ppi1_pc & 0x0F) << 8);

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
            return nr_button_pressed ? (cpu->ppi1_pb & ~0x02) : (cpu->ppi1_pb | 0x02);

        case 2: // --- ПОРТ С (Старшие 4 бита столбцов) ---
            if (cpu->ppi1_ctrl & 0x01) { // Если младшая половина PC на ввод
                BYTE res = 0x0F;
                for (int r = 0; r < 6; r++) {
                    if (!(cpu->ppi1_pb & (1 << (r + 2))) || spec_key_matrix[r] != 0xFFF) {
                        res &= ((spec_key_matrix[r] >> 8) & 0x0F);
                    }
                }
                return (cpu->ppi1_pc & 0xF0) | res;
            }
            return cpu->ppi1_pc;

        case 3: // Управляющее слово
            return cpu->ppi1_ctrl;
        }
    }

    // Пользовательский ППА №1 (0xF000 - 0xF7FF)
    if (addr >= 0xF000 && addr <= 0xF7FF) {
        switch (addr & 3) {
        case 0: return cpu->ppi0_pa;
        case 1: return cpu->ppi0_pb;
        case 2: return cpu->ppi0_pc;
        case 3: return cpu->ppi0_ctrl;
        }
    }

    return spec_ram[addr]; // Обычное линейное ОЗУ
}

// --- ДИСПЕТЧЕР ЗАПИСИ "СПЕЦИАЛИСТА" ---
extern "C" void cpu8080_write(CPU8080* cpu, uint16_t addr, uint8_t val) {
    if (addr >= 0xC000 && addr <= 0xCFFF) return; // ПЗУ защита

    // Обработка системного ППА №2 (Включая зеркало 0xFF00-0xFF03)
    if ((addr >= 0xF800 && addr <= 0xFFFF) || (addr & 0xFFFC) == 0xFF00) {
        WORD reg = addr & 3;
        if (reg == 0) { cpu->ppi1_pa = val; }
        else if (reg == 1) { cpu->ppi1_pb = val; }
        else if (reg == 2) {
            cpu->ppi1_pc = val;
            spec_speaker_state = (val & 0x20) != 0; // Звук бипера
        }
        else { cpu->ppi1_ctrl = val; 
            // --- ДОБАВЛЕНА ПОДДЕРЖКА РЕЖИМА BSR И ЗВУКА ЗАГРУЗЧИКА (STA 0xFF03) ---
            // Если старший бит (Бит 7) равен 0, это команда BSR (побитовое изменение Порта C)
            if ((val & 0x80) == 0) {
                int bit_select = (val >> 1) & 7; // Выделяем номер бита (биты 1-3)
                int bit_value = val & 1;         // Выделяем значение бита (бит 0)

                // Если загрузчик или игра меняет Бит 5 Порта C
                if (bit_select == 5) {
                    spec_speaker_state = (bit_value == 1);
                }

                // Синхронизируем изменение в самом виртуальном порту cpu->ppi1_pc
                if (bit_value) cpu->ppi1_pc |= (1 << bit_select);
                else           cpu->ppi1_pc &= ~(1 << bit_select);
            }
        }
        return;
    }

    // Пользовательский ППА №1
    if (addr >= 0xF000 && addr <= 0xF7FF) {
        WORD reg = addr & 3;
        if (reg == 0) cpu->ppi0_pa = val;
        else if (reg == 1) cpu->ppi0_pb = val;
        else if (reg == 2) cpu->ppi0_pc = val;
        else cpu->ppi0_ctrl = val;
        return;
    }

    spec_ram[addr] = val;
}

CPU8080 cpu;

// --- ЗАГРУЗКА И КОРРЕКТИРОВКА РОМОВ ---
bool LoadRawBinaryFile(const wchar_t* filepath, BYTE* target_dest, size_t max_bytes) {
    // Открываем файл через системные вызовы Windows
    HANDLE hFile = CreateFileW(
        filepath, 
        GENERIC_READ, 
        FILE_SHARE_READ, 
        NULL, 
        OPEN_EXISTING, 
        FILE_ATTRIBUTE_NORMAL, 
        NULL
    );
    
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesRead = 0;
    // Читаем бинарные данные напрямую в эмулируемую память
    BOOL success = ReadFile(hFile, target_dest, (DWORD)max_bytes, &bytesRead, NULL);
    
    CloseHandle(hFile);
    return success && (bytesRead > 0);
}

void LoadRoms() {
    Reset(&cpu);
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
    
    // Выделяем избыточный буфер в стеке во избежание переполнения строки
    wchar_t buf[128];
    
    wsprintfW(buf, L" PC: %04Xh SP: %04Xh", cpu.PC, cpu.SP);
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    
    wsprintfW(buf, L" REG A: %02Xh BC: %04Xh", cpu.A, (uint16_t)((cpu.B << 8) | cpu.C));
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    
    wsprintfW(buf, L" REG DE: %04Xh HL: %04Xh", (uint16_t)((cpu.D << 8) | cpu.E), (uint16_t)((cpu.H << 8) | cpu.L));
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    
    wsprintfW(buf, L" Флаги: S:%d Z:%d AC:%d P:%d CY:%d", cpu.S, cpu.Z, cpu.AC, cpu.P, cpu.CY);
    SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
    
    wsprintfW(buf, L" Режим экрана: ЧБ 384x256");
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

	for (int i = 0; i < sizeof(lower_keys)/sizeof(LowerKey); i++) {
		LowerKey key = lower_keys[i];
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

    // --- ФУНКЦИОНАЛЬНЫЙ РЯД (F1 - F12) ---
    if (vkCode >= VK_F1 && vkCode <= VK_F12) {
        out_row = 5;                  // Функциональный ряд на ПК "Специалист" — это строка 5
        out_col = 11 - (vkCode - VK_F1); // Инвертируем столбцы зеркально по горизонтали (как в WM_COMMAND)
        return true;
    }

    return false;
}


// Загрузка исполняемых образов программ Специалиста (.RKS или сырых .BIN)
void LoadSpecFile(HWND hwnd) {
    OPENFILENAMEW ofn = { 0 }; // Обнуление средствами компилятора без memset
    wchar_t szFile[260] = { 0 };
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"Файлы Специалиста (*.rks;*.bin;*.spc)\0*.rks;*.bin;*.spc\0Все файлы (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        HANDLE hFile = CreateFileW(ofn.lpstrFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            MessageBoxW(hwnd, L"Не удалось открыть файл!", L"Ошибка", MB_OK | MB_ICONERROR);
            return;
        }

        LARGE_INTEGER fileSize;
        GetFileSizeEx(hFile, &fileSize);
        size_t file_size = (size_t)fileSize.QuadPart;

        WORD load_address = 0x0000;
        WORD file_length = 0;
        bool has_rks_header = false;
        DWORD bytesRead = 0;

        // Валидация стандартного RKS контейнера (2 байта старта, 2 байта конца)
        if (file_size >= 4) {
            BYTE header[4];
            if (ReadFile(hFile, header, 4, &bytesRead, NULL) && bytesRead == 4) {
                WORD start_addr = header[0] | (header[1] << 8);
                WORD end_addr = header[2] | (header[3] << 8);

                if (end_addr >= start_addr && (size_t)(end_addr - start_addr + 5) <= file_size) {
                    load_address = start_addr;
                    file_length = end_addr - start_addr + 1;
                    has_rks_header = true;
                }
            }
        }

        if (!has_rks_header) {
            SetFilePointer(hFile, 0, NULL, FILE_BEGIN); // Перемотка в начало, аналог seekg
            load_address = 0x0000;
            file_length = (file_size > 0x9000) ? 0x9000 : (WORD)file_size;
        }

        ReadFile(hFile, &spec_ram[load_address], file_length, &bytesRead, NULL);
        CloseHandle(hFile);

        // Инициализация CPU для запуска программы
        Reset(&cpu);
        cpu.PC = load_address;
        cpu.SP = 0x8FFF; // Граница пользовательского ОЗУ перед видеообластью

        UpdateRegisterDisplay();
        InvalidateRect(hwnd, NULL, FALSE);

        wchar_t msg_buf[256];
        wsprintfW(msg_buf, L"Файл успешно загружен!\nАдрес старта: %04Xh\nРазмер кода: %d байт", load_address, file_length);
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
			Reset(&cpu);
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

// Флаг завершения: после waveOutReset колбэк получает WOM_DONE для всех буферов и не должен ставить их в очередь снова
static volatile bool audio_closing = false;

void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE && !audio_closing) {
        WAVEHDR* pHdr = (WAVEHDR*)dwParam1;
        FillAudioBuffer((short*)pHdr->lpData, AUDIO_BUF_SIZE);
        waveOutWrite(hwo, pHdr, sizeof(WAVEHDR));
    }
}

static double sound_accumulator = 0.0; // Сохраняет остаток тактов между кадрами

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    LoadRoms();
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

	// --- Инициализация звуковой подсистемы waveOut ---
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

        // --- ДИСПЕТЧЕР И ТАКТОВАЯ СИНХРОНИЗАЦИЯ ЯДРА ---
        LARGE_INTEGER current_hardware_time;
        QueryPerformanceCounter(&current_hardware_time);
        double elapsed_seconds = (double)(current_hardware_time.QuadPart - last_hardware_time.QuadPart) / frequency.QuadPart;

        if (elapsed_seconds > 0.1) elapsed_seconds = 0.1; // Защита от лагов Windows

        if (elapsed_seconds > 0.0) {
            last_hardware_time = current_hardware_time;
            internal_cycles_debt += elapsed_seconds * SYSTEM_CLOCK_HZ; // Накапливаем такты ПК

            int cycles_to_execute = (int)internal_cycles_debt;
            if (cycles_to_execute > 0) {
                while (cycles_to_execute > 0) {
                    int elapsed_ticks = cpu8080_step(&cpu);

                    cycles_to_execute -= elapsed_ticks;
                    internal_cycles_debt -= elapsed_ticks;

                    // Звуковой аккумулятор считает по факту выполненных тактов
                    sound_accumulator += elapsed_ticks;
                    while (sound_accumulator >= 90.7029) {
                        sound_accumulator -= 90.7029;
                        short sample = spec_speaker_state ? 4000 : -4000;
                        audio_ring_buffer[ring_write_ptr] = sample;
                        ring_write_ptr = (ring_write_ptr + 1) % RING_BUF_SIZE;
                    }

                    // Отсчитываем такты до кадрового прерывания 50 Гц
                    cpu.cycles_until_interrupt -= elapsed_ticks;
                    if (cpu.cycles_until_interrupt <= 0) {
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

                        // Обновляем графику и окна отладки строго 1 раз по завершении кадра
                        UpdateRegisterDisplay();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
            }
        }

        if (internal_cycles_debt <= 0.0) {
            Sleep(1); // Освобождаем CPU ПК
        }
    }

    // --- Безопасная очистка аудиодескрипторов перед выходом ---
    if (hWaveOut) {
        audio_closing = true;    // колбэк больше не ставит буферы в очередь
        waveOutReset(hWaveOut); // вернуть все буферы, иначе Unprepare/Close завершатся ошибкой
        for (int i = 0; i < 2; i++) {
            waveOutUnprepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
            delete[] audioBuffers[i];
        }
        waveOutClose(hWaveOut);
    }
    return (int)msg.wParam; 
}
