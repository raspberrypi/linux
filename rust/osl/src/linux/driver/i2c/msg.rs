use crate::driver::i2c::{I2cMsgFlags, GeneralI2cMsg};

/// an I2C transaction segment beginning with START 
#[derive(Debug) ]
#[allow(dead_code)]
pub struct I2cMsg{
    ///  Slave address, either 7 or 10 bits. When this is a 10 bit address,
    ///  I2C_M_TEN must be set in @flags and the adapter must support I2C_FUNC_10BIT_ADDR
    addr: u16,
    /// msg flags:
    flags: I2cMsgFlags,
    /// The buffer into which data is read, or from which it's written
    buf: *mut u8,
    /// The buffer length
    buf_len: usize,
    /// record current recieve idx
    recieve_idx: isize,
    /// record current send idx
    send_idx: isize,
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
            buf: core::ptr::null_mut(),
            buf_len: 0,
            send_idx: 0,
            recieve_idx: 0,
            recieve_cmd_cnt: 0,
        }
    }
}

unsafe impl Send for I2cMsg {}
unsafe impl Sync for I2cMsg {}

impl I2cMsg {
    /// Create a new I2cMsg from linux
    pub fn new_raw(addr: u16, flags: I2cMsgFlags, buf:  *mut u8, buf_len: usize) -> Self {
         I2cMsg { addr, flags, buf, buf_len, recieve_idx: 0, send_idx: 0,  recieve_cmd_cnt: 0}
    }
}


impl GeneralI2cMsg for I2cMsg {
    /// Create a new I2cMsg with addr and data that need to transfer
    fn new_send<const N: usize>(_addr: u16, _flags: I2cMsgFlags, _data: [u8;N]) -> Self {
        unimplemented!("Linux now use binding:i2c_msg");
    }

    /// Create a new I2cMsg with addr and an empty buf that want to recive
    fn new_recieve(_addr: u16, _flags: I2cMsgFlags, _len: usize) -> Self {
        unimplemented!("Linux now use binding:i2c_msg");
    }

    /// Get msg addr 
    fn addr(&self) -> u16 {
        self.addr
    }

    /// Get msg copy flags
    fn flags(&self) -> I2cMsgFlags {
        self.flags
    }

    /// inc recieve_cmd_cnt
    fn inc_recieve_cmd_cnt(&mut self) {
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.recieve_cmd_cnt +=1;
    }

    /// Check whether the send msg is left last
    fn send_left_last(&self) -> bool {
        // MasterRead means msg can be write
        if self.flags.contains(I2cMsgFlags::I2cMasterRead) {
            self.recieve_cmd_cnt as usize == self.buf_len -1
        } else {
            self.send_idx as usize == self.buf_len - 1
        }
    }

    /// Check whether the send msg is end
    fn send_end(&self) -> bool {
        // MasterRead means msg can be write
        if self.flags.contains(I2cMsgFlags::I2cMasterRead) {
            self.recieve_cmd_cnt as usize == self.buf_len
        } else {
            self.send_idx as usize == self.buf_len
        }
    }

    /// Check whether the recieve is end
    fn recieve_end(&self) -> bool {
        // MasterRead means msg can be write
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.recieve_idx as usize == self.buf_len
    }

    /// Write 1byte to recieve msg
    fn push_byte(&mut self, byte: u8) {
        // MasterRead means msg can be write
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));

        if self.recieve_end() {
            panic!("access buf overfllow");
        }
        unsafe{*self.buf.offset(self.recieve_idx) = byte};
        self.recieve_idx +=1;
    }

    /// Read 1byte from send msg front
    fn pop_front_byte(&mut self) -> u8 {
        // MasterRead means msg can be write, don't alow read 
        assert!(!self.flags.contains(I2cMsgFlags::I2cMasterRead));

        if self.send_end() {
            panic!("access buf overfllow");
        }

        let byte = unsafe{*self.buf.offset(self.send_idx)};
        self.send_idx +=1;
        byte
    }

    /// modify msg buf len, only used when flags contains I2cMasterRecvLen 
    fn modify_recieve_threshold(&mut self, buf_len: usize) {
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRecvLen));
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.buf_len = buf_len;
    }

    /// modify recieve_cmd_cnt only flags contains I2cMasterRecvLen
    fn modify_recieve_cmd_cnt(&mut self, read_cmd_cnt: isize) {
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRecvLen));
        assert!(self.flags.contains(I2cMsgFlags::I2cMasterRead));
        self.recieve_cmd_cnt = read_cmd_cnt;
    }

    /// remove one flag
    fn remove_flag(&mut self, flag: I2cMsgFlags) {
        self.flags.remove(flag);
    }
}
