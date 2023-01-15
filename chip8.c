#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include "SDL.h"

typedef struct {
    SDL_Window          *window;
    SDL_Renderer        *renderer;
    SDL_AudioSpec       want, have;
    SDL_AudioDeviceID   dev;
} sdl_t;

typedef enum {
    QUIT = 0,
    RUNNING,
    PAUSED,
} emulator_state_t;

typedef enum {
    CHIP8,
    SUPERCHIP,
    XOCHIP,
} extension_t;

typedef struct {
    char        *window_title;
    uint32_t    window_width;
    uint32_t    window_height;
    uint32_t    fg_color;
    uint32_t    bg_color;
    uint32_t    scale_factor;
    bool        pixel_outlines;
    uint32_t    insts_per_sec;
    uint32_t    square_wave_freq;
    uint32_t    audio_sample_rate;
    int16_t     volume;
    float       color_lerp_rate;
    extension_t current_extension;
} config_t;

typedef struct {
    uint16_t    opcode;
    uint16_t    NNN;
    uint8_t     NN;
    uint8_t     N;
    uint8_t     X;
    uint8_t     Y;
} instruction_t;

typedef struct {
    emulator_state_t    state;
    uint8_t             ram[4096];
    bool                display[64*32];
    uint32_t            pixel_color[64*32];
    uint16_t            stack[12];
    uint16_t            *stack_ptr;
    uint8_t             V[16];
    uint16_t            I;
    uint16_t            PC;
    uint8_t             delay_timer;
    uint8_t             sound_timer;
    bool                keypad[16];
    const char          *rom_name;
    instruction_t       inst;
    bool                draw;
} chip8_t;

uint32_t color_lerp(const uint32_t start_color, const uint32_t end_color, const float t)
{
    const uint8_t s_r = (start_color >> 24) & 0xFF;
    const uint8_t s_g = (start_color >> 16) & 0xFF;
    const uint8_t s_b = (start_color >>  8) & 0xFF;
    const uint8_t s_a = (start_color >>  0) & 0xFF;

    const uint8_t e_r = (end_color >> 24) & 0xFF;
    const uint8_t e_g = (end_color >> 16) & 0xFF;
    const uint8_t e_b = (end_color >>  8) & 0xFF;
    const uint8_t e_a = (end_color >>  0) & 0xFF;

    const uint8_t ret_r = ((1 - t) * s_r) + (t * e_r);
    const uint8_t ret_g = ((1 - t) * s_g) + (t * e_g);
    const uint8_t ret_b = ((1 - t) * s_b) + (t * e_b);
    const uint8_t ret_a = ((1 - t) * s_a) + (t * e_a);

    return (ret_r << 24) | (ret_g << 16) | (ret_b << 8) | ret_a;
}

void audio_callback(void *userdata, uint8_t *stream, int len)
{
    config_t *config = (config_t *)userdata;
    
    int16_t *audio_data = (int16_t *)stream;
    uint32_t running_sample_index = 0;
    const int32_t square_wave_period = config->audio_sample_rate / config->square_wave_freq;
    const int32_t half_square_wave_period = square_wave_period / 2;

    int i;
    for (i = 0; i < len / 2; ++i)
        audio_data[i] = ((running_sample_index++ / half_square_wave_period) % 2) ? 
                        config->volume : -config->volume;
}

bool init_sdl(sdl_t *sdl, config_t *config)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        SDL_Log("Could not initialize SDL %s\n", SDL_GetError());
        return false;
    }

    sdl->window = SDL_CreateWindow(config->window_title, 
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    config->window_width * config->scale_factor,
                                    config->window_height * config->scale_factor,
                                    0);

    if (!sdl->window) {
        SDL_Log("Could not create SDL window %s\n", SDL_GetError());
        return false;
    }

    sdl->renderer = SDL_CreateRenderer(sdl->window, -1, SDL_RENDERER_ACCELERATED);

    if (!sdl->renderer) {
        SDL_Log("Could not create SDL renderer %s\n", SDL_GetError());
        return false;
    }

    sdl->want = (SDL_AudioSpec) {
        .freq       = 44100,
        .format     = AUDIO_S16LSB,
        .channels   = 1,
        .samples    = 512,
        .callback   = audio_callback,
        .userdata   = config,
    };

    sdl->dev = SDL_OpenAudioDevice(NULL, 0, &sdl->want, &sdl->have, 0);

    if (sdl->dev == 0) {
        SDL_Log("Could not get an Audio Device %s\n", SDL_GetError());
        return false;
    }

    if ((sdl->want.format != sdl->have.format) ||
        (sdl->want.channels != sdl->have.channels)) {
            SDL_Log("Could not get an Audio Spec\n");
            return false;
        }
        

    return true;
}

