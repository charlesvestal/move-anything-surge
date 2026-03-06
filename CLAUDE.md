# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

Surge XT module for Move Anything - a hybrid synthesizer based on Surge XT by the Surge Synth Team. Supports wavetable, FM, subtractive, and physical modeling synthesis with 12 oscillator types.

## Architecture

```
src/
  dsp/
    surge/                # Surge XT git submodule (full source)
    surge_plugin.cpp      # Move Anything plugin wrapper (V2 API)
  module.json             # Module metadata
  ui.js                   # JavaScript UI
cmake/
  aarch64-toolchain.cmake # Cross-compilation toolchain
CMakeLists.txt            # Top-level CMake build
```

## Key Implementation Details

### Plugin API

Implements Move Anything plugin_api_v2 (multi-instance):
- `create_instance`: Creates SurgeSynthesizer with headless config
- `destroy_instance`: Cleanup
- `on_midi`: Routes MIDI to Surge (notes, CC, pitch bend, aftertouch)
- `set_param`: preset, octave_transpose, cutoff, resonance, ADSR
- `get_param`: preset browsing, ui_hierarchy, chain_params, state
- `render_block`: Calls Surge process() 4x per Move block (32 -> 128 frames)

### Audio Processing

Surge processes 32 samples per block, Move needs 128. The render function calls
`synth->process()` 4 times per Move render block and copies float output to int16.

### Build System

Unlike other modules that use simple g++ commands, this module uses CMake because
Surge has a complex build with many dependencies. The build flags:
- `SURGE_SKIP_JUCE_FOR_RACK=ON` - No JUCE dependency (headless)
- `SURGE_SKIP_LUA=ON` - No LuaJIT (disables formula modulator)
- `SURGE_SKIP_ODDSOUND_MTS=ON` - No MTS-ESP
- Cross-compilation via `cmake/aarch64-toolchain.cmake`

### Parameters

Currently exposes a subset of Surge's ~766 parameters focused on Scene A:
- `cutoff`, `resonance` - Filter 1
- `attack`, `decay`, `sustain`, `release` - Amp envelope
- `filter_env` - Filter envelope amount
- `octave_transpose` - Plugin-level octave shift

### MPE Support

Surge XT has full MPE (MIDI Polyphonic Expression) support:
- `mpe_enabled` / `mpe_pitch_bend_range` params control MPE mode
- Auto-detection via RPN 0,6 (MPE Configuration Message) — when an MPE controller
  like LinnStrument sends its MCM, Surge enables MPE automatically
- Per-note pitch bend, channel pressure, and CC74 (timbre/slide) are routed
  to per-voice modulation when MPE is active

**Important**: For MPE to work on Move, the chain slot must be configured:
1. Receive Channel = All (so all MPE member channels reach the synth)
2. Forward Channel = THRU (so per-channel data isn't remapped to one channel)

Without this, the slot's channel remapping destroys MPE data before it reaches Surge.

Factory and third-party MPE presets are included:
- `patches_factory/MPE/` — 9 factory MPE patches
- `patches_3rdparty/LinnStrument MPE/` — 63 Roger Linn MPE patches across 12 categories

### Preset Management

Surge patches are loaded from the factory data bundled in `surge-data/`.
Patches are browsed by index using preset/preset_count/preset_name params.
Third-party patches (including LinnStrument MPE) are in `patches_3rdparty/`.

### Dependencies (via submodules)

Core engine: simde (SIMD portability), fmt, pffft, r8brain, tuning-library,
sst-basic-blocks, sst-filters, sst-waveshapers, sst-effects, sst-plugininfra,
eurorack, sqlite, zstd, PEGTL, airwindows, binn

## License

GPL-3.0 (inherited from Surge XT)
