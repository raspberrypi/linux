// SPDX-License-Identifier: GPL-2.0

//! Delay functions for operations like sleeping.
//!
//! C header: [`include/linux/delay.h`](../../../../include/linux/delay.h)

use crate::bindings;
use core::{cmp::min, time::Duration};

const MILLIS_PER_SEC: u64 = 1_000;

fn coarse_sleep_conversion(duration: Duration) -> core::ffi::c_uint {
    let milli_as_nanos = Duration::from_millis(1).subsec_nanos();

    // Rounds the nanosecond component of `duration` up to the nearest millisecond.
    let nanos_as_millis = duration.subsec_nanos().wrapping_add(milli_as_nanos - 1) / milli_as_nanos;

    // Saturates the second component of `duration` to `c_uint::MAX`.
    let seconds_as_millis = min(
        duration.as_secs().saturating_mul(MILLIS_PER_SEC),
        u64::from(core::ffi::c_uint::MAX),
    ) as core::ffi::c_uint;

    seconds_as_millis.saturating_add(nanos_as_millis)
}

/// usleep_range_state - Sleep for an approximate time in a given state
/// @min:        Minimum time in usecs to sleep
/// @max:        Maximum time in usecs to sleep
pub fn usleep_range(min: u64, max: u64) {
    // SAFETY: call ffi
    unsafe{bindings::usleep_range_state(min, max, bindings::TASK_UNINTERRUPTIBLE)}
}

/// Sleeps safely even with waitqueue interruptions.
///
/// This function forwards the call to the C side `msleep` function. As a result,
/// `duration` will be rounded up to the nearest millisecond if granularity less
/// than a millisecond is provided. Any [`Duration`] that exceeds
/// [`c_uint::MAX`][core::ffi::c_uint::MAX] in milliseconds is saturated.
///
/// # Examples
///
// Keep these in sync with `test_coarse_sleep_examples`.
/// ```
/// # use core::time::Duration;
/// # use kernel::delay::coarse_sleep;
/// coarse_sleep(Duration::ZERO);                   // Equivalent to `msleep(0)`.
/// coarse_sleep(Duration::from_nanos(1));          // Equivalent to `msleep(1)`.
///
/// coarse_sleep(Duration::from_nanos(1_000_000));  // Equivalent to `msleep(1)`.
/// coarse_sleep(Duration::from_nanos(1_000_001));  // Equivalent to `msleep(2)`.
/// coarse_sleep(Duration::from_nanos(1_999_999));  // Equivalent to `msleep(2)`.
///
/// coarse_sleep(Duration::from_millis(1));         // Equivalent to `msleep(1)`.
/// coarse_sleep(Duration::from_millis(2));         // Equivalent to `msleep(2)`.
///
/// coarse_sleep(Duration::from_secs(1));           // Equivalent to `msleep(1000)`.
/// coarse_sleep(Duration::new(1, 1));              // Equivalent to `msleep(1001)`.
/// coarse_sleep(Duration::new(1, 2));              // Equivalent to `msleep(1001)`.
/// ```
pub fn coarse_sleep(duration: Duration) {
    // SAFETY: msleep is safe for all values of an `unsigned int`.
    unsafe { bindings::msleep(coarse_sleep_conversion(duration)) }
}

#[cfg(test)]
mod tests {
    use super::{coarse_sleep_conversion, MILLIS_PER_SEC};
    use core::time::Duration;

    #[test]
    fn test_coarse_sleep_examples() {
        // Keep these in sync with `coarse_sleep`'s `# Examples` section.

        assert_eq!(coarse_sleep_conversion(Duration::ZERO), 0);
        assert_eq!(coarse_sleep_conversion(Duration::from_nanos(1)), 1);

        assert_eq!(coarse_sleep_conversion(Duration::from_nanos(1_000_000)), 1);
        assert_eq!(coarse_sleep_conversion(Duration::from_nanos(1_000_001)), 2);
        assert_eq!(coarse_sleep_conversion(Duration::from_nanos(1_999_999)), 2);

        assert_eq!(coarse_sleep_conversion(Duration::from_millis(1)), 1);
        assert_eq!(coarse_sleep_conversion(Duration::from_millis(2)), 2);

        assert_eq!(coarse_sleep_conversion(Duration::from_secs(1)), 1000);
        assert_eq!(coarse_sleep_conversion(Duration::new(1, 1)), 1001);
        assert_eq!(coarse_sleep_conversion(Duration::new(1, 2)), 1001);
    }

    #[test]
    fn test_coarse_sleep_saturation() {
        assert!(
            coarse_sleep_conversion(Duration::new(
                core::ffi::c_uint::MAX as u64 / MILLIS_PER_SEC,
                0
            )) < core::ffi::c_uint::MAX
        );
        assert_eq!(
            coarse_sleep_conversion(Duration::new(
                core::ffi::c_uint::MAX as u64 / MILLIS_PER_SEC,
                999_999_999
            )),
            core::ffi::c_uint::MAX
        );

        assert_eq!(
            coarse_sleep_conversion(Duration::MAX),
            core::ffi::c_uint::MAX
        );
    }
}
