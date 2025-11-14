#!/bin/bash

# Inject custom adv7180 driver into Raspberry Pi image
# This modifies the image file so the custom driver is included when you flash it

set -e

# Get directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODULE_FILE="${KERNEL_DIR}/module_output/adv7180.ko"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  Inject Custom Driver into Raspberry Pi Image           ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Check if module exists
if [ ! -f "${MODULE_FILE}" ]; then
    echo "❌ Error: Module not found at ${MODULE_FILE}"
    echo "Please build it first:"
    echo "  cd build_tools"
    echo "  ./build_adv7180_module_only.sh"
    exit 1
fi

MODULE_SIZE=$(du -h "${MODULE_FILE}" | awk '{print $1}')
echo "✓ Found module: ${MODULE_FILE} (${MODULE_SIZE})"
echo ""

# Preset image paths (only uncompressed images)
PRESET_TRIXIE="${SCRIPT_DIR}/2025-10-01-raspios-trixie-arm64.img"
PRESET_SOKIL="${SCRIPT_DIR}/sokil-min.raspberry.img"

# Build menu of available images
echo "Available images (base images, will be copied before injection):"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

MENU_OPTIONS=()
MENU_COUNTER=1

# Add Trixie preset if exists
if [ -f "$PRESET_TRIXIE" ]; then
    SIZE=$(du -h "$PRESET_TRIXIE" | awk '{print $1}')
    echo "$MENU_COUNTER) Trixie (RasPiOS latest): 2025-10-01-raspios-trixie-arm64.img ($SIZE)"
    MENU_OPTIONS+=("$PRESET_TRIXIE")
    ((MENU_COUNTER++))
fi

# Add Sokil preset if exists
if [ -f "$PRESET_SOKIL" ]; then
    SIZE=$(du -h "$PRESET_SOKIL" | awk '{print $1}')
    echo "$MENU_COUNTER) Sokil (custom): sokil-min.raspberry.img ($SIZE)"
    MENU_OPTIONS+=("$PRESET_SOKIL")
    ((MENU_COUNTER++))
fi

echo "$MENU_COUNTER) Custom path (enter manually)"
echo ""

# Ask user to pick
read -p "Select image [1-$MENU_COUNTER]: " CHOICE

if [ -z "$CHOICE" ]; then
    echo "❌ Error: Choice required"
    exit 1
fi

# Validate choice
if ! [[ "$CHOICE" =~ ^[0-9]+$ ]] || [ "$CHOICE" -lt 1 ] || [ "$CHOICE" -gt "$MENU_COUNTER" ]; then
    echo "❌ Error: Invalid choice"
    exit 1
fi

# Get selected path
if [ "$CHOICE" -eq "$MENU_COUNTER" ]; then
    # Custom path
    echo ""
    read -p "Enter path to your PI4 image (.img or .img.xz): " IMAGE_PATH
    if [ -z "$IMAGE_PATH" ]; then
        echo "❌ Error: Image path required"
        exit 1
    fi
    # Expand ~ to home directory
    IMAGE_PATH="${IMAGE_PATH/#\~/$HOME}"
else
    # Use preset
    IMAGE_PATH="${MENU_OPTIONS[$((CHOICE-1))]}"
fi

if [ ! -f "$IMAGE_PATH" ]; then
    echo "❌ Error: Image file not found: $IMAGE_PATH"
    exit 1
fi

echo "✓ Found base image: $IMAGE_PATH"
echo ""

# Check if compressed
if [[ "$IMAGE_PATH" == *.xz ]]; then
    echo "❌ Error: Compressed images not supported"
    echo "Please decompress the image first:"
    echo "  xz -dk $IMAGE_PATH"
    exit 1
fi

# Create a copy with timestamp to preserve the original
BASE_NAME=$(basename "$IMAGE_PATH" .img)
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
WORK_IMAGE="$(dirname "$IMAGE_PATH")/${BASE_NAME}_custom_${TIMESTAMP}.img"

echo "📋 Creating working copy to preserve original..."
echo "   Original: $IMAGE_PATH"
echo "   Working:  $WORK_IMAGE"
echo ""
echo "⏳ Copying image (this may take 2-5 minutes)..."
cp "$IMAGE_PATH" "$WORK_IMAGE"

if [ $? -ne 0 ]; then
    echo "❌ Error: Failed to copy image"
    exit 1