bool set_config_from_args(config_t *config, const int argc, char **argv)
{
    *config = (config_t) {
        .window_title       = "CHIP8",
        .window_width       = 64,
        .window_height      = 32,
        .fg_color           = 0xFFFFFFFF,
        .bg_color           = 0x000000FF,
        .scale_factor       = 20,
        .pixel_outlines     = true,
        .insts_per_sec      = 700,
        .square_wave_freq   = 440,
        .audio_sample_rate  = 44100,
        .volume             = 3000,
        .color_lerp_rate    = 0.7,
        .current_extension  = CHIP8,
    };

    int8_t i;
    for (i = 1; i < argc; ++i)
        if (strncmp(argv[i], "--scale-factor", strlen("--scale-factor")) == 0)
            config->scale_factor = (uint32_t)strtol(argv[++i], NULL, 10);

    return true;
}

bool init_chip8(chip8_t *chip8, const config_t config, const char rom_name[])
{
    const uint32_t entry_point = 0x200; // CHIP8 ROM entry point
    const uint8_t font[] = {
        0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
        0x20, 0x60, 0x20, 0x20, 0x70, // 1
        0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
        0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
        0x90, 0x90, 0xF0, 0x10, 0x10, // 4
        0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
        0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
        0xF0, 0x10, 0x20, 0x40, 0x40, // 7
        0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
        0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
        0xF0, 0x90, 0xF0, 0x90, 0x90, // A
        0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
        0xF0, 0x80, 0x80, 0x80, 0xF0, // C
        0xE0, 0x90, 0x90, 0x90, 0xE0, // D
        0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
        0xF0, 0x80, 0xF0, 0x80, 0x80  // F
    };

    // Initialize entire CHIP8 machine
    memset(chip8, 0, sizeof(chip8_t));

    memcpy(chip8->ram, font, sizeof(font));

    FILE *rom = fopen(rom_name, "rb");
    if (!rom) {
        SDL_Log("ROM file %s is invalid or does not exist\n", rom_name);
        return false;
    }

    fseek(rom, 0, SEEK_END);
    const size_t rom_size = ftell(rom);
    const size_t max_size = sizeof(chip8->ram) - entry_point;
    rewind(rom);

    if (rom_size > max_size) {
        SDL_Log("ROM file %s is too big. ROM size: %llu, max allowed size: %llu\n", 
                rom_name, (long long unsigned)rom_size, (long long unsigned)max_size);
        return false;
    }
    
    if (fread(&chip8->ram[entry_point], rom_size, 1, rom) != 1) {
        SDL_Log("Could not read ROM file %s into CHIP8 memory\n", rom_name);
        return false;
    }

    fclose(rom);
    
    chip8->state = RUNNING;
    chip8->PC = entry_point;
    chip8->rom_name = rom_name;
    chip8->stack_ptr = &chip8->stack[0];
    memset(chip8->pixel_color, config.bg_color, sizeof(chip8->pixel_color));

    return true;
}

void final_cleanup(const sdl_t sdl)
{
    SDL_DestroyRenderer(sdl.renderer);
    SDL_DestroyWindow(sdl.window);
    SDL_CloseAudioDevice(sdl.dev);
    SDL_Quit();
}

void clear_screen(const sdl_t sdl, const config_t config)
{
    const uint8_t r = (config.bg_color >> 24) & 0xFF;
    const uint8_t g = (config.bg_color >> 16) & 0xFF;
    const uint8_t b = (config.bg_color >>  8) & 0xFF;
    const uint8_t a = (config.bg_color >>  0) & 0xFF;

    SDL_SetRenderDrawColor(sdl.renderer, r, g, b, a);
    SDL_RenderClear(sdl.renderer);
}

void update_screen(const sdl_t sdl, const config_t config, chip8_t *chip8)
{
    SDL_Rect rect = {.x = 0, .y = 0, .w = config.scale_factor, .h = config.scale_factor}; 

    const uint8_t bg_r = (config.bg_color >> 24) & 0xFF;
    const uint8_t bg_g = (config.bg_color >> 16) & 0xFF;
    const uint8_t bg_b = (config.bg_color >>  8) & 0xFF;
    const uint8_t bg_a = (config.bg_color >>  0) & 0xFF;

    uint32_t i;
    for(i = 0; i < sizeof(chip8->display); ++i) {
        rect.x = (i % config.window_width) * config.scale_factor;
        rect.y = (i / config.window_width) * config.scale_factor;

        if (chip8->display[i]) {
            if (chip8->pixel_color[i] != config.fg_color)
                chip8->pixel_color[i] = color_lerp(chip8->pixel_color[i], 
                                                    config.fg_color,
                                                    config.color_lerp_rate);

            const uint8_t r = (chip8->pixel_color[i] >> 24) & 0xFF;
            const uint8_t g = (chip8->pixel_color[i] >> 16) & 0xFF;
            const uint8_t b = (chip8->pixel_color[i] >>  8) & 0xFF;
            const uint8_t a = (chip8->pixel_color[i] >>  0) & 0xFF;

            SDL_SetRenderDrawColor(sdl.renderer, r, g, b, a);
            SDL_RenderFillRect(sdl.renderer, &rect);

            if (config.pixel_outlines) {
                SDL_SetRenderDrawColor(sdl.renderer, bg_r, bg_g, bg_b, bg_a);
                SDL_RenderDrawRect(sdl.renderer, &rect);
            }
        }
        else {
            if (chip8->pixel_color[i] != config.bg_color)
                chip8->pixel_color[i] = color_lerp(chip8->pixel_color[i],
                                                    config.bg_color,
                                                    config.color_lerp_rate);

            const uint8_t r = (chip8->pixel_color[i] >> 24) & 0xFF;
            const uint8_t g = (chip8->pixel_color[i] >> 16) & 0xFF;
            const uint8_t b = (chip8->pixel_color[i] >>  8) & 0xFF;
            const uint8_t a = (chip8->pixel_color[i] >>  0) & 0xFF;

            SDL_SetRenderDrawColor(sdl.renderer, r, g, b, a);
            SDL_RenderFillRect(sdl.renderer, &rect);
        }
    }

    SDL_RenderPresent(sdl.renderer);
}

