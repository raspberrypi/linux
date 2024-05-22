// SPDX-License-Identifier: GPL-2.0

//! Rust character device sample.

use kernel::{prelude::*,
             chrdev,
             file,
             sync::Mutex,
             sync::{Arc,ArcBorrow},
             io_buffer::{IoBufferReader, IoBufferWriter},
             c_str,
             new_mutex,
};

const GLOBALMEM_SIZE: usize = 0x1000;

module! {
    type: RustChrdev,
    name: "rust_chrdev",
    author: "Rust for Linux Contributors",
    description: "Rust character device sample",
    license: "GPL",
}

#[pin_data]
struct RustFile {
    #[pin]
    inner: Mutex<[u8;GLOBALMEM_SIZE]>,
}

impl RustFile {
    fn try_new() -> Result<Arc<Self>>{
        Ok(Arc::pin_init(
            pin_init!(Self {inner <- new_mutex!([0u8;GLOBALMEM_SIZE]),})
        )?)
    }
}

#[vtable]
impl file::Operations for RustFile {
    type Data = Arc<Self>;

    fn open(_shared: &(), _file: &file::File) -> Result<Self::Data> {
        pr_info!("open in chrdev");
        RustFile::try_new()
    }

    fn write(
        _this: ArcBorrow<'_, Self>,
        _file: &file::File,
        _reader: &mut impl IoBufferReader,
        _offset:u64,
    ) -> Result<usize> {
        pr_info!("write in chrdev");
        Err(EPERM)
    }

    fn read(
        _this: ArcBorrow<'_, Self>,
        _file: &file::File,
        _writer: &mut impl IoBufferWriter,
        _offset:u64,
    ) -> Result<usize> {
        pr_info!("read in chrdev");
        Err(EPERM)
    }
}

struct RustChrdev {
    _dev: Pin<Box<chrdev::Registration<2>>>,
}

impl kernel::Module for RustChrdev {
    fn init(module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust character device sample (init)\n");

        let mut chrdev_reg = chrdev::Registration::new_pinned(c_str!("my_chrdev"), 0, module)?;

        // Register the same kind of device twice, we're just demonstrating
        // that you can use multiple minors. There are two minors in this case
        // because its type is `chrdev::Registration<2>`
        chrdev_reg.as_mut().register::<RustFile>()?;
        chrdev_reg.as_mut().register::<RustFile>()?;

        Ok(RustChrdev { _dev: chrdev_reg })
    }
}

impl Drop for RustChrdev {
    fn drop(&mut self) {
        pr_info!("Rust character device sample (exit)\n");
    }
}