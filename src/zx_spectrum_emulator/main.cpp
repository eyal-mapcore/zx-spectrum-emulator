#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <string>
#include <utility>

#include "z80/z80_cpu.h"
#include "z80/keyb_lookup.h"
#include "z80/tape_trap.h"
#include "z80/beeper.h"

using namespace z80;

/// Shared emulated keyboard matrix state
/// between Main Thread and Emulator Thread (Initialized to 0x1F = unpressed)
std::atomic<uint8_t> key_matrix[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };

/// Tape controls, set by the main thread and serviced by the CPU thread.
///
/// The TapeDeck itself is only ever touched from the CPU thread -- the traps
/// run there -- so these flags are the whole synchronisation story. Giving the
/// deck a mutex instead would work, but this keeps the deck single-threaded
/// and the ownership obvious.
std::atomic<bool> tape_rewind_requested{ false };
std::atomic<bool> tape_reload_requested{ false };
std::atomic<bool> tape_new_requested{ false };

/// Samples in one emulated frame: 44100Hz / 50fps divides exactly.
constexpr int BEEPER_SAMPLES_PER_FRAME = BEEPER_DEFAULT_SAMPLE_RATE / FRMAES_PER_SECOND;

/// Let at most this much audio back up before dropping a frame of it.
constexpr Uint32 kMaxQueuedAudioBytes = 4 * BEEPER_SAMPLES_PER_FRAME * sizeof(Sint16);

/// Silences the speaker without stopping it: the beeper keeps rendering so
/// the audio queue stays the same depth, the samples are just zeroed.
std::atomic<bool> audio_muted{ false };

/// ZX Spectrum colour palette, indexed as kPalette[bright][colour].
/// The colour bits are ordered GRB: bit0 = blue, bit1 = red, bit2 = green.
/// Exact RGB levels differ a little between references/emulators; these are
/// the commonly used 0xD7 (normal) and 0xFF (bright) levels.
struct Colour { Uint8 r, g, b; };
static const Colour kPalette[2][8] = {
    { {0x00,0x00,0x00}, {0x00,0x00,0xD7}, {0xD7,0x00,0x00}, {0xD7,0x00,0xD7},
      {0x00,0xD7,0x00}, {0x00,0xD7,0xD7}, {0xD7,0xD7,0x00}, {0xD7,0xD7,0xD7} },
    { {0x00,0x00,0x00}, {0x00,0x00,0xFF}, {0xFF,0x00,0x00}, {0xFF,0x00,0xFF},
      {0x00,0xFF,0x00}, {0x00,0xFF,0xFF}, {0xFF,0xFF,0x00}, {0xFF,0xFF,0xFF} },
};

/// Border colour (0-7), written by the CPU thread via OUT to the ULA port
/// and read by the main thread when rendering. BRIGHT does not apply to
/// the border on real hardware, so only the 3 colour bits are kept.
std::atomic<uint8_t> border_colour{ 7 };

/// The 256x192 display is drawn at 2x scale and centred in the window; the
/// remaining margin is the border.
constexpr int PIXEL_SCALE = 2;
constexpr int SCREEN_PIXELS_W = 256;
constexpr int SCREEN_PIXELS_H = 192;
constexpr int SCREEN_WIDTH = SCREEN_PIXELS_W * PIXEL_SCALE;
constexpr int SCREEN_HEIGHT = SCREEN_PIXELS_H * PIXEL_SCALE;
constexpr int WINDOW_WIDTH = 640;
constexpr int WINDOW_HEIGHT = 480;
constexpr int SCREEN_OFFSET_X = (WINDOW_WIDTH - SCREEN_WIDTH) / 2;
constexpr int SCREEN_OFFSET_Y = (WINDOW_HEIGHT - SCREEN_HEIGHT) / 2;

/// kPalette packed as ARGB8888, so the inner pixel loop is a single lookup
/// rather than three shifts. ARGB8888 is a packed format defined by the
/// Uint32 value, so this is endianness-independent.
Uint32 palette_argb[2][8];

