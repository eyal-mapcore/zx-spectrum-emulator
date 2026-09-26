# pragma once
#include <SDL2/SDL.h>
#include "globals.h"

struct SpectrumKeyMapping {
    SDL_Scancode scancode;
    uint8_t row; // Index 0 to 7 (corresponds to A8-A15)
    uint8_t bit; // Bit 0 to 4
};


const SpectrumKeyMapping keymap[] = {
    { SDL_SCANCODE_LSHIFT, 0, 0 }, // CAPS SHIFT -> Row 0 (A8), Bit 0
    { SDL_SCANCODE_Z,      0, 1 }, // Z          -> Row 0 (A8), Bit 1
    { SDL_SCANCODE_X,      0, 2 }, // X          -> Row 0 (A8), Bit 2
    { SDL_SCANCODE_C,      0, 3 }, // C          -> Row 0 (A8), Bit 3
    { SDL_SCANCODE_V,      0, 4 }, // V          -> Row 0 (A8), Bit 4

    { SDL_SCANCODE_A,      1, 0 }, // A          -> Row 1 (A9), Bit 0
    { SDL_SCANCODE_S,      1, 1 }, // S          -> Row 1 (A9), Bit 1
    { SDL_SCANCODE_D,      1, 2 }, // D          -> Row 1 (A9), Bit 2
    { SDL_SCANCODE_F,      1, 3 }, // F          -> Row 1 (A9), Bit 3
    { SDL_SCANCODE_G,      1, 4 }, // G          -> Row 1 (A9), Bit 4

    { SDL_SCANCODE_Q,      2, 0 }, // Q          -> Row 2 (A10), Bit 0
    { SDL_SCANCODE_W,      2, 1 }, // W          -> Row 2 (A10), Bit 1
    { SDL_SCANCODE_E,      2, 2 }, // E          -> Row 2 (A10), Bit 2
    { SDL_SCANCODE_R,      2, 3 }, // R          -> Row 2 (A10), Bit 3
    { SDL_SCANCODE_T,      2, 4 }, // T          -> Row 2 (A10), Bit 4

    { SDL_SCANCODE_1,      3, 0 }, // 1          -> Row 3 (A11), Bit 0
    { SDL_SCANCODE_2,      3, 1 }, // 2          -> Row 3 (A11), Bit 1
    { SDL_SCANCODE_3,      3, 2 }, // 3          -> Row 3 (A11), Bit 2
    { SDL_SCANCODE_4,      3, 3 }, // 4          -> Row 3 (A11), Bit 3
    { SDL_SCANCODE_5,      3, 4 }, // 5          -> Row 3 (A11), Bit 4

    { SDL_SCANCODE_0,      4, 0 }, // 0          -> Row 4 (A12), Bit 0
    { SDL_SCANCODE_9,      4, 1 }, // 9          -> Row 4 (A12), Bit 1
    { SDL_SCANCODE_8,      4, 2 }, // 8          -> Row 4 (A12), Bit 2
    { SDL_SCANCODE_7,      4, 3 }, // 7          -> Row 4 (A12), Bit 3
    { SDL_SCANCODE_6,      4, 4 }, // 6          -> Row 4 (A12), Bit 4

    { SDL_SCANCODE_P,      5, 0 }, // P          -> Row 5 (A13), Bit 0
    { SDL_SCANCODE_O,      5, 1 }, // O          -> Row 5 (A13), Bit 1
    { SDL_SCANCODE_I,      5, 2 }, // I          -> Row 5 (A13), Bit 2
    { SDL_SCANCODE_U,      5, 3 }, // U          -> Row 5 (A13), Bit 3
    { SDL_SCANCODE_Y,      5, 4 }, // Y          -> Row 5 (A13), Bit 4

    { SDL_SCANCODE_RETURN, 6, 0 }, // ENTER      -> Row 6 (A14), Bit 0
    { SDL_SCANCODE_L,      6, 1 }, // L          -> Row 6 (A14), Bit 1
    { SDL_SCANCODE_K,      6, 2 }, // K          -> Row 6 (A14), Bit 2
    { SDL_SCANCODE_J,      6, 3 }, // J          -> Row 6 (A14), Bit 3
    { SDL_SCANCODE_H,      6, 4 }, // H          -> Row 6 (A14), Bit 4

    { SDL_SCANCODE_SPACE,  7, 0 }, // SPACE      -> Row 7 (A15), Bit 0
    { SDL_SCANCODE_RSHIFT, 7, 1 }, // SYM SHIFT  -> Row 7 (A15), Bit 1
    { SDL_SCANCODE_M,      7, 2 }, // M          -> Row 7 (A15), Bit 2
    { SDL_SCANCODE_N,      7, 3 }, // N          -> Row 7 (A15), Bit 3
    { SDL_SCANCODE_B,      7, 4 }  // B          -> Row 7 (A15), Bit 4
};