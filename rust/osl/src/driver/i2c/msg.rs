//! I2C msg abstraction
//!
//! Every OS should provide I2cMsg type and implement GeneralI2cMsg
//!

use bitflags::bitflags;

bitflags! {
    /// I2c msgs flags
    #[repr(transparent)]
    #[derive(Debug, Copy, Clone, PartialEq, Eq)]
    pub struct I2cMsgFlags: u16 {
        /// read data (from slave to master). Guaranteed to be 0x0001
        const I2cMasterRead = 0x0001;
        /// Use Packet Error Checking
        const I2cClientPec = 0x0004;
        /// this is a 10 bit chip address
        /// Only if I2C_FUNC_10BIT_ADDR is set
        const I2cAddrTen =  0x0010;
        /// we are the slave
        const I2cClientSlave = 0x0020;
        /// We want to use I2C host notify
        const I2cClientHostNotify = 0x0040;
        /// for board_info; true if can wake
        const I2cClientWake = 0x0080;
        /// Linux kernel
        const I2cMasterDmaSafe = 0x0200;
        /// message length will be first received byte
        /// Only if I2C_FUNC_SMBUS_READ_BLOCK_DATA is set
        const I2cMasterRecvLen = 0x0400;
        /// in a read message, master ACK/NACK bit is skipped
        const I2cMasterNoReadAck = 0x0800;
        /// treat NACK from client as ACK
        const I2cMasterIgnNak = 0x1000;
        /// toggles the Rd/Wr bit
        const I2cMasterRevDir = 0x2000;
        /// skip repeated start sequence
        const I2cMasterNoStart = 0x4000;
        /// force a STOP condition after the message
        const I2cMasterStop = 0x8000;

         // Multi-bit flags
        /// Use Omnivision SCCB protocol Must match I2C_M_STOP|IGNORE_NAK
        const I2cClientSccb = Self::I2cMasterStop.bits() | Self::I2cMasterIgnNak.bits();
    }
}

/// GeneralI2cMsg that Type I2cMsg must implement 
pub trait GeneralI2cMsg:Default+Sync+Send {
    /// Create a new I2cMsg with addr and data that need to transfer
    fn new_send<const N: usize>(addr: u16, flags: I2cMsgFlags, data: [u8;N]) -> Self;
    /// Create a new I2cMsg with addr and an empty buf that want to recive
    fn new_recieve(addr: u16, flags: I2cMsgFlags, len: usize) -> Self;
    /// Get msg addr 
    fn addr(&self) -> u16;
    /// Get msg copy flags
    fn flags(&self) -> I2cMsgFlags;
    /// int recieve cmd cnt
    fn inc_recieve_cmd_cnt(&mut self);
    /// Check whether the send msg is left last
    fn send_left_last(&self) -> bool;
    /// Check whether the send msg is end
    fn send_end(&self) -> bool;
    /// Check whether the recieve is end
    fn recieve_end(&self) -> bool ;
    /// Write 1byte to recieve msg
    fn push_byte(&mut self, byte: u8);
    /// Read 1byte from send msg front
    fn pop_front_byte(&mut self) -> u8;

    /// modify msg buf len, only used when flags contains I2cMasterRecvLen 
    fn modify_recieve_threshold(&mut self, buf_len: usize);
    /// modify recieve_cmd_cnt only flags contains I2cMasterRecvLen
    fn modify_recieve_cmd_cnt(&mut self, read_cmd_cnt: isize);
    /// remove one flag
    fn remove_flag(&mut self, flag: I2cMsgFlags);
}

#[cfg(feature = "linux")]
pub use crate::linux::driver::i2c::*;
#[cfg(feature = "arceos")]
pub use crate::arceos::driver::i2c::*;
