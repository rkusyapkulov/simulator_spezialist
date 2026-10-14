#ifndef CPU_8080_H
#define CPU_8080_H

#include <stdint.h>
#if defined(__cplusplus)
    /* в C++ bool встроен */
#elif defined(_MSC_VER) && _MSC_VER < 1800
    /* VS2010/2012 не имеют <stdbool.h> (появился в VS2013) */
    typedef unsigned char bool;
    #define true 1
    #define false 0
#else
    #include <stdbool.h>
#endif

#pragma pack(push, 1)

// --- СТРУКТУРА ПРОЦЕССОРА INTEL 8080 (КР580ВМ80А) ---
typedef struct {
    uint8_t A, B, C, D, E, H, L;            // Основные регистры
    uint16_t PC, SP;                        // Регистр счетчика команд и указатель стека
    
    // Флаги процессора
    bool S, Z, AC, P, CY;                   // Sign, Zero, Auxiliary Carry, Parity, Carry
    bool EI;                                // Разрешение прерываний
    bool halted;                            // Состояние останова процессора (HLT)
    
    int cycles_until_interrupt;             // Счетчик тактов до генерации прерывания
    bool int_pending;                       // Флаг отложенного аппаратного прерывания
    
    // Порты КР580ВВ55 (ППА)
    uint8_t ppi0_pa, ppi0_pb, ppi0_pc, ppi0_ctrl;
    uint8_t ppi1_pa, ppi1_pb, ppi1_pc, ppi1_ctrl;
} CPU8080;
#pragma pack(pop)

#endif // CPU_8080_H