void build_palette() {
    for (int bright = 0; bright < 2; bright++) {
        for (int colour = 0; colour < 8; colour++) {
            const Colour& c = kPalette[bright][colour];
            palette_argb[bright][colour] = 0xFF000000u
                                         | (static_cast<Uint32>(c.r) << 16)
                                         | (static_cast<Uint32>(c.g) << 8)
                                         |  static_cast<Uint32>(c.b);
        }
    }
}

/// @brief              Renders the screen memory into the display texture and
///                     copies it to the renderer, scaled and centred.
/// @param renderer     Renderer to present through
/// @param screen       Streaming 256x192 ARGB8888 texture, created once by the
///                     caller -- creating it per frame is what previously made
///                     rendering overrun the frame interval
/// @param memory       Emulated memory holding the display and attribute files
void render_screen_memory(SDL_Renderer* renderer, SDL_Texture* screen, Memory* memory) {
    // FLASH swaps INK and PAPER every 16 frames (~1.56Hz at 50Hz). This is
    // called once per frame, so a frame counter here is good enough.
    static int frame_counter = 0;
    const bool flash_phase = ((frame_counter++ / 16) & 1) != 0;

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(screen, nullptr, &pixels, &pitch) != 0) {
        std::fprintf(stderr, "SDL_LockTexture failed: %s\n", SDL_GetError());
        return;
    }

    // The driver may pad rows, so pitch is in *bytes* and can exceed
    // SCREEN_PIXELS_W * 4. Step rows in bytes and only then reinterpret.
    Uint8* rows = static_cast<Uint8*>(pixels);

    for (int y = 0; y < 24; y++)                            // 24 character rows
    {
        for (int x = 0; x < 32; x++)                        // 32 characters per row
        {
            // The attribute file IS linear: one byte per 8x8 cell.
            byte attr = memory->read(static_cast<word>(ATTRIBUTE_MEMORY_START + y * 32 + x));
            int ink    = attr & 0x07;
            int paper  = (attr >> 3) & 0x07;
            int bright = (attr >> 6) & 0x01;
            if ((attr & 0x80) && flash_phase) {
                std::swap(ink, paper);
            }

            const Uint32 ink_argb   = palette_argb[bright][ink];
            const Uint32 paper_argb = palette_argb[bright][paper];

            for (int row = 0; row < 8; row++)               // Each character is 8 pixels tall
            {
                // The display file is not laid out linearly: the address bits
                // are  0 1 0 T T   S S S   L L L   C C C C C  where
                //   TT    = third of the screen  (y / 8)
                //   SSS   = pixel row within the character  (row)
                //   LLL   = character row within that third (y % 8)
                //   CCCCC = column (x)
                word addr = static_cast<word>(SCREEN_MEMORY_START
                                              | ((y / 8) << 11)
                                              | (row     << 8)
                                              | ((y % 8) << 5)
                                              | x);

                byte pixel_row = memory->read(addr);

                // Writing into a buffer, so every pixel gets a colour -- there
                // is no cost to writing PAPER, unlike issuing a draw call for it.
                Uint32* dst = reinterpret_cast<Uint32*>(rows + (y * 8 + row) * pitch)
                            + x * 8;
                for (int col = 0; col < 8; col++) {
                    dst[col] = (pixel_row & (0x80 >> col)) ? ink_argb : paper_argb;
                }
            }
        }
    }

    SDL_UnlockTexture(screen);

    // One scaled blit for the whole display, instead of a fill per pixel.
    SDL_Rect dst_rect{ SCREEN_OFFSET_X, SCREEN_OFFSET_Y, SCREEN_WIDTH, SCREEN_HEIGHT };
    SDL_RenderCopy(renderer, screen, nullptr, &dst_rect);
}


