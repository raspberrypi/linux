//! To determine i2c what functionality is present
//!
//! From linux/include/uapi/linux/i2c.h

use bitflags::bitflags;

bitflags! {
    /// To determine what I2C functionality is present
    #[allow(non_camel_case_types)]
    #[repr(transparent)]
    #[derive(Debug, Copy, Clone, PartialEq, Eq)]
    pub struct I2cFuncFlags: u32 {
         /// Support I2C
         const I2C                   = 0x00000001;
         /// required for I2C_M_TEN
         const TEN_BIT_ADDR          = 0x00000002;
         /// required for I2C_M_IGNORE_NAK etc.
         const PROTOCOL_MANGLING     = 0x00000004;
         /// Support smbus_pec
         const SMBUS_PEC             = 0x00000008;
         /// Support Nostart
         const NOSTART               = 0x00000010;
         /// Support Slave
         const SLAVE                 = 0x00000020;
         /// Fill Doc
         const SMBUS_BLOCK_PROC_CALL = 0x00008000; /* SMBus 2.0 or later */
         /// Fill Doc
         const SMBUS_QUICK           = 0x00010000;
         /// Fill Doc
         const SMBUS_READ_BYTE       = 0x00020000;
         /// Fill Doc
         const SMBUS_WRITE_BYTE      = 0x00040000;
         /// Fill Doc
         const SMBUS_READ_BYTE_DATA  = 0x00080000;
         /// Fill Doc
         const SMBUS_WRITE_BYTE_DATA = 0x00100000;
         /// Fill Doc
         const SMBUS_READ_WORD_DATA  = 0x00200000;
         /// Fill Doc
         const SMBUS_WRITE_WORD_DATA = 0x00400000;
         /// Fill Doc
         const SMBUS_PROC_CALL       = 0x00800000;
         /// required for I2C_M_RECV_LEN
         const SMBUS_READ_BLOCK_DATA = 0x01000000;
         /// Fill Doc
         const SMBUS_WRITE_BLOCK_DATA = 0x02000000;
         /// I2C-like block xfer
         const SMBUS_READ_I2C_BLOCK  = 0x04000000;
         /// w/ 1-byte reg. addr.
         const SMBUS_WRITE_I2C_BLOCK = 0x08000000;
         /// SMBus 2.0 or later
         const SMBUS_HOST_NOTIFY     = 0x10000000;

         // Multi-bit flags

         /// Fill Doc
         const SMBUS_BYTE =  Self::SMBUS_READ_BYTE.bits() | Self::SMBUS_WRITE_BYTE.bits();
         /// Fill Doc
         const SMBUS_BYTE_DATA =  Self::SMBUS_READ_BYTE_DATA.bits() | Self::SMBUS_WRITE_BYTE_DATA.bits();
         /// Fill Doc
         const SMBUS_WORD_DATA = Self::SMBUS_READ_WORD_DATA.bits()| Self::SMBUS_WRITE_WORD_DATA.bits();
         /// Fill Doc
         const SMBUS_BLOCK_DATA = Self::SMBUS_READ_BLOCK_DATA.bits() | Self::SMBUS_WRITE_BLOCK_DATA.bits();
         /// Fill Doc
         const SMBUS_I2C_BLOCK = Self::SMBUS_READ_I2C_BLOCK.bits() | Self::SMBUS_WRITE_I2C_BLOCK.bits();
    }
}
