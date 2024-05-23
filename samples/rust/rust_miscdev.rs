// SPDX-License-Identifier: GPL-2.0

//!
//! How to build only modules:
//! make LLVM=-17 O=build_4b ARCH=arm64 M=samples/rust
//!
//! How to use in qemu:
//! / # sudo insmod rust_miscdev.ko
//! / # sudo cat /proc/misc  -> c 10 122
//! / # sudo chmod 777 /dev/rust_misc
//! / # sudo echo "hello" > /dev/rust_misc
//! / # sudo cat /dev/rust_misc  -> Hello
//! 

use kernel::prelude::*;
use kernel::{
    file::{self, File},
    io_buffer::{IoBufferReader, IoBufferWriter},
    sync::{Arc, ArcBorrow},
    sync::Mutex,
    miscdev, 
    pin_init,
    new_mutex,
    fmt,
};

module! {
    type: RustMiscDev,
    name: "rust_miscdev",
    author: "i dont konw",
    description: "Rust exercise 002",
    license: "GPL",
}

const GLOBALMEM_SIZE: usize = 0x1000;

#[pin_data]
struct RustMiscdevData {
    #[pin]
    inner: Mutex<[u8;GLOBALMEM_SIZE]>,
}

impl RustMiscdevData {
    fn try_new() -> Result<Arc<Self>>{
        pr_info!("rust miscdevice created\n");
        Ok(Arc::pin_init(
            pin_init!(Self {
                inner <- new_mutex!([0u8;GLOBALMEM_SIZE])
            })
        )?)
    }
}

unsafe impl Sync for RustMiscdevData {}
unsafe impl Send for RustMiscdevData {}

// unit struct for file operations
struct RustFile;

#[vtable]
impl file::Operations for RustFile {
    type Data = Arc<RustMiscdevData>;
    type OpenData = Arc<RustMiscdevData>;

    fn open(_shared: &Arc<RustMiscdevData>, _file: &file::File) -> Result<Self::Data> {
        pr_info!("open in miscdevice\n",);
        //TODO
        todo!()
    }

    fn read(
        _shared: ArcBorrow<'_, RustMiscdevData>,
        _file: &File,
        _writer: &mut impl IoBufferWriter,
        _offset: u64,
    ) -> Result<usize> {
        pr_info!("read in miscdevice\n");
        //TODO
        todo!()
        
    }

    fn write(
        _shared: ArcBorrow<'_, RustMiscdevData>,
        _file: &File,
        _reader: &mut impl IoBufferReader,
        _offset: u64,
    ) -> Result<usize> {
        pr_info!("write in miscdevice\n");
        //TODO
        todo!()

    }

    fn release(_data: Self::Data, _file: &File) {
        pr_info!("release in miscdevice\n");
    }
}

struct RustMiscDev {
    _dev: Pin<Box<miscdev::Registration<RustFile>>>,
}

impl kernel::Module for RustMiscDev {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust miscdevice device sample (init)\n");

        let data: Arc<RustMiscdevData> = RustMiscdevData::try_new()?;

        let misc_reg = miscdev::Registration::new_pinned(fmt!("rust_misc"), data)?;

        Ok(RustMiscDev { _dev: misc_reg })
    }
}

impl Drop for RustMiscDev {
    fn drop(&mut self) {
        pr_info!("Rust miscdevice device sample (exit)\n");
    }
}