// CHIP8 Keypad     QWERTY
// 123C             1234
// 456D             QWER
// 789E             ASDF
// A0BF             ZXCV
void handle_input(chip8_t *chip8, config_t *config)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            chip8->state = QUIT;
            break;
        
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                chip8->state = QUIT;
                puts("CHIP8 CLOSED");
                break;  
            
            case SDLK_SPACE:
                if (chip8->state == RUNNING) {
                    chip8->state = PAUSED;
                    puts("CHIP8 PAUSED");
                }
                else {
                    chip8->state = RUNNING;             
                    puts("CHIP8 RUNNING");
                }
                break;

            case SDLK_n:
                // '=' Reset CHIP8 machine for the current ROM
                init_chip8(chip8, *config, chip8->rom_name);
                break;

            case SDLK_j:
                // Decrese color lerp rate
                if (config->color_lerp_rate > 0.1)
                    config->color_lerp_rate -= 0.1;
                break;

            case SDLK_k:
                // Increase color lerp rate
                if (config->color_lerp_rate < 1.0)
                    config->color_lerp_rate += 0.1;
                break;

            case SDLK_o:
                // Decrese volume
                if (config->volume > 0)
                    config->volume -= 500;
                break;

            case SDLK_p:
                // Increase volume
                if (config->volume < INT16_MAX)
                    config->volume += 500;
                break;
            
            // Map QWERTY keys to CJIP8 Keypad
            case SDLK_1: chip8->keypad[0x1] = true; break;
            case SDLK_2: chip8->keypad[0x2] = true; break;
            case SDLK_3: chip8->keypad[0x3] = true; break;
            case SDLK_4: chip8->keypad[0xC] = true; break;

            case SDLK_q: chip8->keypad[0x4] = true; break;
            case SDLK_w: chip8->keypad[0x5] = true; break;
            case SDLK_e: chip8->keypad[0x6] = true; break;
            case SDLK_r: chip8->keypad[0xD] = true; break;
            
            case SDLK_a: chip8->keypad[0x7] = true; break;
            case SDLK_s: chip8->keypad[0x8] = true; break;
            case SDLK_d: chip8->keypad[0x9] = true; break;
            case SDLK_f: chip8->keypad[0xE] = true; break;
            
            case SDLK_z: chip8->keypad[0xA] = true; break;
            case SDLK_x: chip8->keypad[0x0] = true; break;
            case SDLK_c: chip8->keypad[0xB] = true; break;
            case SDLK_v: chip8->keypad[0xF] = true; break;

            default: break;
            }
            break;
        
        case SDL_KEYUP:
            switch (event.key.keysym.sym) {
                // Map QWERTY keys to CJIP8 Keypad
                case SDLK_1: chip8->keypad[0x1] = false; break;
                case SDLK_2: chip8->keypad[0x2] = false; break;
                case SDLK_3: chip8->keypad[0x3] = false; break;
                case SDLK_4: chip8->keypad[0xC] = false; break;

                case SDLK_q: chip8->keypad[0x4] = false; break;
                case SDLK_w: chip8->keypad[0x5] = false; break;
                case SDLK_e: chip8->keypad[0x6] = false; break;
                case SDLK_r: chip8->keypad[0xD] = false; break;
                
                case SDLK_a: chip8->keypad[0x7] = false; break;
                case SDLK_s: chip8->keypad[0x8] = false; break;
                case SDLK_d: chip8->keypad[0x9] = false; break;
                case SDLK_f: chip8->keypad[0xE] = false; break;
                
                case SDLK_z: chip8->keypad[0xA] = false; break;
                case SDLK_x: chip8->keypad[0x0] = false; break;
                case SDLK_c: chip8->keypad[0xB] = false; break;
                case SDLK_v: chip8->keypad[0xF] = false; break;

                default: break;
            }
            break;

        default:
            break;
        }
    }
}

