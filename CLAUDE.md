# Music Buzzer - Claude Code Configuration

ESP32-S3 multi-track music player with 5 passive buzzers and PWA control interface.

## Project Overview

- **Platform**: ESP32-S3 with PlatformIO (espressif32 6.12.0)
- **Architecture**: Single-file (`src/main.cpp` ~1350 lines) with embedded PWA
- **PSRAM**: 8MB OPI PSRAM for GPT model weights and KV cache
- **LittleFS**: Model binary stored in `/data/model.bin`, uploaded via `pio run -t uploadfs`
- **Hardware**: 5 buzzers on GPIO 4-7,15; stop button on GPIO 16
- **Network**: Static IP 192.168.0.201, WebSocket for real-time state

## Important Technical Notes

### ESP32 LEDC API (Core 2.x)
This project uses the **Arduino Core 2.x API**, NOT Core 3.x:
```cpp
ledcSetup(channel, freq, resolution)  // Configure channel
ledcAttachPin(pin, channel)           // Attach pin to channel
ledcWriteTone(channel, freq)          // Play tone on channel
ledcDetachPin(pin)                    // Detach pin
```
Do NOT use Core 3.x functions like `ledcAttachChannel()` or pin-based `ledcWriteTone(pin, freq)`.

### GPT Inference Engine
- `include/mini_gpt.h` + `src/mini_gpt.cpp` — INT8 quantized inference on ESP32-S3
- Weights loaded from LittleFS into PSRAM via zero-copy pointer arithmetic
- KV cache in PSRAM, activation buffers in internal SRAM
- Graceful degradation: firmware works fully without model file

### Threading Rules (FreeRTOS)
- AsyncTCP runs on **core 1** (`CONFIG_ASYNC_TCP_RUNNING_CORE=1`)
- GPT generation task runs on **core 0** via `xTaskCreatePinnedToCore`
- **NEVER call `ws.textAll()` from core 0** — use a FreeRTOS queue to relay messages to core 1's `loop()`
- `volatile` required for flags shared between cores (`generating`, `genAbort`)

### Custom Partition Table
- `partitions.csv` gives 3MB to app, 1.875MB to LittleFS, 64KB coredump
- Model binary must fit in LittleFS partition — check size before flashing

### Key Constants
- `MAX_TRACKS = 5` - Used for all track array sizes
- Channels 0-4 map to buzzers on GPIO 4,5,6,7,15
- LEDC resolution: 8-bit

### Hardware Limitations
- Passive buzzers: volume cannot be software-controlled (ledcWriteTone sets 50% duty cycle internally)
- Volume control would require hardware modification (potentiometer/amplifier) or active buzzers

## File Structure

```
src/main.cpp      # All firmware + embedded PWA HTML
src/mini_gpt.cpp  # GPT inference engine
include/
  config.h        # Pin definitions, network config
  secrets.h       # WiFi credentials (gitignored)
  songs.h         # 300+ songs in PROGMEM (RTTTL/MML format)
  mini_gpt.h      # GPT inference header
data/
  model.bin       # Trained GPT weights (INT8, ~1.2MB)
partitions.csv    # Custom flash partition table
```

## Worktrees

Worktree directory: `.worktrees/`

Active branches:
- `feature/song-improvements` - Fixing/improving existing songs
- `feature/player-timing` - Player timing and mechanics

## Build Commands

```bash
pio run              # Compile (auto-flashes on success via hook)
pio run -t upload    # Flash to device
pio run -t compiledb # Generate compile_commands.json for LSP
pio run -t uploadfs  # Upload data/ directory to LittleFS
```

**Serial Monitor**: Do NOT run `pio device monitor` - it hangs Claude Code. Ask the user to run it themselves and paste output back.

## Related Projects

- `nicholas-buzzer` (192.168.0.200) - Single buzzer version, shares `songs.h`

## Automations

- **Auto-flash**: Successful `pio run` automatically triggers `pio run -t upload`
- **secrets.h protection**: Edits to secrets.h are blocked
- **/flash skill**: User can invoke `/flash` for quick build+upload

## Code Style

- Prefer direct DOM manipulation over innerHTML for security
- Keep all code in single main.cpp file
- Use PROGMEM for all song data
- WebSocket messages: JSON format for state sync

### Web UI Pages
- `/` — Main song list (INDEX_HTML)
- `/generate` — GPT melody generation page (GENERATE_HTML)
- Both pages share WebSocket at `ws://host/ws`