/// @brief Renders all characters in the ZX Spectrum character set
/// @param renderer - SDL_Renderer to render the characters
/// @param memory  - Memory object containing the character set data
void render_all_characters(SDL_Renderer* renderer, Memory* memory) {
    // Create a texture for the character set (8x8 pixels per character)
    SDL_Texture* char_texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 8, 8);
    int count = 0;
    for (int y = 0; y < 3; y++) // 3 Lines of characters (32-127)
    {
        for (int x = 0; x < 32; x++) // 32 characters per line
        {
            // Calculate the character memory offset based on the position
            int char_offset = y * 256 + x * 8;

            // Prevent memory overfow
            if (char_offset >= ROM_SIZE) {
                // Prevent reading beyond the character set memory
                break;
            }

            // Render the character's 8x8 font bitmap (currently black)
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);     // Set draw color to black
            for (int row = 0; row < 8; row++) {                 // Each character is 8 pixels tall

                // Find character's row location in memory
                byte pixel_row = memory->read(0x3D00 + char_offset + row);  

                for (int col = 0; col < 8; col++) {
                    if (pixel_row & (0x80 >> col)) {
                        // Render the pixel and inflate it to make it more visible (2x2 pixels)
                        SDL_Rect pixel_rect{ x * 20 + col * 2, y * 20 + row * 2, 2, 2 };
                        SDL_RenderFillRect(renderer, &pixel_rect);
                    }
                }
            }
        }
    }
    SDL_DestroyTexture(char_texture);

}


/// Writes 16-bit mono PCM as a RIFF/WAVE file. Exists so that sound can be
/// checked by measuring it rather than by listening -- the beeper's whole
/// correctness criterion is a frequency.
class WavWriter {
public:
    ~WavWriter() { close(); }

    bool open(const std::string& path, int sample_rate) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) return false;
        rate_ = sample_rate;
        frames_ = 0;
        write_header();          // rewritten with real sizes on close()
        return true;
    }

    void write(const std::vector<Sint16>& samples) {
        if (!file_ || samples.empty()) return;
        std::fwrite(samples.data(), sizeof(Sint16), samples.size(), file_);
        frames_ += samples.size();
    }

    void close() {
        if (!file_) return;
        std::fseek(file_, 0, SEEK_SET);
        write_header();
        std::fclose(file_);
        file_ = nullptr;
    }

    bool is_open() const { return file_ != nullptr; }
    size_t frames() const { return frames_; }

private:
    void put32(Uint32 v) { std::fwrite(&v, 4, 1, file_); }
    void put16(Uint16 v) { std::fwrite(&v, 2, 1, file_); }

    void write_header() {
        const Uint32 data_bytes = static_cast<Uint32>(frames_ * 2);
        std::fwrite("RIFF", 1, 4, file_);  put32(36 + data_bytes);
        std::fwrite("WAVE", 1, 4, file_);
        std::fwrite("fmt ", 1, 4, file_);  put32(16);
        put16(1);                          // PCM
        put16(1);                          // mono
        put32(static_cast<Uint32>(rate_));
        put32(static_cast<Uint32>(rate_ * 2));   // byte rate
        put16(2);                          // block align
        put16(16);                         // bits per sample
        std::fwrite("data", 1, 4, file_);  put32(data_bytes);
    }

    std::FILE* file_ = nullptr;
    int rate_ = 0;
    size_t frames_ = 0;
};

/// @brief Called upon each timer tick to push a render event to the SDL event queue
/// @param interval interval in milliseconds for the timer
/// @param param    user data passed to the callback (in this case, the event type)
/// @return         Interval for the next timer tick (in milliseconds)
Uint32 render_timer_callback(Uint32 interval, void* param) {
    SDL_Event event{};
    event.type = *static_cast<Uint32*>(param);
    SDL_PushEvent(&event);
    return interval;
}

/// @brief Handles keyboard input events and updates the emulated keyboard matrix state
/// @param row          matrix row (0-7) corresponding to the ZX Spectrum keyboard layout
/// @param bit          matrix bit (0-4) corresponding to the ZX Spectrum keyboard layout
/// @param is_pressed   true if the key is pressed, false if released
void update_key(int row, int bit, bool is_pressed) {
    if (is_pressed) {
        // Clear bit (Active Low)
        key_matrix[row].fetch_and(~(1 << bit), std::memory_order_relaxed);
    } else {
        // Set bit (Unpressed)
        key_matrix[row].fetch_or(1 << bit, std::memory_order_relaxed);
    }
}

