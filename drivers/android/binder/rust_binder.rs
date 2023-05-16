// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Binder -- the Android IPC mechanism.

use kernel::prelude::*;

module! {
    type: BinderModule,
    name: "rust_binder",
    author: "Wedson Almeida Filho, Alice Ryhl",
    description: "Android Binder",
    license: "GPL",
}

struct BinderModule {}

impl kernel::Module for BinderModule {
    fn init(_module: &'static kernel::ThisModule) -> Result<Self> {
        Ok(Self {})
    }
}
