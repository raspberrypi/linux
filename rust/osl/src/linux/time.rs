use kernel::timekeeping::*;

use crate::time::NSEC_PER_USEC;

/// Linux ktime add us
pub fn time_add_us(us: u64) -> u64 {
    ktime_get() + us * NSEC_PER_USEC
}

/// Linux ktime add us
pub fn current_time() -> u64 {
    ktime_get()
}