/// @brief Handles keyboard input events and updates the emulated keyboard matrix state
/// @param event 
void handle_key_event(const SDL_Event& event) {
    bool is_pressed = (event.type == SDL_KEYDOWN);
    SDL_Scancode scancode = event.key.keysym.scancode;

    // Iterate through the keymap to find the corresponding row and bit
    for (const auto& mapping : keymap) {
        if (mapping.scancode == scancode) {
            update_key(mapping.row, mapping.bit, is_pressed);
            break;
        }
    }
}

int main(int argc, char** argv) {
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Audio is initialised on its own because, unlike video, it is optional:
    // a machine with no sound server should still run the emulator, silently.
    // Folding it into SDL_Init above would make a missing audio driver fatal.
    const bool audio_available = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;

    // Create an instance of the CPU and reset it
    z80::Cpu cpu;
    cpu.reset();

    // Create an SDL window
    SDL_Window* window = SDL_CreateWindow(
        "ZX Spectrum Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_SHOWN);

    // Check if the window was created successfully
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Raise the window to the front
    SDL_RaiseWindow(window);

    // Initialize state variables for the CPU thread and rendering
    bool running = true;
    std::atomic<bool> rom_loaded{false};

    // Create an SDL renderer for the window
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED);

    // Check if the renderer was created successfully
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The display texture, at the Spectrum's native 256x192. Created ONCE here
    // and reused every frame; SDL_RenderCopy does the 2x scaling for free.
    SDL_Texture* screen_texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        SCREEN_PIXELS_W, SCREEN_PIXELS_H);

    if (!screen_texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Nearest-neighbour, so scaled pixels stay square instead of blurring.
    SDL_SetTextureScaleMode(screen_texture, SDL_ScaleModeNearest);

    build_palette();

    // The virtual tape. Inserting fails harmlessly when the file is not there
    // yet -- the deck still knows the filename, so SAVE will create it.
    // ---- audio ------------------------------------------------------------
    // Optional capture, so the sound can be checked by measuring a frequency
    // rather than by listening to it.
    WavWriter wav;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--wav") == 0) {
            if (wav.open(argv[i + 1], BEEPER_DEFAULT_SAMPLE_RATE)) {
                std::printf("Capturing audio to '%s'.\n", argv[i + 1]);
            } else {
                std::fprintf(stderr, "Cannot write '%s'\n", argv[i + 1]);
            }
        }
    }

    SDL_AudioSpec want{}, have{};
    want.freq     = BEEPER_DEFAULT_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = nullptr;      // pushed with SDL_QueueAudio, not pulled

    // allowed_changes 0: take exactly this format or nothing, so the Beeper's
    // sample rate and SDL's always agree.
    SDL_AudioDeviceID audio_device = audio_available
        ? SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0)
        : 0;
    if (audio_device == 0) {
        std::printf("Audio unavailable (%s); running silent.\n", SDL_GetError());
    } else {
        // Prime the queue so the first real frame does not underrun.
        const std::vector<Sint16> silence(2 * BEEPER_SAMPLES_PER_FRAME, 0);
        SDL_QueueAudio(audio_device, silence.data(),
                       static_cast<Uint32>(silence.size() * sizeof(Sint16)));
        SDL_PauseAudioDevice(audio_device, 0);
        std::printf("Audio: %d Hz mono.  F8 mute.\n", have.freq);
    }

    Beeper beeper;
    std::vector<Sint16> frame_samples;   // CPU thread only; reused every frame

    TapeDeck tape_deck(TAPE_FILE_NAME);
    if (tape_deck.insert()) {
        std::printf("Tape '%s': %zu block(s).  F5 rewind, F6 reload, F7 new tape.\n",
                    TAPE_FILE_NAME, tape_deck.block_count());
    } else {
        std::printf("Tape '%s': not loaded (%s). SAVE will create it.\n",
                    TAPE_FILE_NAME, tape_deck.last_error().c_str());
    }

    // Intercept the ROM's LD-BYTES/SA-BYTES. Installed before the CPU thread
    // starts, because set_trap() is not safe against a running CPU.
    TapeTrap tape_trap(cpu, tape_deck);
    tape_trap.install();

    // Start a separate thread for the CPU execution
    std::thread cpu_thread([&cpu, &running, &rom_loaded, &renderer, &tape_deck,
                            &beeper, &frame_samples, &wav, audio_device]() {
        std::string rom_path = ROM_FILE_NAME;
        rom_loaded = cpu.get_memory()->load_rom(rom_path, ROM_START_ADDRESS, ROM_SIZE);
        if (!rom_loaded) {
            // Without this the window just shows an empty border, which gives
            // no hint of what went wrong.
            std::fprintf(stderr,
                         "ROM not found at '%s'.\n"
                         "Install the spectrum-roms package, or set ROM_FILE_NAME in\n"
                         "include/z80/globals.h to where your 48K ROM image is.\n",
                         rom_path.c_str());
        }
        cpu.reset();
        // The ULA answers any port with bit 0 clear (conventionally 0xFE), not
        // just port 0xFE exactly. One write carries three things: bits 0-2 are
        // the border colour, bit 3 is MIC (the tape output, which never moves
        // here because SA-BYTES is trapped) and bit 4 is the speaker.
        cpu.set_port_out_callback([&cpu, &beeper](word port, byte value) {
            if ((port & 0x01) != 0) return;
            border_colour.store(value & 0x07, std::memory_order_relaxed);

            // The speaker riding on the same write as the border is why
            // changing the border clicks on real hardware. Timestamped with
            // the free-running T-state clock, because the pitch is decided
            // entirely by *when* it toggles.
            beeper.set_level(cpu.total_cycles(), (value & 0x10) != 0);
        });

        // Keyboard read. The high byte of the port selects which half-rows
        // to scan: a row is active when its bit is LOW, and several can be
        // selected at once, in which case their results are ANDed together
        // (which is how the ROM detects shifted keys).
        cpu.set_port_in_callback([](word port) -> byte {
            if ((port & 0x01) != 0) {
                return 0xFF; // nothing else is on the bus yet
            }

            byte row_select = static_cast<byte>(port >> 8);
            byte keys = 0x1F; // bits 0-4: 1 = released (active low)
            for (int row = 0; row < 8; row++) {
                if ((row_select & (1 << row)) == 0) {
                    keys &= key_matrix[row].load(std::memory_order_relaxed);
                }
            }

            // Bits 5-7 aren't keyboard bits: bit 6 is the EAR (tape) input,
            // 5 and 7 are unused. With no tape attached they read high.
            return static_cast<byte>(keys | 0xE0);
        });

        cpu.set_frame_callback([&renderer, &tape_deck, &beeper,
                                &frame_samples, &wav, audio_device](Cpu* cpu) {
            // Tape controls are serviced here rather than in the step loop:
            // this runs on the CPU thread, so the deck stays single-threaded,
            // and once per frame is plenty for a button press.
            if (tape_rewind_requested.exchange(false, std::memory_order_relaxed)) {
                tape_deck.rewind();
                std::printf("Tape rewound (%zu block(s)).\n", tape_deck.block_count());
            }
            if (tape_reload_requested.exchange(false, std::memory_order_relaxed)) {
                if (tape_deck.insert()) {
                    std::printf("Tape reloaded: %zu block(s).\n", tape_deck.block_count());
                } else {
                    std::printf("Tape reload failed: %s\n", tape_deck.last_error().c_str());
                }
            }
            if (tape_new_requested.exchange(false, std::memory_order_relaxed)) {
                // In memory only: '' on disk survives until the next SAVE,
                // so F6 undoes an accidental F7.
                tape_deck.new_tape();
                std::printf("New blank tape. '%s' on disk is untouched until the "
                            "next SAVE; F6 reloads it.\n", TAPE_FILE_NAME);
            }

            // Audio. Rendered here rather than on SDL's audio thread so the
            // Beeper stays owned by the CPU thread, exactly like the TapeDeck:
            // no locking, and SDL's own queue absorbs the jitter.
            frame_samples.clear();
            beeper.render_to(cpu->total_cycles(), frame_samples);
            if (audio_muted.load(std::memory_order_relaxed)) {
                std::fill(frame_samples.begin(), frame_samples.end(), Sint16(0));
            }
            if (wav.is_open()) wav.write(frame_samples);
            if (audio_device != 0 && !frame_samples.empty()) {
                // If the emulator runs ahead the queue would grow without
                // bound and audio would lag further and further behind the
                // picture, so past a few frames of backlog we drop instead.
                if (SDL_GetQueuedAudioSize(audio_device) < kMaxQueuedAudioBytes) {
                    SDL_QueueAudio(audio_device, frame_samples.data(),
                                   static_cast<Uint32>(frame_samples.size() * sizeof(Sint16)));
                }
            }

            SDL_Event event;
            event.type = SDL_USEREVENT;
            event.user.data1 = cpu->get_memory()->data();
            event.user.data2 = renderer;
            SDL_PushEvent(&event);
        });
        while (running && rom_loaded) {
            cpu.step();
        }
    });

    // Initialize a renderer event type and timer for periodic rendering
    Uint32 render_event_type = SDL_RegisterEvents(1);
    //SDL_TimerID render_timer_id = SDL_AddTimer(20 , render_timer_callback, &render_event_type);

    // Main thread event loop to handle SDL events and rendering
    SDL_Event event;
    while (running && SDL_WaitEvent(&event)) {
        // Handle Quit event (User pressed the close button)
        if (event.type == SDL_QUIT) {
            running = false;
            //SDL_RemoveTimer(render_timer_id);
            break;
        // Handle custom render event (triggered by the CPU thread) 
        } else if (event.type == SDL_USEREVENT) {
            // Clear to the border colour; the display is then drawn centred
            // on top of it, leaving the border visible around the edges.
            const Colour& border = kPalette[0][border_colour.load(std::memory_order_relaxed) & 0x07];
            SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, 255);
            SDL_RenderClear(renderer);

            if (rom_loaded) {
                render_screen_memory(renderer, screen_texture, cpu.get_memory());
            }

            SDL_RenderPresent(renderer);
        // Handle Key Down and Key Up events
        } else if (event.type == SDL_KEYDOWN | event.type == SDL_KEYUP) {
            // Handle key press events
            switch (event.key.keysym.sym) {
                // Handle Escape key to exit the emulator
                case SDLK_ESCAPE:
                    running = false;
                    //SDL_RemoveTimer(render_timer_id);
                    running = false;
                    break;
                // Tape controls. The Spectrum uses every key it has, so these
                // live on function keys, which the matrix does not map.
                // Rewind matters: the ROM's LOAD keeps asking for blocks, so
                // once the tape runs out it will never find anything again.
                case SDLK_F5:
                    if (event.type == SDL_KEYDOWN) {
                        tape_rewind_requested.store(true, std::memory_order_relaxed);
                    }
                    break;
                case SDLK_F6:
                    if (event.type == SDL_KEYDOWN) {
                        tape_reload_requested.store(true, std::memory_order_relaxed);
                    }
                    break;
                case SDLK_F7:
                    if (event.type == SDL_KEYDOWN) {
                        tape_new_requested.store(true, std::memory_order_relaxed);
                    }
                    break;
                case SDLK_F8:
                    if (event.type == SDL_KEYDOWN) {
                        const bool now = !audio_muted.load(std::memory_order_relaxed);
                        audio_muted.store(now, std::memory_order_relaxed);
                        std::printf("Sound %s.\n", now ? "muted" : "unmuted");
                    }
                    break;
                // Handle other key events to update the keyboard matrix
                default:
                    handle_key_event(event);
                    break;
            }
            SDL_RenderPresent(renderer);
        }
    }

    cpu_thread.join();

    if (audio_device != 0) SDL_CloseAudioDevice(audio_device);
    if (wav.is_open()) {
        std::printf("Captured %zu samples (%.1fs).\n", wav.frames(),
                    double(wav.frames()) / BEEPER_DEFAULT_SAMPLE_RATE);
        wav.close();
    }

    SDL_DestroyTexture(screen_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
