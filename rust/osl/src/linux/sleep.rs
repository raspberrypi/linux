use kernel::delay::usleep_range;

/// Linux usleep
pub fn usleep(us: u64) {
    usleep_range(us >> 2 + 1, us);
}
