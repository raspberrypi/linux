// SPDX-License-Identifier: GPL-2.0

//! Rust file system sample.

use kernel::prelude::*;
use kernel::{c_str, fs};

module! {
    type: FsModule,
    name: "rust_fs",
    author: "Rust for Linux Contributors",
    license: "GPL",
}

struct RustFs;
impl fs::Type for RustFs {
    const NAME: &'static CStr = c_str!("rustfs");
    const FLAGS: i32 = fs::flags::USERNS_MOUNT;
}

struct FsModule {
    _fs: Pin<Box<fs::Registration>>,
}

impl kernel::Module for FsModule {
    fn init(module: &'static ThisModule) -> Result<Self> {
        let mut reg = Pin::from(Box::try_new(fs::Registration::new())?);
        reg.as_mut().register::<RustFs>(module)?;
        Ok(Self { _fs: reg })
    }
}
