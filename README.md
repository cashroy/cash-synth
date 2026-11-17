# Cash Synth

A JUCE 8 synthesizer plugin (VST3, AU, Standalone).

## Build

Requires CMake 3.22+ and a C++20 compiler (Xcode CLT on macOS, VS2022 on Windows).
JUCE and Catch2 are fetched automatically on first configure.

```bash
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

On Windows: `cmake -B build -G "Visual Studio 17 2022"` then `cmake --build build --config Debug`.

Built plugins are copied to the user plug-in folders (`~/Library/Audio/Plug-Ins/{VST3,Components}` on macOS).

## Licence

Project code: MIT (see `LICENSE`). JUCE is licensed separately (AGPLv3 or commercial).
