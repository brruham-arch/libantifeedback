# libantifeedback

AML native mod untuk SA-MP Android.  
Auto mute speaker SV saat mic aktif, mencegah feedback audio.

## Cara kerja

- Hook `BASS_RecordStart` → tangkap handle recording
- Hook `BASS_ChannelPlay` → deteksi channel SV (freq=48000, mono)
- Hook `BASS_ChannelFree` → bersihkan channel dari list
- Polling thread 50ms → deteksi mic ON/OFF via `BASS_ChannelIsActive`
- Saat mic ON → `BASS_ChannelSetAttribute(h, VOL, 0.0)` semua SV channel
- Saat mic OFF → volume kembali ke 1.0

## Build

GitHub Actions otomatis build saat push ke `main` atau `dev`.  
Download artifact `libantifeedback-arm32` dari tab Actions.

## Install

Taruh `libantifeedback.so` ke:
```
/storage/emulated/0/Android/data/com.sampmobilerp.game/mods/
```

## Log

```
/storage/emulated/0/antifeedback_log.txt
```

## Bridge Lua (opsional)

```lua
local ffi = require "ffi"
ffi.cdef[[
    typedef struct {
        int (*is_mic_active)(void);
        int (*get_sv_count)(void);
    } AntiFeedbackAPI;
]]

local f = io.open("/storage/emulated/0/antifeedback_addr.txt", "r")
local addr = tonumber(f:read("*l")); f:close()
local api = ffi.cast("AntiFeedbackAPI*", addr)

-- api.is_mic_active() -> 1/0
-- api.get_sv_count()  -> jumlah channel SV aktif
```

## Author

brruham-arch
