# Godot Engine

## Project Overview

Godot is a feature-packed, cross-platform game engine to create 2D and 3D games from a unified interface. It is free and open-source software released under the MIT license.

**Key Technologies:**
*   **Languages:** C++ (Core), Python (Build System via SCons), GLSL (Shaders).
*   **Build System:** SCons.
*   **Test Framework:** doctest (C++).

## Directory Structure

*   **`core/`**: The heart of the engine. Contains main loop, math library, memory management, and low-level data structures.
*   **`editor/`**: Source code for the Godot Editor application.
*   **`scene/`**: High-level nodes and scene system (e.g., `Node`, `Sprite2D`, `PhysicsBody3D`).
*   **`servers/`**: Low-level systems for rendering, audio, physics, etc. (e.g., `RenderingServer`, `PhysicsServer3D`).
*   **`modules/`**: Optional components and extensions (e.g., `gdscript`, `mono`, `gltf`).
*   **`drivers/`**: Abstractions for OS-specific APIs (e.g., Vulkan, OpenGL, Audio drivers).
*   **`platform/`**: OS-specific entry points and implementations (e.g., `windows`, `macos`, `android`, `web`).
*   **`tests/`**: Unit tests using the `doctest` framework.
*   **`misc/`**: Utility scripts, icons, and configuration files.

## Building and Running

### Prerequisites
*   **Python 3.8+**
*   **SCons 4.0+**
*   C++ Compiler (GCC, Clang, or MSVC)

### Build Commands

1.  **Compile the Editor:**
    ```bash
    scons platform=<your_platform> target=editor
    ```
    *   Common platforms: `linuxbsd`, `windows`, `macos`, `web`, `android`, `ios`.
    *   Binaries are output to the `bin/` directory.

2.  **Compile Export Templates (Release):**
    ```bash
    scons platform=<your_platform> target=template_release
    ```

3.  **Compile Export Templates (Debug):**
    ```bash
    scons platform=<your_platform> target=template_debug
    ```

4.  **Common SCons Flags:**
    *   `target=editor` (default): Build the editor.
    *   `arch=<arch>`: Specify architecture (e.g., `x86_64`, `arm64`).
    *   `dev_build=yes`: Enable dev-only debugging code.
    *   `vsproj=yes`: Generate Visual Studio project files (Windows).
    *   `compile_commands=yes`: Generate `compile_commands.json` for generic LSP support.

## Testing

Godot uses **doctest** for C++ unit testing.

### 1. Build with Tests Enabled
You must compile the engine with the `tests=yes` flag to include the test suite.

```bash
scons platform=<your_platform> tests=yes
```

### 2. Run Tests
Execute the compiled binary with the `--test` argument.

```bash
./bin/godot.<platform>.editor.<arch> --test
```

*   **Run specific tests:**
    ```bash
    ./bin/godot.<platform>.editor.<arch> --test --test-case="*Vector3*"
    ```
    (Accepts doctest filter patterns).

## Development Conventions

*   **Code Style:**
    *   Strict adherence to **clang-format**.
    *   Run `misc/scripts/clang_format.sh` (or similar) to check/fix style.
    *   See `.clang-format` for configuration.

*   **Git & Contribution:**
    *   **Commit Messages:** Must be clear and follow the format:
        ```
        Short summary (under 72 chars, imperative verb)

        More detailed description of why the change is needed,
        what was fixed, etc.
        ```
    *   **Pull Requests:** One feature/fix per PR. Rebase on `upstream/master` (or target branch) to avoid merge commits.
    *   **Unit Tests:** New features and bug fixes should be accompanied by unit tests in the `tests/` directory whenever possible.

*   **Modules:**
    *   New functionality is often best implemented as a module in `modules/` rather than modifying `core/` directly, unless it's a fundamental engine change.

## Useful Commands

*   **Generate Compilation Database (compile_commands.json):**
    ```bash
    scons compiledb=yes
    ```
*   **Clean Build:**
    ```bash
    scons --clean
    ```
