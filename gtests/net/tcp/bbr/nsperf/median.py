#!/usr/bin/python
#
# Read a file with one float per line, and print the median of all numbers.
# Usage:
#    infile=numbers.txt ./median.py

import os
import sys

def read_file():
    """Read a file with one float per line, and return as a list of floats."""

    nums = []

    path = os.environ['infile']
    f = open(path)

    # Read a line, or EOF.
    line = f.readline()
    while True:
        if not line:
            return nums
        num_str = line.strip()
        num = float(num_str)
        nums.append(num)
        line = f.readline()

def median(nums):
    """Return median of all numbers."""

    sorted_nums = sorted(nums)
    n = len(sorted_nums)
    m = n - 1
    return (sorted_nums[n/2] + sorted_nums[m/2]) / 2.0

def main():
    """Main function to run everything."""
    nums = read_file()
    print('%s' % median(nums))
    return 0

if __name__ == '__main__':
    sys.exit(main())
