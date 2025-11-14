#!/bin/bash

# Verify that your changes are in the driver source

KERNEL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER_SOURCE="${KERNEL_DIR}/drivers/media/i2c/adv7180.c"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  Verify adv7180.c Changes                                ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Check if source file exists
if [ ! -f "$DRIVER_SOURCE" ]; then
    echo "❌ Error: Source file not found: $DRIVER_SOURCE"
    exit 1
fi

echo "✓ Source file: $DRIVER_SOURCE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Show file info
echo "📋 File Information:"
echo "  Last modified: $(stat -f "%Sm" "$DRIVER_SOURCE" 2>/dev/null || stat -c "%y" "$DRIVER_SOURCE")"
echo "  Size: $(du -h "$DRIVER_SOURCE" | awk '{print $1}')"
echo "  Lines: $(wc -l < "$DRIVER_SOURCE")"
echo ""

# Check git status
if git -C "$KERNEL_DIR" rev-parse --git-dir > /dev/null 2>&1; then
    echo "📊 Git Status:"
    if git -C "$KERNEL_DIR" diff --quiet "$DRIVER_SOURCE"; then
        echo "  ⚠️  No uncommitted changes detected"
    else
        echo "  ✓ File has uncommitted changes"
        echo ""
        echo "📝 Recent changes (last 10 lines):"
        git -C "$KERNEL_DIR" diff "$DRIVER_SOURCE" | tail -20
    fi
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "💡 Tips to ensure your changes are included:"
echo ""
echo "1. Add a custom log message to verify:"
echo "   In adv7180.c, add:"
echo "   dev_info(&client->dev, \"Custom ADV7180 driver loaded - YOUR_MARKER\");"
echo ""
echo "2. Check on PI4 after loading:"
echo "   dmesg | grep -i \"YOUR_MARKER\""
echo ""
echo "3. Compare module checksums:"
echo "   Before: md5sum /lib/modules/.../adv7180.ko"
echo "   After:  md5sum ~/adv7180.ko"
echo ""
echo "4. Check module build date:"
echo "   modinfo adv7180.ko | grep vermagic"
echo ""

