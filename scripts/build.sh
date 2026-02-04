#!/usr/bin/env bash
# Build Surge XT module for Move Anything (ARM64)
#
# Uses CMake to build the Surge core engine and plugin wrapper.
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-surge-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Surge XT Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
cd "$REPO_ROOT"

echo "=== Building Surge XT Module ==="

# Create build directory
mkdir -p build

# Run CMake configure with cross-compilation toolchain
echo "Configuring CMake..."
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja \
    2>&1

# Build
echo "Building (this may take a while)..."
cmake --build build --target surge-move-plugin -j$(nproc) 2>&1

# Package
echo "Packaging..."
mkdir -p dist/surge

# Copy files to dist
cat src/module.json > dist/surge/module.json
cat src/ui.js > dist/surge/ui.js
cat build/dsp.so > dist/surge/dsp.so
chmod +x dist/surge/dsp.so

# Copy Surge factory data if available
if [ -d "src/dsp/surge/resources/data" ]; then
    echo "Copying Surge factory data..."
    mkdir -p dist/surge/surge-data
    # Copy patches (factory presets), excluding non-functional categories
    if [ -d "src/dsp/surge/resources/data/patches_factory" ]; then
        cp -r src/dsp/surge/resources/data/patches_factory dist/surge/surge-data/
        rm -rf dist/surge/surge-data/patches_factory/Tutorials   # Requires Lua (Formula Modulator)
        rm -rf dist/surge/surge-data/patches_factory/Templates   # Audio In / Init templates
    fi
    # Copy wavetables
    if [ -d "src/dsp/surge/resources/data/wavetables" ]; then
        cp -r src/dsp/surge/resources/data/wavetables dist/surge/surge-data/
    fi
    # Copy configuration
    if [ -f "src/dsp/surge/resources/surge-shared/configuration.xml" ]; then
        cp src/dsp/surge/resources/surge-shared/configuration.xml dist/surge/surge-data/
    fi
fi

# Create tarball for release
cd dist
tar -czvf surge-module.tar.gz surge/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/surge/"
echo "Tarball: dist/surge-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
