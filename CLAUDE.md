# Music Buzzer - Claude Code Configuration

ESP32-S3 multi-track music player with 5 passive buzzers and PWA control interface.

## Project Overview

- **Platform**: ESP32-S3 with PlatformIO (espressif32 6.12.0)
- **Architecture**: Single-file (`src/main.cpp` ~850 lines) with embedded PWA
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

### Key Constants
- `MAX_TRACKS = 5` - Used for all track array sizes
- Channels 0-4 map to buzzers on GPIO 4,5,6,7,15
- LEDC resolution: 8-bit

## File Structure

```
src/main.cpp      # All firmware + embedded PWA HTML
include/
  config.h        # Pin definitions, network config
  secrets.h       # WiFi credentials (gitignored)
  songs.h         # 300+ songs in PROGMEM (RTTTL/MML format)
```

## Worktrees

Worktree directory: `.worktrees/`

Active branches:
- `feature/song-improvements` - Fixing/improving existing songs
- `feature/player-timing` - Player timing and mechanics

## Build Commands

```bash
pio run              # Compile
pio run -t upload    # Flash to device
pio device monitor   # Serial monitor
```

## Code Style

- Prefer direct DOM manipulation over innerHTML for security
- Keep all code in single main.cpp file
- Use PROGMEM for all song data
- WebSocket messages: JSON format for state sync
