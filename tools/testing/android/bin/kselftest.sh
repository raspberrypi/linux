#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

BAZEL=tools/bazel
BIN_DIR=common/tools/testing/android/bin
ACLOUD=$BIN_DIR/acloudb.sh
TRADEFED=prebuilts/tradefed/filegroups/tradefed/tradefed.sh
TESTSDIR=bazel-bin/common/testcases

print_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "This script builds kernel, launches cvd and runs selftests on it."
    echo "Available options:"
    echo "  --skip-kernel-build    Skip the kernel building step"
    echo "  --skip-cvd-launch      Skip the CVD launch step"
    echo "  --skip-cvd-kill        Do not kill CVD launched by running this script"
    echo "  --dist-dir             The kernel dist dir (default is /tmp/kernel_dist)"
    echo "  --help                 Display this help message and exit"
    echo ""
    exit 0
}

BUILD_KERNEL=true
LAUNCH_CVD=true
KILL_CVD=true
DIST_DIR=/tmp/kernel_dist

for arg in "$@"; do
    case $arg in
        --skip-kernel-build)
            BUILD_KERNEL=false
            shift
            ;;
        --skip-cvd-launch)
            LAUNCH_CVD=false
            shift
            ;;
        --skip-cvd-kill)
            KILL_CVD=false
            shift
            ;;
        --dist-dir)
            DIST_DIR="${arg#*=}"
            shift
            ;;
        --help)
            print_help
            ;;
        *)
            ;;
    esac
done

if $BUILD_KERNEL; then
    echo "Building kernel..."
    $BAZEL run //common-modules/virtual-device:virtual_device_x86_64_dist --  --dist_dir=$DIST_DIR
fi

if $LAUNCH_CVD; then
    echo "Launching cvd..."
    CVD_OUT=$($ACLOUD create --local-kernel-image $DIST_DIR)
    echo $CVD_OUT
    INSTANCE_NAME=$(echo $CVD_OUT|grep -o "ins-[^\[]*")
fi

echo "Building selftests..."
ABI=$(adb shell getprop ro.product.cpu.abi)
case $ABI in
	arm64*)
		$BAZEL build //common:kselftest_tests_arm64
		;;
	x86_64*)
		$BAZEL build //common:kselftest_tests_x86_64
		;;
	*)
		echo "$ABI not supported"
		exit 1
		;;
esac

$TRADEFED run commandAndExit template/local_min --template:map test=suite/test_mapping_suite \
--include-filter selftests --tests-dir=$TESTSDIR --primary-abi-only

if $LAUNCH_CVD && $KILL_CVD; then
    echo "Test finished. Deleting cvd..."
    $ACLOUD delete --instance-names $INSTANCE_NAME
fi
