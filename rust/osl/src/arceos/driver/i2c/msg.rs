use crate::VecDeque;
use crate::driver::i2c::{I2cMsgFlags, GeneralI2cMsg};

/// an I2C transaction segment beginning with START 
#[derive(Debug)]
pub struct I2cMsg{
    ///  Slave address, either 7 or 10 bits. When this is a 10 bit address,
    ///  I2C_M_TEN must be set in @flags and the adapter must support I2C_FUNC_10BIT_ADDR
    addr: u16,
    /// msg flags:
    flags: I2cMsgFlags,
    /// The buffer into which data is read, or from which it's written
    buf: VecDeque<u8>,
    /// Only used when msg is used to recive
    recieve_threshold: usize,
    /// Only used when msg is used to recieve,
    /// before recieve data, first need to send recieve cmd
    /// so this is used to record recieve data cmd cnt
    recieve_cmd_cnt: isize,
}

impl Default for I2cMsg {
    fn default() -> Self {
        Self {
            addr: 0,
            flags: I2cMsgFlags::empty(),
            buf: VecDeque::new(),
            write_idx: 0,
            read_idx: 0,
            read_cmd_cnt: 0,
        }
    }
}

unsafe impl Send for I2cMsg {}
unsafe impl Sync for I2cMsg {}

impl <const N: usize> GeneralI2cMsg for I2cMsg {
    /// Create a new I2cMsg with addr and data that need to transfer
    fn new_send(addr: u16, flags: I2cMsgFlags, data: [u8;N]) -> I2cMsg {
         assert!(!flags.contains(I2cMsgFlags::I2cMasterRead));
         I2cMsg { addr, flags, buf:VecDeque::from(data),recieve_cmd_cnt: 0, recieve_threshold: 0 }
    }

    /// Create a new I2cMsg with addr and an empty buf that want to recive
    fn new_recieve(addr: u16, flags: I2cMsgFlags, len: usize) -> I2cMsg {
         assert!(flags.contains(I2cMsgFlags::I2cMasterRead));
         I2cMsg { addr, flags, buf: VecDeque::with_capacity(len), recieve_threshold: len, 
             send_idx: 0, recieve_idx: 0,  recieve_cmd_cnt: 0}
    }

    /// Get msg copy flags
    pub fn flags(&self) -> I2cMsgFlags {
        self.flags
    }

    /// remove one flag
    pub fn remove_flag(&mut self, flag: I2cMsgFlags) {
        self.flags.remove(flag);
    }

    /// Get msg addr 
    pub fn addr(&self) -> u16{
        self.addr
    }

    /// modify recieve_cmd_cnt only flags contains I2cMasterRecvLen
    pub fn modify_recieve_cmd_cnt(&mut self, read_cmd_cnt: isize) {
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRecvLen));
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.recieve_cmd_cnt = recieve_cmd_cnt;
    }

    /// int recieve_cmd_cnt
    pub fn inc_recieve_cmd_cnt(&mut self) {
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.recieve_cmd_cnt +=1;
    }

    /// Check whether the buffer pointer has left last one
    pub  fn send_left_last(&self) -> bool {
        // MasterRead means msg can be write
        if self.flags.contains(I2cMsgFlags::I2cMasterRead) {
            self.recieve_cmd_cnt as usize == self.recieve_threshold -1
        } else {
            self.data.len() == 1
        }
    }

    /// Check whether the buffer pointer has reached the end
    pub  fn send_end(&self) -> bool {
        // MasterRead means msg can be write
        if self.flags.contains(I2cMsgFlags::I2cMasterRead) {
            self.recieve_cmd_cnt as usize == self.recieve_threshold
        } else {
            self.data.len() == 0
        }
    }

    /// Check whether the buffer pointer has reached the end
    pub  fn recieve_end(&self) -> bool {
        // MasterRead means msg can be write
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.data.len() == self.recieve_threshold
    }

    /// Write 1byte at the specified location
    pub  fn push_byte(&mut self, byte: u8) {
        // MasterRead means msg can be write
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));

        if self.recieve_end() {
            panic!("access buf overfllow");
        }
        self.data.push(byte);
    }

    /// Read 1byte from specified location
    pub  fn pop_front_byte(&mut self) -> u8 {
        // MasterRead means msg can be write, don't alow read 
        assert!(!self.flags.contains(I2cMsgFlags::I2cMasterRead));

        if self.send_end() {
            panic!("access buf overfllow");
        }
        self.data.pop_front()
    }

    /// modify msg buf len
    pub fn modify_recieve_threshold(&mut self, buf_len: usize) {
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRecvLen));
        self.buf_len = buf_len;
    }
}
