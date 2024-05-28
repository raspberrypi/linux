//! I2c module
//!
//! Compatible with different hardware platforms
//! Include:
//! timing: I2C timing config
//! functionality: Describe I2c functionality, Compatible with Linux
//! msg: Describe I2cMsg
//!

/// i2c operation mode
#[derive(Debug)]
pub enum I2cMode {
    /// Master Mode.
    ///
    ///A master in an I2C system and programmed only as a Master
    Master = 0,
    /// Slave Mode
    ///
    ///A slave in an I2C system and programmed only as a Slave
    Slave = 1,
}

/// i2c func
mod functionality;
/// i2c msg
mod msg;
/// i2c timing
mod timing;

/// As specified in SMBus standard
pub const  I2C_SMBUS_BLOCK_MAX: u8 = 32;

pub use self::{functionality::*, timing::*, msg::*};
