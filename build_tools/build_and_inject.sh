#!/bin/bash

# Complete workflow: Build driver and inject into image

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  Build & Inject Driver - Complete Workflow              ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Step 1: Build the module
echo "Step 1/2: Building adv7180 driver module..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
"${SCRIPT_DIR}/build_adv7180_module_only.sh"

if [ $? -ne 0 ]; then
    echo "❌ Build failed"
    exit 1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Step 2: Inject into image
echo "Step 2/2: Injecting driver into PI4 image..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
"${SCRIPT_DIR}/inject_driver_to_image.sh"

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ✅ Complete! Ready to flash to SD card                  ║"
echo "╚══════════════════════════════════════════════════════════╝"

