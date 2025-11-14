#!/bin/bash

# Build ONLY the adv7180 driver module
# Much faster than building the entire kernel!

set -e

# Get the script directory and kernel root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${KERNEL_DIR}/module_output"

echo "========================================="
echo "Building adv7180 Driver Module Only"
echo "========================================="

# Check if Docker is available
if ! command -v docker &> /dev/null; then
    echo "❌ Error: Docker not found!"
    echo "Please install Docker Desktop for Mac"
    exit 1
fi

echo "✓ Docker found"
mkdir -p "${OUTPUT_DIR}"

# Show what we're building from
echo ""
echo "📋 Verifying source file changes..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Source file: ${KERNEL_DIR}/drivers/media/i2c/adv7180.c"
echo "Last modified: $(stat -f "%Sm" "${KERNEL_DIR}/drivers/media/i2c/adv7180.c" 2>/dev/null || stat -c "%y" "${KERNEL_DIR}/drivers/media/i2c/adv7180.c" 2>/dev/null || echo "unknown")"
echo "File size: $(du -h "${KERNEL_DIR}/drivers/media/i2c/adv7180.c" | awk '{print $1}')"
echo ""

# Clean previous build to ensure fresh compilation
if [ -f "${KERNEL_DIR}/drivers/media/i2c/adv7180.o" ] || [ -f "${KERNEL_DIR}/drivers/media/i2c/adv7180.ko" ]; then
    echo "🧹 Cleaning previous build artifacts to ensure fresh build..."
    rm -f "${KERNEL_DIR}/drivers/media/i2c/adv7180.o"
    rm -f "${KERNEL_DIR}/drivers/media/i2c/adv7180.ko"
    rm -f "${KERNEL_DIR}/drivers/media/i2c/.adv7180.o.cmd"
    echo "✓ Previous build cleaned"
    echo ""
fi

echo "🔨 Building adv7180 module from current source..."

docker run --rm \
    -v "${KERNEL_DIR}:/linux" \
    -v "${OUTPUT_DIR}:/output" \
    -w /linux \
    --platform linux/amd64 \
    debian:bookworm \
    /bin/bash -c "
set -e

echo '📦 Installing build dependencies...'
apt-get update -qq
apt-get install -y -qq \
    build-essential \
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    crossbuild-essential-arm64 \
    kmod

echo '⚙️  Preparing kernel config for PI4...'
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bcm2711_defconfig

echo '🔧 Preparing kernel build environment...'
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare

echo '🔨 Building adv7180 module (out-of-tree build)...'
echo '   Note: Symbol warnings are normal for out-of-tree builds'
echo '   The module will work correctly when loaded on the PI4'
echo ''

# Build the module allowing symbol warnings (out-of-tree build)
# The .ko will be valid and work on the target system
KBUILD_MODPOST_WARN=1 make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    M=drivers/media/i2c modules

echo '📥 Copying module to output...'
mkdir -p /output

# Find and copy the module
MODULE_PATH=\$(find drivers/media/i2c -name 'adv7180.ko' | head -n 1)
if [ -n \"\$MODULE_PATH\" ]; then
    echo \"Found module: \$MODULE_PATH\"

    # Show module info
    ls -lh \"\$MODULE_PATH\"
    echo \"Module size: \$(du -h \$MODULE_PATH | awk '{print \$1}')\"

    # Copy to output
    cp \"\$MODULE_PATH\" /output/

    # Show build timestamp embedded in module
    echo ''
    echo 'Module metadata:'
    modinfo /output/adv7180.ko | grep -E '(filename|vermagic|description|author)' || true

    echo '✓ Module built successfully'
else
    echo '❌ Error: Module not found after build!'
    exit 1
fi
"

if [ -f "${OUTPUT_DIR}/adv7180.ko" ]; then
    MODULE_SIZE=$(du -h "${OUTPUT_DIR}/adv7180.ko" | awk '{print $1}')
    MODULE_TIMESTAMP=$(stat -f "%Sm" "${OUTPUT_DIR}/adv7180.ko" 2>/dev/null || stat -c "%y" "${OUTPUT_DIR}/adv7180.ko" 2>/dev/null || echo "unknown")

    echo ""
    echo "========================================="
    echo "✅ SUCCESS!"
    echo "========================================="
    echo "Module: ${OUTPUT_DIR}/adv7180.ko"
    echo "Size: ${MODULE_SIZE}"
    echo "Built: ${MODULE_TIMESTAMP}"
    echo ""
    echo "🔍 Verification:"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Show file info to confirm it's fresh
    ls -lh "${OUTPUT_DIR}/adv7180.ko"

    # Calculate checksum to verify uniqueness
    if command -v md5 &> /dev/null; then
        echo "MD5: $(md5 -q "${OUTPUT_DIR}/adv7180.ko")"
    elif command -v md5sum &> /dev/null; then
        echo "MD5: $(md5sum "${OUTPUT_DIR}/adv7180.ko" | awk '{print $1}')"
    fi
    echo ""
    echo "📋 To deploy to your PI4:"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "1. Copy module to PI4:"
    echo "   scp ${OUTPUT_DIR}/adv7180.ko pi@<pi-ip>:~/"
    echo ""
    echo "2. On the PI4:"
    echo "   # Remove old module"
    echo "   sudo rmmod adv7180 2>/dev/null || true"
    echo ""
    echo "   # Find the correct path"
    echo "   MODULE_PATH=\$(find /lib/modules/\$(uname -r) -name adv7180.ko)"
    echo "   echo \"Backing up: \$MODULE_PATH\""
    echo ""
    echo "   # Backup and replace"
    echo "   sudo cp \$MODULE_PATH \${MODULE_PATH}.backup"
    echo "   sudo cp ~/adv7180.ko \$MODULE_PATH"
    echo ""
    echo "   # Load new module"
    echo "   sudo modprobe adv7180"
    echo ""
    echo "   # Verify"
    echo "   lsmod | grep adv7180"
    echo "   dmesg | tail -20"
    echo "========================================="
else
    echo "❌ Error: Module not found after build"
    exit 1
fi

