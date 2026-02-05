---
name: flash
description: Build and flash firmware to ESP32
disable-model-invocation: true
---

Build and upload firmware to the connected ESP32:

```bash
pio run -t upload
```

Report success or failure. Do NOT attempt to open serial monitor - if you need serial output, tell the user to run `pio device monitor` themselves and paste the output back.