#ifdef DEBUG
void print_debug_info(chip8_t *chip8)
{
    printf("Address: 0x%04X, Opcode: 0x%04X, Desc: ", 
            chip8->PC - 2, chip8->inst.opcode);
    switch ((chip8->inst.opcode >> 12) & 0x0F) {
    case 0x00:
        if (chip8->inst.NN == 0xE0) {
            // 00E0: Clears the screen
            printf("Clear screen\n");
        }
        else if (chip8->inst.NN == 0xEE) {
            // 00EE: Returns from subrutine
            printf("Return from subrutine to address: 0x%04X\n",
                    *(chip8->stack_ptr - 1));
        }
        else 
        {
            printf("Unimplemented instuction\n");
        }      
        break;

    case 0x01:
        // 1NNN: Jumps to address NNN
        printf("Jump to address NNN (0x%04X)\n", chip8->inst.NNN);
        break;

    case 0x02:
        // 2NNN: Calls subrutine at NNN
        printf("Call subroutine at NNN (0x%04X)\n", chip8->inst.NNN);
        break;

    case 0x03:
        // 3XNN: Skips the next instruction if VX equals NN
        printf("Check if V%X (0x%02X) == NN (0x%02X), skip next instruction if true\n",
                chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.NN);
        break;

    case 0x04:
        // 4XNN: Skips the next instruction if VX != NN
        printf("Check if V%X (0x%02X) != NN (0x%02X), skip next instruction if true\n",
                chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.NN);
        break;

    case 0x05:
        // 5XY0: Skips the next instruction if VX == VY
        printf("Check if V%X (0x%02X) == V%X (0x%02X), skip next instruction if true\n",
                chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
        break;

    case 0x06:
        // 6XNN: Sets VX to NN
        printf("Set V%X = NN (0x%02X)\n", chip8->inst.X, chip8->inst.NN);
        break;
    
    case 0x07:
        // 7XNN: Adds NN to VX (carry flag is not changed)
        printf("Set V%X (0x%02X) += NN (0x%02X), Result: 0x%02X\n", 
                chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.NN, 
                chip8->V[chip8->inst.X] + chip8->inst.NN);
        break;

    case 0x08:
        switch (chip8->inst.N) {
        case 0x0:
            // 8XY0: Sets VX to the value of VY
            printf("Set register V%X = V%X (0x%02X)\n", 
                    chip8->inst.X, chip8->inst.Y, chip8->V[chip8->inst.Y]);
            break;
        
        case 0x1:
            // 8XY1: Sets VX to VX or VY
                printf("Set register V%X (0x%02X) |= V%X (0x%02X): Result: 0x%02X\n", 
                        chip8->inst.X, chip8->V[chip8->inst.X],
                        chip8->inst.Y, chip8->V[chip8->inst.Y],
                        chip8->V[chip8->inst.X] | chip8->V[chip8->inst.Y]);
            break;
        
        case 0x2:
            // 8XY2: Sets VX to VX and VY
            printf("Set register V%X (0x%02X) &= V%X (0x%02X): Result: 0x%02X\n", 
                        chip8->inst.X, chip8->V[chip8->inst.X],
                        chip8->inst.Y, chip8->V[chip8->inst.Y],
                        chip8->V[chip8->inst.X] & chip8->V[chip8->inst.Y]);
            break;
        
        case 0x3:
            // 8XY3: Sets VX to VX xor VY
            printf("Set register V%X (0x%02X) ^= V%X (0x%02X): Result: 0x%02X\n", 
                        chip8->inst.X, chip8->V[chip8->inst.X],
                        chip8->inst.Y, chip8->V[chip8->inst.Y],
                        chip8->V[chip8->inst.X] ^ chip8->V[chip8->inst.Y]);
            break;
        
        case 0x4:
            // 8XY4: Adds VY to VX
            // VF is set to 1 when there's a carry, and to 0 when there is not 
            printf("Set register V%X (0x%02X) += V%X (0x%02X), VF = 1 if carry: Result: 0x%02X, VF = %X\n", 
                        chip8->inst.X, chip8->V[chip8->inst.X],
                        chip8->inst.Y, chip8->V[chip8->inst.Y],
                        chip8->V[chip8->inst.X] + chip8->V[chip8->inst.Y],
                        ((uint16_t)chip8->V[chip8->inst.X] + chip8->V[chip8->inst.Y]) > 255);
            break;
        
        case 0x5:
            // 8XY5: VY is subtracted from VX
            // VF is set to 0 when there's a borrow, and 1 when there is not
            printf("Set register V%X (0x%02X) -= V%X (0x%02X), VF = 1 if carry: Result: 0x%02X, VF = %X\n", 
                    chip8->inst.X, chip8->V[chip8->inst.X],
                    chip8->inst.Y, chip8->V[chip8->inst.Y],
                    chip8->V[chip8->inst.X] - chip8->V[chip8->inst.Y],
                    chip8->V[chip8->inst.Y] <= chip8->V[chip8->inst.X]);
            break;
        
        case 0x6:
            // 8XY6: Stores the most significant bit of VX in VF
            // and then shifts VX to the left by 1
            printf("Set register V%X (0x%02X) >>= 1, VF = shifted off bit (%X): Result: 0x%02X\n", 
                    chip8->inst.X, chip8->V[chip8->inst.X],
                    chip8->V[chip8->inst.X] & 1,
                    chip8->V[chip8->inst.X] >> 1);
            break;
        
        case 0x7:
            // 8XY7: Sets VX to VY minus VX. VF is set to 0 
            // when there's a borrow, and 1 when there is not.
            printf("Set register V%X = V%X (0x%02X) - V%X (0x%02X), VF = 1 if no borrow: Result: 0x%02X, VF = %X\n", 
                    chip8->inst.X, chip8->inst.Y, chip8->V[chip8->inst.Y],
                    chip8->inst.X, chip8->V[chip8->inst.X],
                    chip8->V[chip8->inst.Y] - chip8->V[chip8->inst.X],
                    chip8->V[chip8->inst.X] <= chip8->V[chip8->inst.Y]);
            break;
        
        case 0xE:
            // 8XYE: Stores the most significant bit of VX in VF 
            // and then shifts VX to the left by 1.
            printf("Set register V%X (0x%02X) <<= 1, VF = shifted off bit (%X): Result: 0x%02X\n", 
                    chip8->inst.X, chip8->V[chip8->inst.X],
                    (chip8->V[chip8->inst.X] & 0x80) >> 7,
                    chip8->V[chip8->inst.X] << 1);
            break;

        default:
            // Wrong or unimplemented opcode
            break;
        }
        break;

    case 0x09:
        // 9XY0: Skips the next instruction if VX does not equal VY
        printf("Check if V%X (0x%02X) != V%X (0x%02X), skip next instruction if true\n",
                chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y, chip8->V[chip8->inst.Y]);
        break;

    case 0x0A:
        // ANNN: Sets I to the address NNN
        printf("Set I to NNN (0x%04X)\n", chip8->inst.NNN);
        break;

    case 0x0B:
        // ANNN: Jumps to the address NNN plus V0
        printf("Set PC to V0 (0x%02X) + NNN (0x%04X): Result: PC = 0x%04X\n",
                chip8->V[0], chip8->inst.NNN, chip8->V[0] + chip8->inst.NNN);
        break;

    case 0x0C:
        // CNNN: Sets VX to the result of a bitwise and 
        // operation on a random number (Typically: 0 to 255) and NN. 
        printf("Set V%X = rand() %% 256 & NN (0x%02X)\n",
                chip8->inst.X, chip8->inst.NN);
        break;   

     case 0x0D:
        // DXYN: Draws a sprite at coordinate (VX, VY) that. 
        // Read from location I.
        // Screen pixels are XOR'd with sprite bits,
        // VF (Carry Flag) is set if any screen pixels are set off.
        printf("Draw N (%u) height sprite at coords V%X (0x%02X), V%X (0x%02X) "
                "from memory location I (0x%04X). Set VF = 1 if any pixels are turned off.\n",
                chip8->inst.N, chip8->inst.X, chip8->V[chip8->inst.X], chip8->inst.Y, 
                chip8->V[chip8->inst.Y], chip8->I);
        break;

    case 0x0E:
        switch (chip8->inst.NN) {
        case 0x9E:
            // EX9E: Skips the next instruction if the key stored in VX is pressed
            printf("Skip next instruction if key in V%X (0x%02X) is pressed: Keypad value: %d\n",
                    chip8->inst.X, chip8->V[chip8->inst.X], chip8->keypad[chip8->V[chip8->inst.X]]);
            break;
        case 0xA1:
            // EXA1: Skips the next instruction if the key stored in VX is not pressed
            printf("Skip next instruction if key in V%X (0x%02X) is not pressed: Keypad value: %d\n",
                    chip8->inst.X, chip8->V[chip8->inst.X], chip8->keypad[chip8->V[chip8->inst.X]]);
   
            break;
        
        default:
            // No opcode
            break;
        }
        break;

    case 0x0F:
        switch (chip8->inst.NN) {
        case 0x07:
            // FX07: Sets VX to the value of the delay timer
            printf("Set V%X = delay timer value (0x%02X)\n",
                    chip8->inst.X, chip8->delay_timer);
            break;
        
        case 0x0A:
            // FX0A: A key press is awaited, and then stored in VX
            printf("Await until a key is pressed, store key in V%X\n", chip8->inst.X);
            break;

        case 0x15:
            // FX15: Sets the delay timer to VX
            printf("Set delay timer value = V%X (0x%02X)\n",
                    chip8->inst.X, chip8->V[chip8->inst.X]);
            break;

        case 0x18:
            // FX18: Sets the sound timer to VX
            printf("Set sound timer value = V%X (0x%02X)\n",
                    chip8->inst.X, chip8->V[chip8->inst.X]);
            break;

        case 0x1E:
            // FX1E: Adds VX to I. VF is not affected.
            printf("I (0x%04X) += V%X (0x%02X): Result (I): 0x%04X\n",
                    chip8->I, chip8->inst.X, chip8->V[chip8->inst.X],
                    chip8->I + chip8->V[chip8->inst.X]);
            break;

        case 0x29:
            // FX29: Sets I to the location of the sprite for the character in VX.
            // Characters 0-F (in hexadecimal) are represented by a 4x5 font. 
            printf("Set I to sprite location in memory for character in V%X (0x%02X). Result (VX * 5) = (0x%02X)\n",
                    chip8->inst.X, chip8->V[chip8->inst.X], chip8->V[chip8->inst.X] * 5);
            break;

        case 0x33:
            // FX33: Stores the binary-coded decimal representation of VX,
            // with the hundreds digit in memory at location in I,
            // the tens digit at location I+1, and the ones digit at location I+2.
            printf("Store BCD representation of V%X (0x%02X) at memory from I (0x%04X)\n",
                    chip8->inst.X, chip8->V[chip8->inst.X], chip8->I); 
            break;

        case 0x55:
            // FX55: Stores from V0 to VX (including VX) in memory, starting at address I.
            // The offset from I is increased by 1 for each value written,
            // but I itself is left unmodified (SCHIP).
            printf("Register dump V0-V%X (0x%02X) inclusive at memory from I (0x%04X)\n",
                    chip8->inst.X, chip8->V[chip8->inst.X], chip8->I); 
            break;

        case 0x65:
            // FX65: Fills from V0 to VX (including VX) with values from memory, starting at address I.
            // The offset from I is increased by 1 for each value read, but I itself is left unmodified
            printf("Register load V0-V%X (0x%02X) inclusive at memory from I (0x%04X)\n",
                    chip8->inst.X, chip8->V[chip8->inst.X], chip8->I); 
            break;
        
        default:
            // No opcode
            break;
        }
        break;

    default:
        printf("Unimplemented instuction\n");
        break;
    }
}
#endif

void emulate_instruction(chip8_t *chip8, const config_t config)
{
    bool carry;
    chip8->inst.opcode = (chip8->ram[chip8->PC] << 8 | chip8->ram[chip8->PC + 1]);
    chip8->PC += 2;

    chip8->inst.NNN = chip8->inst.opcode & 0x0FFF;
    chip8->inst.NN  = chip8->inst.opcode & 0x0FF;
    chip8->inst.N   = chip8->inst.opcode & 0x0F;
    chip8->inst.X   = (chip8->inst.opcode >> 8) & 0x0F;
    chip8->inst.Y   = (chip8->inst.opcode >> 4) & 0x0F;

#ifdef DEBUG
    print_debug_info(chip8);
#endif

    switch ((chip8->inst.opcode >> 12) & 0x0F) {
    case 0x00:
        if (chip8->inst.NN == 0xE0) {
            // 0x00E0: Clears the screen
            memset(chip8->display, 0, sizeof(chip8->display));
            chip8->draw = true;
        }
        else if (chip8->inst.NN == 0xEE) {
            // 0x00EE: Returns from subrutine
            chip8->PC = *--chip8->stack_ptr;
        }
        else {
            // Unimplemented/invalid opcode, 0xNNN?
        }            
        break;
    
    case 0x01:
        // 1NNN: Jumps to address NNN
        chip8->PC = chip8->inst.NNN;
        break;

    case 0x02:
        // 0x2NNN: Calls subrutine at NNN
        *chip8->stack_ptr++ = chip8->PC;
        chip8->PC = chip8->inst.NNN;
        break;
    
    case 0x03:
        // 3XNN: Skips the next instruction if VX == NN
        if (chip8->V[chip8->inst.X] == chip8->inst.NN)
            chip8->PC += 2;
        break;

    case 0x04:
        // 4XNN: Skips the next instruction if VX != NN
        if (chip8->V[chip8->inst.X] != chip8->inst.NN)
            chip8->PC += 2;
        break;

    case 0x05:
        // 5XY0: Skips the next instruction if VX == VY
        if (chip8->inst.N != 0)
            break;
        if (chip8->V[chip8->inst.X] == chip8->V[chip8->inst.Y])
            chip8->PC += 2;
        break;

    case 0x06:
        // 6XNN: Sets VX to NN
        chip8->V[chip8->inst.X] = chip8->inst.NN;
        break;

    case 0x07:
        // 7XNN: Adds NN to VX (carry flag is not changed)
        chip8->V[chip8->inst.X] += chip8->inst.NN;
        break;

    case 0x08:
        switch (chip8->inst.N) {
        case 0x0:
            // 8XY0: Sets VX to the value of VY
            chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y];
            break;
        
        case 0x1:
            // 8XY1: Sets VX to VX or VY
            chip8->V[chip8->inst.X] |= chip8->V[chip8->inst.Y];
            if (config.current_extension == CHIP8)
                chip8->V[0xF] = 0;
            break;
        
        case 0x2:
            // 8XY2: Sets VX to VX and VY
            chip8->V[chip8->inst.X] &= chip8->V[chip8->inst.Y];
            if (config.current_extension == CHIP8)
                chip8->V[0xF] = 0;
            break;
        
        case 0x3:
            // 8XY3: Sets VX to VX xor VY
            chip8->V[chip8->inst.X] ^= chip8->V[chip8->inst.Y];
            if (config.current_extension == CHIP8)
                chip8->V[0xF] = 0;
            break;
        
        case 0x4:
            // 8XY4: Adds VY to VX
            // VF is set to 1 when there's a carry, and to 0 when there is not 
            carry = ((uint16_t)(chip8->V[chip8->inst.X] + chip8->V[chip8->inst.Y]) > 255);

            chip8->V[chip8->inst.X] += chip8->V[chip8->inst.Y];
            chip8->V[0xF] = carry;
            break;
        
        case 0x5:
            // 8XY5: VY is subtracted from VX
            // VF is set to 0 when there's a borrow, and 1 when there is not
            carry = (chip8->V[chip8->inst.Y] <= chip8->V[chip8->inst.X]);

            chip8->V[chip8->inst.X] -= chip8->V[chip8->inst.Y];
            chip8->V[0xF] = carry;
            break;
        
        case 0x6:
            // 8XY6: Stores the most significant bit of VX in VF
            // and then shifts VX to the left by 1
            if (config.current_extension == CHIP8) {
                carry = (chip8->V[chip8->inst.Y] & 1);
                chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y] >> 1;
            } else {
                carry = (chip8->V[chip8->inst.X] & 1);
                chip8->V[chip8->inst.X] >>= 1;
            }
            chip8->V[0xF] = carry;
            break;
        
        case 0x7:
            // 8XY7: Sets VX to VY minus VX. VF is set to 0 
            // when there's a borrow, and 1 when there is not.
            carry = (chip8->V[chip8->inst.X] <= chip8->V[chip8->inst.Y]);
            
            chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y] - chip8->V[chip8->inst.X];
            chip8->V[0xF] = carry;
            break;
        
        case 0xE:
            // 8XYE: Stores the most significant bit of VX in VF 
            // and then shifts VX to the left by 1.
            if (config.current_extension == CHIP8) {
                carry = (chip8->V[chip8->inst.Y] & 0x80) >> 7;
                chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y] << 1;
            } else {
                carry = (chip8->V[chip8->inst.X] & 0x80) >> 7;
                chip8->V[chip8->inst.X] <<= 1;
            }
            chip8->V[0xF] = carry;
            break;

        default:
            // Wrong or unimplemented opcode
            break;
        }
        break;

    case 0x09:
        // 9XY0: Skips the next instruction if VX does not equal VY
        if (chip8->V[chip8->inst.X] != chip8->V[chip8->inst.Y])
            chip8->PC += 2;
        break;

    case 0x0A:
        // ANNN: Sets I to the address NNN
        chip8->I = chip8->inst.NNN;
        break;

    case 0x0B:
        // BNNN: Jumps to the address NNN plus V0
        chip8->PC = chip8->V[0] + chip8->inst.NNN;
        break;

    case 0x0C:
        // CNNN: Sets VX to the result of a bitwise and 
        // operation on a random number (Typically: 0 to 255) and NN. 
        chip8->V[chip8->inst.X] = (rand() % 256) & chip8->inst.NN;
        break;   
    
    case 0x0D:
        // DXYN: Draws a sprite at coordinate (VX, VY) that. 
        // Read from location I.
        // Screen pixels are XOR'd with sprite bits,
        // VF (Carry Flag) is set if any screen pixels are set off.
        uint8_t x_coord = chip8->V[chip8->inst.X] % config.window_width;
        uint8_t y_coord = chip8->V[chip8->inst.Y] % config.window_height;
        const uint8_t orig_x = x_coord;
        
        chip8->V[0xF] = 0;

        // Loop over all N rows of the sprite
        uint8_t i;
        int8_t j;
        for (i = 0; i < chip8->inst.N; ++i) {
            // Get index row/byte of sprite data
            const uint8_t sprite_data = chip8->ram[chip8->I + i];
            x_coord = orig_x; // Reset X for next row to draw

            for (j = 7; j >= 0; --j) {
                // If sprite pixel/bit is on and display pixel is on, set carry flag
                bool *pixel = &chip8->display[y_coord * config.window_width + x_coord];
                const bool sprite_bit = (sprite_data & (1 << j));
                
                if (sprite_bit && *pixel) {
                    chip8->V[0xF] = 1;
                }

                // XOR display pixel with sprite pixel/bit
                *pixel ^= sprite_bit;

                // Stop drawing if hit right edge of screen
                if (++x_coord >= config.window_width)
                    break;
            }
            // Stop drawing entire sprite if hit bottom page of screen
            if (++y_coord >= config.window_height)
                break;
        }
        chip8->draw = true;
        break;

    case 0x0E:
        switch (chip8->inst.NN) {
        case 0x9E:
            // EX9E: Skips the next instruction if the key stored in VX is pressed
            if (chip8->keypad[chip8->V[chip8->inst.X]])
                chip8->PC += 2;
            break;
        case 0xA1:
            // EXA1: Skips the next instruction if the key stored in VX is not pressed
            if (!chip8->keypad[chip8->V[chip8->inst.X]])
                chip8->PC += 2;
            break;
        
        default:
            // No opcode
            break;
        }
        break;

    case 0x0F:
        switch (chip8->inst.NN) {
        case 0x07:
            // FX07: Sets VX to the value of the delay timer
            chip8->V[chip8->inst.X] = chip8->delay_timer;
            break;

        case 0x0A:
            // FX0A: A key press is awaited, and then stored in VX
            static bool any_key_pressed = false;
            static uint8_t key = 0xFF;
            uint8_t i;
            for (i = 0; (key == 0xFF) && (i < sizeof(chip8->keypad)); ++i) 
                if (chip8->keypad[i]) {
                    key = i;
                    any_key_pressed = true;
                    break;
                }

            // Run the same opcode if no key has been pressed yet
            if (!any_key_pressed) {
                chip8->PC -= 2;
            } else {
                if (chip8->keypad[key]) {
                    chip8->PC -= 2;
                }
                else {
                    chip8->V[chip8->inst.X] = key;
                    key = 0xFF;
                    any_key_pressed = false;
                }
            } 
            break;

        case 0x15:
            // FX15: Sets the delay timer to VX
            chip8->delay_timer = chip8->V[chip8->inst.X];
            break;

        case 0x18:
            // FX18: Sets the sound timer to VX
            chip8->sound_timer = chip8->V[chip8->inst.X];
            break;

        case 0x1E:
            // FX1E: Adds VX to I. VF is not affected.
            chip8->I += chip8->V[chip8->inst.X];
            break;

        case 0x29:
            // FX29: Sets I to the location of the sprite for the character in VX.
            // Characters 0-F (in hexadecimal) are represented by a 4x5 font. 
            chip8->I = chip8->V[chip8->inst.X] * 5;
            break;

        case 0x33:
            // FX33: Stores the binary-coded decimal representation of VX,
            // with the hundreds digit in memory at location in I,
            // the tens digit at location I+1, and the ones digit at location I+2. 
            uint8_t bcd = chip8->V[chip8->inst.X];
            chip8->ram[chip8->I + 2] = bcd % 10;
            bcd /= 10;
            chip8->ram[chip8->I + 1] = bcd % 10;
            bcd /= 10;
            chip8->ram[chip8->I + 0] = bcd;
            break;

        case 0x55:
            // FX55: Stores from V0 to VX (including VX) in memory, starting at address I.
            // The offset from I is increased by 1 for each value written, but I itself is left unmodified.
            // CHIP8 does increment I, SCHIP does not increment I.
            for (i = 0; i <= chip8->inst.X; ++i)                
                if (config.current_extension == CHIP8)
                    chip8->ram[chip8->I++] = chip8->V[i];
                else 
                    chip8->ram[chip8->I + i] = chip8->V[i];
                
            break;

        case 0x65:
            // FX65: Fills from V0 to VX (including VX) with values from memory, starting at address I.
            // The offset from I is increased by 1 for each value read, but I itself is left unmodified.
            // CHIP8 does increment I, SCHIP does not increment I.
            for (i = 0; i <= chip8->inst.X; ++i)
                if (config.current_extension == CHIP8)
                    chip8->V[i] = chip8->ram[chip8->I++];
                else
                    chip8->V[i] = chip8->ram[chip8->I + i];
                
            break;
        
        default:
            break;
        }
        break;

    default:
        break; // Unimplemented instuction
    }
}

