═══════════════════════════════════════════════════════════════════════════
  BUILD ONLY THE adv7180 MODULE - QUICK & SIMPLE
═══════════════════════════════════════════════════════════════════════════

🎯 YOU ONLY CHANGED: drivers/media/i2c/adv7180.c
   ➜ You don't need to rebuild the entire kernel!
   ➜ Just rebuild the driver module (much faster!)

═══════════════════════════════════════════════════════════════════════════

⚡ COMPARISON:

   Full Kernel Build:        Module Only Build:
   ━━━━━━━━━━━━━━━━━━━      ━━━━━━━━━━━━━━━━━━━
   ⏱️  20-60 minutes          ⏱️  1-2 minutes
   💾 ~500 MB output          💾 ~200 KB output
   📦 Entire kernel           📦 Just adv7180.ko
   
   Note: Out-of-tree build - symbol warnings are normal and safe!

═══════════════════════════════════════════════════════════════════════════

🚀 CHOOSE YOUR WORKFLOW:

   ┌─────────────────────────────────────────────────────────────────────┐
   │ OPTION A: Build & Inject into Image (RECOMMENDED for fresh images) │
   └─────────────────────────────────────────────────────────────────────┘
   
   Perfect for: Creating a custom image to flash with Raspberry Pi Imager
   
   Step 1: Build and inject (5-10 minutes)
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   cd build_tools
   ./build_and_inject.sh
   (It will ask for your image file path)
   
   Step 2: Flash to SD card
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Use Raspberry Pi Imager with the modified image
   
   DONE! ✅ Custom driver included in image!

   ┌─────────────────────────────────────────────────────────────────────┐
   │ OPTION B: Deploy to Running PI4 (for quick testing)                │
   └─────────────────────────────────────────────────────────────────────┘
   
   Perfect for: Testing changes on an already-running PI4
   
   Step 1: Build the module (1-5 minutes)
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   cd build_tools
   ./build_adv7180_module_only.sh

   Step 2: Deploy to PI4 (1 minute)
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   ./deploy_module_to_pi4.sh
   (It will ask for your PI4's IP address)

   DONE! ✅

═══════════════════════════════════════════════════════════════════════════

📋 MANUAL DEPLOYMENT (Alternative):

   1. Build module:
      ./build_adv7180_module_only.sh

   2. Copy to PI4:
      scp module_output/adv7180.ko pi@<pi-ip>:~/

   3. On PI4, run these commands:
      sudo rmmod adv7180
      MODULE_PATH=$(find /lib/modules/$(uname -r) -name adv7180.ko)
      sudo cp $MODULE_PATH ${MODULE_PATH}.backup
      sudo cp ~/adv7180.ko $MODULE_PATH
      sudo modprobe adv7180
      dmesg | tail -20

═══════════════════════════════════════════════════════════════════════════

⚠️  WHEN DO YOU NEED FULL KERNEL BUILD?

   You need full kernel build ONLY if:
   ✗ You changed core kernel code
   ✗ You changed kernel configuration
   ✗ You need a different kernel version
   ✗ You're setting up a PI4 from scratch

   You DON'T need full build if:
   ✓ You only changed a driver (like adv7180.c) ← THIS IS YOU!
   ✓ PI4 is already running with compatible kernel
   ✓ You just want to test driver changes

═══════════════════════════════════════════════════════════════════════════

🔍 VERIFY AFTER DEPLOYMENT:

   ssh pi@<pi-ip>
   
   # Check module is loaded
   lsmod | grep adv7180
   
   # Check version/info
   modinfo adv7180
   
   # Check kernel messages
   dmesg | grep adv7180

═══════════════════════════════════════════════════════════════════════════

💡 TIP: Start with module-only build! If it doesn't work, then try full kernel.

═══════════════════════════════════════════════════════════════════════════

