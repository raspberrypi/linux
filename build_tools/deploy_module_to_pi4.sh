#!/bin/bash

# Deploy adv7180 module to PI4 automatically

set -e

# Get the script directory and module path
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODULE_FILE="${KERNEL_DIR}/module_output/adv7180.ko"

if [ ! -f "${MODULE_FILE}" ]; then
    echo "❌ Error: Module not found!"
    echo "Please build it first: ./build_adv7180_module_only.sh"
    exit 1
fi

echo "========================================="
echo "Deploy adv7180 Module to PI4"
echo "========================================="
echo ""

# Get PI4 IP address
read -p "Enter PI4 IP address: " PI_IP

if [ -z "$PI_IP" ]; then
    echo "❌ Error: IP address required"
    exit 1
fi

echo ""
echo "🔗 Connecting to PI4 at ${PI_IP}..."

# Copy module to PI4
echo "📤 Copying module..."
scp "${MODULE_FILE}" pi@${PI_IP}:~/

# Deploy on PI4
echo "🔧 Installing module on PI4..."
ssh pi@${PI_IP} << 'ENDSSH'
set -e

echo "Finding existing module location..."
MODULE_PATH=$(find /lib/modules/$(uname -r) -name adv7180.ko | head -n 1)

if [ -z "$MODULE_PATH" ]; then
    echo "❌ Error: Could not find adv7180.ko in kernel modules"
    echo "You may need to build and install the full kernel first"
    exit 1
fi

echo "Found at: $MODULE_PATH"

# Check if module is currently loaded
if lsmod | grep -q adv7180; then
    echo "⚠️  Module is currently loaded, unloading..."
    sudo rmmod adv7180 || true
    sleep 1
fi

# Backup existing module
echo "💾 Backing up existing module..."
sudo cp "$MODULE_PATH" "${MODULE_PATH}.backup.$(date +%Y%m%d_%H%M%S)"

# Install new module
echo "📥 Installing new module..."
sudo cp ~/adv7180.ko "$MODULE_PATH"

# Update module dependencies
echo "🔄 Updating module dependencies..."
sudo depmod -a

# Load the module
echo "▶️  Loading module..."
sudo modprobe adv7180

# Verify
echo ""
echo "✅ Module installed and loaded!"
echo ""
echo "Module info:"
modinfo adv7180 | head -n 5
echo ""
echo "Loaded modules:"
lsmod | grep adv7180
echo ""
echo "Recent kernel messages:"
dmesg | grep adv7180 | tail -5

ENDSSH

echo ""
echo "========================================="
echo "✅ Deployment complete!"
echo "========================================="
echo ""
echo "To verify on PI4:"
echo "  ssh pi@${PI_IP}"
echo "  lsmod | grep adv7180"
echo "  dmesg | grep adv7180"