void update_timers(const sdl_t sdl, chip8_t *chip8)
{
    if (chip8->delay_timer > 0)
        chip8->delay_timer--;
    if (chip8->sound_timer > 0) {
        chip8->sound_timer--;
        SDL_PauseAudioDevice(sdl.dev, 0);
    } else {
        SDL_PauseAudioDevice(sdl.dev, 1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom_name>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    //Initialize emulator config
    config_t config = {0};
    if (!set_config_from_args(&config, argc, argv))
        exit(EXIT_FAILURE);

    // Initialize SDL
    sdl_t sdl = {0};
    if (!init_sdl(&sdl, &config))
        exit(EXIT_FAILURE);

    chip8_t chip8 = {0};
    const char *rom_name = argv[1];
    if (!init_chip8(&chip8, config, rom_name))
        exit(EXIT_FAILURE);

    // Initial screen clear
    clear_screen(sdl, config);

    srand(time(NULL));
    
    // Main loop
    while (chip8.state != QUIT) {
        handle_input(&chip8, &config);

        if (chip8.state == PAUSED)
            continue;

        const uint64_t start_frame_time = SDL_GetPerformanceCounter();

        uint32_t i;
        for (i = 0; i < config.insts_per_sec / 60; ++i)
            emulate_instruction(&chip8, config);

        const uint64_t end_frame_time = SDL_GetPerformanceCounter();
        
        const double time_elapsed = (double)((end_frame_time - start_frame_time) * 1000) / SDL_GetPerformanceFrequency();

        SDL_Delay(16.67f > time_elapsed ? 16.67f - time_elapsed : 0);

        if (chip8.draw) {
            update_screen(sdl, config, &chip8);
            chip8.draw = false;
        }

        update_timers(sdl, &chip8);
    }

    // Final cleanup
    final_cleanup(sdl);

    exit(EXIT_SUCCESS);
}