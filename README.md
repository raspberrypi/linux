# 1. Build Setup

## - config 생성하기
```bash
KERNEL=kernel8
make LLVM=1 ARCH=arm64 O=build/ bcm2711_defconfig
```

## - config 수정
```bash
make LLVM=1 ARCH=arm64 O=build/ menuconfig
```

- Kernel Features -> [ ] Randomize the address of the kernel image
- Kernel hacking -> Compile-time checks and compiler options
                 -> Debug information
                    (Rely on the toolchain's impilicit default DWARF version)
- Kernel hacking -> Compile-time checks and compiler options ->
                    [*] Provide GDB scripts for kernel debugging

# 2. Build
```bash
make LLVM=1 ARCH=arm64 O=build/ -j$(nproc) Image modules dtbs
```

# 3. DB 생성
```bash
make LLVM=1 ARCH=arm64 O=build/ tags cscope
./scripts/clang-tools/gen_compile_commands.py -d build/
```
