# GenieBuild

GenieBuild is a lightweight C/C++ build manager with a fast UI and CLI. Configure multi-target builds, glob source rules, per-folder/file flag overrides, custom configurations, and incremental parallel compilation in a single `project.json`—no CMake or Makefiles required.

## Features

- **Project Explorer & Visual Build Control**: Browse folders, toggle individual files or directories, and drag-and-drop project folders directly onto the window.
- **Glob Source Rules**: Auto-discover sources using `include`/`exclude` glob patterns (`**`, `*`, `?`, `{a,b}`) and optional platform/compiler conditions (`if`).
- **Multi-Target Dependency Graph**: Define `executable`, `static`, `shared`, and `interface` (header-only) targets with topological build ordering, transitive `public` usage propagation, and cycle detection.
- **Per-Folder & Per-File Flag Overrides**: Override optimization level, debug info, defines, includes, and compile flags for specific folders or files, with clickable `Effective Flags` origin tracking and selective incremental recompilation.
- **Custom Configurations**: Create configurations built on `debug`, `release`, `relWithDebInfo`, or `minSize` presets with configurable LTO, runtime CRT (`dynamic`/`static`), and isolated object/output directories.
- **Incremental Parallel Builds & Diagnostics**: Tracks compiler invocation hashes and timestamps, skips up-to-date objects, supports response files (`.rsp`), and streams color-coded compiler diagnostics.
- **Toolchain Auto-Detection & Headless CLI**: Detects `g++`, `clang++`, `gcc`, `clang`, and MSVC `cl` from `PATH`, and supports scriptable CLI builds alongside the OpenGL/ImGui desktop app.

## Building from Source

### Prerequisites

- **CMake** 3.16 or newer
- **C++17** compiler (`g++`, `clang++`, or MSVC)
- **OpenGL** development libraries
- Git & internet access on first configure (dependencies are fetched via CMake `FetchContent`: GLFW 3.5.1, Dear ImGui v1.93.0, nlohmann/json v3.12.0, reproc 14.2.8, nativefiledialog-extended v1.4.1)

### Configure & Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On Windows with MinGW, CMake statically links the C++ runtime and copies `assets/` next to the compiled `geniebuild` executable.

## Usage

### GUI Mode

Launch the app with no arguments or pass a project folder:

```bash
./build/geniebuild
./build/geniebuild path/to/project
```

### Headless CLI Mode

```bash
./build/geniebuild --build path/to/project
./build/geniebuild --build --target my_app --config Release path/to/project
```

You can try it immediately on the included [sample_project](sample_project/):

```bash
./build/geniebuild --build sample_project
```

## Project Structure

- `main.cpp` — Application entry point, CLI mode, window setup, and frame loop
- `core/` — Standalone build engine (`project`, `target`, `source_rules`, `settings_resolver`, `compiler`, `build_runner`), independent of UI libraries
- `ui/` — Dear ImGui panels, theme styling, tree views, build settings modal, and output terminal
- `assets/fonts/` — Bundled UI, monospace, and icon fonts
- `sample_project/` — Minimal multi-file C++20 sample workspace

## License

Licensed under the [MIT License](LICENSE).
