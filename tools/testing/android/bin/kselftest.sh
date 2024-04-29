#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

BAZEL=tools/bazel
BIN_DIR=common/tools/testing/android/bin
ACLOUD=$BIN_DIR/acloudb.sh
TRADEFED=prebuilts/tradefed/filegroups/tradefed/tradefed.sh
TESTSDIR=bazel-bin/common/

print_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "This script builds kernel, launches cvd and runs selftests on it."
    echo "Available options:"
    echo "  --skip-kernel-build   Skip the kernel building step"
    echo "  --skip-cvd-launch     Skip the CVD launch step"
    echo "  --skip-cvd-kill       Do not kill CVD launched by running this script"
    echo "  -d, --dist-dir=DIR    The kernel dist dir (default is /tmp/kernel_dist)"
    echo "  -s, --serial=SERIAL   The device serial number."
    echo "                        If serial is specified, cuttlefish device launch will be skipped"
    echo "  -t, --test=TEST_NAME  The test target name. Can be repeated"
    echo "                        If test is not specified, all kselftests will be run"
    echo "  -h, --help            Display this help message and exit"
    echo ""
    echo "Examples:"
    echo "$0"
    echo "$0 -t kselftest_size_test_get_size -t kselftest_binderfs_binderfs_test"
    echo "$0 -s 127.0.0.1:45549"
    echo ""
    exit 0
}

BUILD_KERNEL=true
LAUNCH_CVD=true
KILL_CVD=true
DIST_DIR=/tmp/kernel_dist
SERIAL_NUMBER=
MODULE_NAME="selftests"
TEST_FILTERS=
SELECTED_TESTS=

while test $# -gt 0; do
    case "$1" in
        -h|--help)
            print_help
            ;;
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
        -d)
            shift
            if test $# -gt 0; then
                DIST_DIR=$1
            else
                echo "kernel distribution directory is not specified"
                exit 1
            fi
            shift
            ;;
        --dist-dir*)
            DIST_DIR=$(echo $1 | sed -e "s/^[^=]*=//g")
            shift
            ;;
        -s)
            shift
            if test $# -gt 0; then
                SERIAL_NUMBER=$1
                BUILD_KERNEL=false
                LAUNCH_CVD=false
                KILL_CVD=false
            else
                echo "device serial is not specified"
                exit 1
            fi
            shift
            ;;
        --serial*)
            BUILD_KERNEL=false
            LAUNCH_CVD=false
            KILL_CVD=false
            SERIAL_NUMBER=$(echo $1 | sed -e "s/^[^=]*=//g")
            shift
            ;;
        -t)
            shift
            if test $# -gt 0; then
                TEST_NAME=$1
                SELECTED_TESTS+="$TEST_NAME "
                TEST_FILTERS+="--include-filter '$MODULE_NAME $TEST_NAME' "
            else
                echo "test name is not specified"
                exit 1
            fi
            shift
            ;;
        --test*)
            TEST_NAME=$(echo $1 | sed -e "s/^[^=]*=//g")
            SELECTED_TESTS+="$TEST_NAME "
            TEST_FILTERS+="--include-filter '$MODULE_NAME $TEST_NAME'"
            shift
            ;;
        *)
            ;;
    esac
done

if $BUILD_KERNEL; then
    echo "Building kernel..."
    # TODO: add support to build kernel for physical device
    $BAZEL run //common-modules/virtual-device:virtual_device_x86_64_dist --  --dist_dir=$DIST_DIR
fi

if $LAUNCH_CVD; then
    echo "Launching cvd..."
    CVD_OUT=$($ACLOUD create --local-kernel-image $DIST_DIR)
    echo $CVD_OUT
    INSTANCE_NAME=$(echo "$CVD_OUT" | grep -o "ins-[^\[]*")
    SERIAL_STRING=$(echo "$CVD_OUT" | grep -oE 'device serial: ([0-9]+\.){3}[0-9]+:[0-9]+')
    SERIAL_NUMBER=$(echo "$SERIAL_STRING" | sed 's/device serial: //')
    echo "acloud launched device $SERIAL_NUMBER with instance $INSTANCE_NAME"
fi

if [ -z "$SERIAL_NUMBER" ]; then
    echo "Device serial is not provided by acloud or by command line flag -s|--serial flag"
    exit 1
else
    echo "Test with device: $SERIAL_NUMBER"
fi

echo "Get abi from device $SERIAL_NUMBER"
ABI=$(adb -s $SERIAL_NUMBER shell getprop ro.product.cpu.abi)
echo "Building kselftests according to device $SERIAL_NUMBER ro.product.cpu.abi $ABI ..."
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

if [ -z "$SELECTED_TESTS" ]; then
    echo "Running all kselftests with device $SERIAL_NUMBER..."
    TEST_FILTERS="--include-filter $MODULE_NAME"
else
    echo "Running $SELECTED_TESTS with device $SERIAL_NUMBER ..."
fi

tf_cli="$TRADEFED run commandAndExit template/local_min \
--template:map test=suite/test_mapping_suite \
$TEST_FILTERS --tests-dir=$TESTSDIR --primary-abi-only -s $SERIAL_NUMBER"

echo "Runing tradefed command: $tf_cli"

eval $tf_cli

if $LAUNCH_CVD && $KILL_CVD; then
    echo "Test finished. Deleting cvd instance $INSTANCE_NAME ..."
    $ACLOUD delete --instance-names $INSTANCE_NAME
fi
