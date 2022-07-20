// SPDX-License-Identifier: GPL-2.0

//! Rust file system sample.

use kernel::prelude::*;
use kernel::{c_str, fs};

module_fs! {
    type: RustFs,
    name: "rust_fs",
    author: "Rust for Linux Contributors",
    license: "GPL",
}

struct RustFs;

#[vtable]
impl fs::Context<Self> for RustFs {
    type Data = ();

    kernel::define_fs_params! {(),
        {flag, "flag", |_, v| { pr_info!("flag passed-in: {v}\n"); Ok(()) } },
        {flag_no, "flagno", |_, v| { pr_info!("flagno passed-in: {v}\n"); Ok(()) } },
        {bool, "bool", |_, v| { pr_info!("bool passed-in: {v}\n"); Ok(()) } },
        {u32, "u32", |_, v| { pr_info!("u32 passed-in: {v}\n"); Ok(()) } },
        {u32oct, "u32oct", |_, v| { pr_info!("u32oct passed-in: {v}\n"); Ok(()) } },
        {u32hex, "u32hex", |_, v| { pr_info!("u32hex passed-in: {v}\n"); Ok(()) } },
        {s32, "s32", |_, v| { pr_info!("s32 passed-in: {v}\n"); Ok(()) } },
        {u64, "u64", |_, v| { pr_info!("u64 passed-in: {v}\n"); Ok(()) } },
        {string, "string", |_, v| { pr_info!("string passed-in: {v}\n"); Ok(()) } },
        {enum, "enum", [("first", 10), ("second", 20)], |_, v| {
            pr_info!("enum passed-in: {v}\n"); Ok(()) }
        },
    }

    fn try_new() -> Result {
        pr_info!("context created!\n");
        Ok(())
    }
}

impl fs::Type for RustFs {
    type Context = Self;
    const SUPER_TYPE: fs::Super = fs::Super::Independent;
    const NAME: &'static CStr = c_str!("rustfs");
    const FLAGS: i32 = fs::flags::USERNS_MOUNT;

    fn fill_super(_data: (), sb: fs::NewSuperBlock<'_, Self>) -> Result<&fs::SuperBlock<Self>> {
        let sb = sb.init(
            (),
            &fs::SuperParams {
                magic: 0x72757374,
                ..fs::SuperParams::DEFAULT
            },
        )?;
        let sb = sb.init_root()?;
        Ok(sb)
    }
}
