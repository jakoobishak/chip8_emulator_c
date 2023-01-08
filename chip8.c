#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "SDL.h"

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
} sdl_t;

typedef struct {
    char    *window_title;
    uint32_t window_width;
    uint32_t window_height;
    uint32_t fg_color;
    uint32_t bg_color;
    uint32_t scale_factor;
    bool     pixel_outlines;
} config_t;

typedef enum {
    QUIT = 0,
    RUNNING,
    PAUSED,
} emulator_state_t;

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
} chip8_t;

bool init_sdl(sdl_t *sdl, const config_t config)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        SDL_Log("Could not initialize SDL %s\n", SDL_GetError());
        return false;
    }

    sdl->window = SDL_CreateWindow(config.window_title, 
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    config.window_width * config.scale_factor,
                                    config.window_height * config.scale_factor,
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

    return true;
}

bool set_config_from_args(config_t *config, const int argc, char **argv)
{
    *config = (config_t) {
        .window_title   =   "CHIP8",
        .window_width   =   64,
        .window_height  =   32,
        .fg_color       =   0xFFFFFFFF,
        .bg_color       =   0x000000FF,
        .scale_factor   =   20,
        .pixel_outlines =   true,
    };

    int8_t i;
    for (i = 1; i < argc; ++i)
        (void)argv[i];

    return true;
}

bool init_chip8(chip8_t *chip8, const char rom_name[])
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
                rom_name, rom_size, max_size);
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

    return true;
}

void final_cleanup(const sdl_t sdl)
{
    SDL_DestroyRenderer(sdl.renderer);
    SDL_DestroyWindow(sdl.window);
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

void update_screen(const sdl_t sdl, const config_t config, const chip8_t chip8)
{
    SDL_Rect rect = {.x = 0, .y = 0, .w = config.scale_factor, .h = config.scale_factor};
    
    const uint8_t fg_r = (config.fg_color >> 24) & 0xFF;
    const uint8_t fg_g = (config.fg_color >> 16) & 0xFF;
    const uint8_t fg_b = (config.fg_color >>  8) & 0xFF;
    const uint8_t fg_a = (config.fg_color >>  0) & 0xFF;

    const uint8_t bg_r = (config.bg_color >> 24) & 0xFF;
    const uint8_t bg_g = (config.bg_color >> 16) & 0xFF;
    const uint8_t bg_b = (config.bg_color >>  8) & 0xFF;
    const uint8_t bg_a = (config.bg_color >>  0) & 0xFF;

    uint32_t i;
    for(i = 0; i < sizeof(chip8.display); ++i) {
        rect.x = (i % config.window_width) * config.scale_factor;
        rect.y = (i / config.window_width) * config.scale_factor;

        if (chip8.display[i]) {
            SDL_SetRenderDrawColor(sdl.renderer, fg_r, fg_g, fg_b, fg_a);
            SDL_RenderFillRect(sdl.renderer, &rect);

            if (config.pixel_outlines) {
                SDL_SetRenderDrawColor(sdl.renderer, bg_r, bg_g, bg_b, bg_a);
                SDL_RenderDrawRect(sdl.renderer, &rect);
            }
        }
        else {
            SDL_SetRenderDrawColor(sdl.renderer, bg_r, bg_g, bg_b, bg_a);
            SDL_RenderFillRect(sdl.renderer, &rect);
        }
    }

    SDL_RenderPresent(sdl.renderer);
}

void handle_input(chip8_t *chip8)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            chip8->state = QUIT;
            return;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                chip8->state = QUIT;
                puts("CHIP8 CLOSED");
                return;  
            case SDLK_SPACE:
                if (chip8->state == RUNNING) {
                    chip8->state = PAUSED;
                    puts("CHIP8 PAUSED");
                }
                else {
                    chip8->state = RUNNING;             
                    puts("CHIP8 RUNNING");
                }
                return;
            default:
                break;
            }
            break;
        case SDL_KEYUP:
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
            // 0x00E0: Clears the screen
            printf("Clear screen\n");
        }
        else if (chip8->inst.NN == 0xEE) {
            // 0x00EE: Returns from subrutine
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
        // 0x2NNN: Calls subrutine at NNN
        *chip8->stack_ptr++ = chip8->PC;
        chip8->PC = chip8->inst.NNN;
        break;

    case 0x0A:
        // ANNN: Sets I to the address NNN
        printf("Set I to NNN (0x%04X)\n", chip8->inst.NNN);
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

    default:
        printf("Unimplemented instuction\n");
        break;
    }
}
#endif

void emulate_instruction(chip8_t *chip8, const config_t config)
{
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
        }
        else if (chip8->inst.NN == 0xEE) {
            // 0x00EE: Returns from subrutine
            chip8->PC = *--chip8->stack_ptr;
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

    case 0x0A:
        //ANNN: Sets I to the address NNN
        chip8->I = chip8->inst.NNN;
        break;

    case 0x06:
        // 6XNN: Sets VX to NN
        chip8->V[chip8->inst.X] = chip8->inst.NN;
        break;

    case 0x07:
        // 7XNN: Adds NN to VX (carry flag is not changed)
        chip8->V[chip8->inst.X] += chip8->inst.NN;
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

        break;

    default:
        break; // Unimplemented instuction
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
    if (!init_sdl(&sdl, config))
        exit(EXIT_FAILURE);

    chip8_t chip8 = {0};
    const char *rom_name = argv[1];
    if (!init_chip8(&chip8, rom_name))
        exit(EXIT_FAILURE);

    // Initial screen clear
    clear_screen(sdl, config);
    
    // Main loop
    while(chip8.state != QUIT) {
        handle_input(&chip8);

        if (chip8.state == PAUSED)
            continue;

        emulate_instruction(&chip8, config);

        SDL_Delay(16);

        update_screen(sdl, config, chip8);
    }

    // Final cleanup
    final_cleanup(sdl);

    exit(EXIT_SUCCESS);
}