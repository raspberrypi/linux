//! Some math funcs
//!

/// PETA
pub const PETA: u64 = 1000000000000000;
/// TERA
pub const TERA: u64 = 1000000000000;
/// GIGA
pub const GIGA: u32 = 1000000000;
/// MEGA
pub const MEGA: u32 = 1000000;
/// KILO
pub const KILO: u32 = 1000;
/// HECTO
pub const HECTO: u32 = 100;
/// DECA
pub const DECA: u32 = 10;
/// DECI
pub const DECI: u32 = 10;
/// CENTI
pub const CENTI: u32 = 100;
/// MILLI
pub const MILLI: u32 = 1000;
/// MICRO
pub const MICRO: u32 = 1000000;
/// NANO
pub const NANO: u32 = 1000000000;
/// PICO
pub const PICO: u64 = 1000000000000;
/// FEMTO
pub const FEMTO: u64 = 1000000000000000;

/// Function to perform division with rounding to the closest integer
pub fn div_round_closest_ull(x: u64, divisor: u32) -> u64 {
    let tmp = x + (divisor as u64 / 2);
    do_div(tmp, divisor)
}

// Function to perform division and return the quotient
fn do_div(n: u64, base: u32) -> u64 {
    if base == 0 {
        panic!("Division by zero");
    }

    if base.is_power_of_two() {
        return n >> base.trailing_zeros();
    }

    // Performing division
    let quotient = n / base as u64;
    quotient
}