fi

echo "✓ Working copy created"

echo ""
echo "🐳 Using Docker to inject driver into image..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Create a temporary directory for mounting
MOUNT_POINT="/mnt/pi_rootfs"

# Use Docker to mount and modify the image
docker run --rm --privileged \
    -v "$(dirname "$WORK_IMAGE"):/images" \
    -v "${MODULE_FILE}:/driver/adv7180.ko:ro" \
    --platform linux/amd64 \
    debian:bookworm \
    /bin/bash -c "
set -e

echo '📦 Installing required tools...'
apt-get update -qq
apt-get install -y -qq kpartx mount util-linux file kmod xz-utils

echo '🔍 Analyzing image partitions...'
IMAGE_FILE=\"/images/$(basename "$WORK_IMAGE")\"
echo \"Image: \$IMAGE_FILE\"

# Setup loop device
LOOP_DEV=\$(losetup -f)
losetup \"\$LOOP_DEV\" \"\$IMAGE_FILE\"
echo \"Loop device: \$LOOP_DEV\"

# Use kpartx to create partition mappings
echo '🗺️  Creating partition mappings...'
kpartx -av \"\$LOOP_DEV\"
sleep 2

# List available devices
ls -la /dev/mapper/

# Find the root partition (usually the second one, ext4)
# kpartx creates devices as /dev/mapper/loopXpY
LOOP_NAME=\$(basename \"\$LOOP_DEV\")
ROOT_PART=\"/dev/mapper/\${LOOP_NAME}p2\"

if [ ! -e \"\$ROOT_PART\" ]; then
    echo \"❌ Error: Root partition not found at \$ROOT_PART\"
    echo \"Available partitions:\"
    ls -la /dev/mapper/ | grep loop
    kpartx -d \"\$LOOP_DEV\"
    losetup -d \"\$LOOP_DEV\"
    exit 1
fi

echo \"Root partition: \$ROOT_PART\"

# Mount root partition
mkdir -p /mnt/pi_rootfs
mount \"\$ROOT_PART\" /mnt/pi_rootfs

echo ''
echo '🔍 Investigating image kernel structure...'

# Check what's in /lib/modules
echo '📋 Kernel versions in image:'
ls -la /mnt/pi_rootfs/lib/modules/ 2>&1 | head -20

# Find kernel version
KERNEL_VER=\$(ls /mnt/pi_rootfs/lib/modules/ 2>/dev/null | grep -v '.' | head -n 1)
if [ -z \"\$KERNEL_VER\" ]; then
    KERNEL_VER=\$(ls /mnt/pi_rootfs/lib/modules/ 2>/dev/null | head -n 1)
fi

echo ''
echo \"Detected kernel version: \$KERNEL_VER\"
echo ''
echo '📁 Kernel module directory structure:'
ls -la /mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/ 2>&1

echo ''
echo '🔍 Looking for adv7180 driver...'
DRIVER_PATH=\$(find /mnt/pi_rootfs/lib/modules -name 'adv7180.ko*' 2>/dev/null | head -n 1)

if [ -z \"\$DRIVER_PATH\" ]; then
    echo '⚠️  adv7180.ko not found in image'
    echo ''
    echo '📋 Checking media drivers structure:'
    
    # Check if drivers/media exists
    if [ -d \"/mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media\" ]; then
        echo '✓ Media drivers directory exists'
        ls -la /mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media/ 2>&1
        
        # Check for i2c subdirectory
        if [ -d \"/mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media/i2c\" ]; then
            echo ''
            echo '✓ Media I2C directory exists'
            echo 'Available media I2C drivers:'
            ls /mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media/i2c/ 2>&1 | head -20
            MEDIA_I2C_DIR=\"/mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media/i2c\"
        else
            echo ''
            echo '⚠️  Media I2C directory does not exist, will create it'
            MEDIA_I2C_DIR=\"/mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media/i2c\"
            mkdir -p \"\$MEDIA_I2C_DIR\"
        fi
    else
        echo '⚠️  Media drivers directory does not exist'
        echo 'Creating directory structure...'
        MEDIA_I2C_DIR=\"/mnt/pi_rootfs/lib/modules/\${KERNEL_VER}/kernel/drivers/media/i2c\"
        mkdir -p \"\$MEDIA_I2C_DIR\"
        echo \"✓ Created: \$MEDIA_I2C_DIR\"
    fi
    
    echo ''
    echo \"✓ Will install driver to: \$MEDIA_I2C_DIR\"
    DRIVER_PATH=\"\${MEDIA_I2C_DIR}/adv7180.ko\"
    DRIVER_IS_NEW=true
else
    echo \"✓ Found existing driver at: \$DRIVER_PATH\"
    DRIVER_IS_NEW=false
fi

echo ''
if [ \"\$DRIVER_IS_NEW\" = \"false\" ]; then
    # Backup original if it exists
    echo '💾 Backing up original driver...'
    cp \"\$DRIVER_PATH\" \"\${DRIVER_PATH}.original\"
    echo \"✓ Backup saved: \${DRIVER_PATH}.original\"
fi

# Install custom driver
if [ \"\$DRIVER_IS_NEW\" = \"true\" ]; then
    echo '📥 Installing NEW adv7180 driver module...'
else
    echo '📥 Replacing existing driver...'
fi

# Check if we need to compress the module
if [[ \"\$DRIVER_PATH\" == *.xz ]]; then
    echo '📦 Original was compressed (.ko.xz), compressing new driver...'
    cp /driver/adv7180.ko /tmp/adv7180.ko
    xz -9 /tmp/adv7180.ko
    cp /tmp/adv7180.ko.xz \"\$DRIVER_PATH\"
    chmod 644 \"\$DRIVER_PATH\"
    rm -f /tmp/adv7180.ko.xz
else
    cp /driver/adv7180.ko \"\$DRIVER_PATH\"
    chmod 644 \"\$DRIVER_PATH\"
fi

# Verify
NEW_SIZE=\$(du -h \"\$DRIVER_PATH\" | awk '{print \$1}')
echo \"✓ Driver installed (size: \$NEW_SIZE)\"

# Update module dependencies
MODULES_DIR=\$(dirname \$(dirname \$(dirname \"\$DRIVER_PATH\")))
KERNEL_VERSION=\$(basename \$MODULES_DIR)
echo \"🔄 Updating module dependencies for kernel \$KERNEL_VERSION...\"

# Use full path to depmod and check if it exists
if command -v depmod >/dev/null 2>&1; then
    depmod -a -b /mnt/pi_rootfs \$KERNEL_VERSION 2>&1 | head -20
    echo '✓ Module dependencies updated'
else
    echo '⚠️  depmod not available in container, skipping dependency update'
    echo '   (Dependencies will be auto-generated on first boot)'
fi

# Cleanup
echo '🧹 Unmounting...'
sync
umount /mnt/pi_rootfs
kpartx -d \"\$LOOP_DEV\"
losetup -d \"\$LOOP_DEV\"

echo '✅ Driver injection complete!'
"

if [ $? -ne 0 ]; then
    echo ""
    echo "❌ Error during driver injection"
    exit 1
fi

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  ✅ Driver Successfully Injected!                        ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "📁 Images:"
echo "   Original (unchanged): $IMAGE_PATH"
echo "   Modified (with driver): $WORK_IMAGE"
echo ""

# Offer to compress
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
read -p "Compress modified image for easier storage? (y/n): " COMPRESS_CHOICE
if [[ "$COMPRESS_CHOICE" =~ ^[Yy]$ ]]; then
    echo "⏳ Compressing image (this may take 10-30 minutes)..."
    COMPRESSED_IMAGE="${WORK_IMAGE}.xz"
    xz -9 -v "$WORK_IMAGE"
    echo "✓ Compressed to: ${WORK_IMAGE}.xz"
    echo "✓ Original uncompressed file removed to save space"
    WORK_IMAGE="$COMPRESSED_IMAGE"
fi

echo ""
echo "🎯 Next Steps:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1. Flash image to SD card using Raspberry Pi Imager:"
echo "   - Choose 'Use custom' and select: $WORK_IMAGE"
echo "   - Or use command line:"
echo "     xzcat $WORK_IMAGE | sudo dd of=/dev/rdiskX bs=4m"
echo ""
echo "2. Boot your PI4 with the new SD card"
echo ""
echo "3. Verify the custom driver is loaded:"
echo "   ssh pi@<pi-ip>"
echo "   lsmod | grep adv7180"
echo "   modinfo adv7180"
echo "   dmesg | grep adv7180"
echo ""
echo "✅ Your custom driver will be included in the image!"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

