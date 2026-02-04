# Surge XT for Move Anything

Hybrid synthesizer module based on [Surge XT](https://github.com/surge-synthesizer/surge) by the Surge Synth Team.

Wavetable, FM, subtractive, and physical modeling synthesis with 600+ factory presets.

## Features

- All Surge XT oscillator types: Classic, Wavetable, Window, Sine, FM2, FM3, String, Twist, Alias, S&H Noise
- Dual filters with multiple filter types
- 3 LFOs per scene with sine, triangle, square, ramp, noise, S&H, envelope, and step sequencer shapes
- Amplitude and filter envelopes
- 600+ factory presets across 15 categories
- Works standalone or as a sound generator in Signal Chain patches

## Limitations

- **Lua/Formula Modulator not supported** - Surge's Formula Modulator LFO shape requires LuaJIT, which is not included in this build. Patches that use Formula Modulator LFOs will still load and produce sound, but the formula-driven modulation will not be active. The Tutorials preset folder (which relies heavily on Formula Modulator) is excluded.
- **Scene B not exposed** - Only Scene A parameters are accessible. Scene B exists internally but is not routed to the UI.
- **No FX section** - Surge's built-in effects are not exposed (use Signal Chain audio FX instead).

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Via Module Store (Recommended)

1. Launch Move Anything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Sound Generators** > **Surge XT**
4. Select **Install**

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

```bash
git clone --recursive https://github.com/charlesvestal/move-anything-surge
cd move-anything-surge
./scripts/build.sh
./scripts/install.sh
```

## Controls

| Control | Function |
|---------|----------|
| Jog wheel | Browse presets / navigate menus |
| Knobs 1-8 | Adjust parameters for current category |

In Shadow UI / Signal Chain, parameters are organized into navigable categories:
Oscillators 1-3, Mixer, Filters 1-2, Amp Envelope, Filter Envelope, LFOs 1-3, and Scene settings.

## Preset Categories

Basses, Brass, Chords, FX, Keys, Leads, MPE, Pads, Percussion, Plucks, Polysynths, Sequences, Splits, Vocoder, Winds

## License

GPL-3.0 - See [LICENSE](LICENSE)

Based on Surge XT by the Surge Synth Team, which is also GPL-3.0 licensed.
