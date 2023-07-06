// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

use core::mem::size_of_val;

use kernel::{page::Page, prelude::*, sync::Arc, uaccess::UserSliceReader};

use crate::{
    node::{Node, NodeRef},
    process::Process,
    DArc,
};

#[derive(Default)]
pub(crate) struct AllocationInfo {
    /// The target node of the transaction this allocation is associated to.
    /// Not set for replies.
    pub(crate) target_node: Option<NodeRef>,
    /// When this allocation is dropped, call `pending_oneway_finished` on the node.
    ///
    /// This is used to serialize oneway transaction on the same node. Binder guarantees that
    /// oneway transactions to the same node are delivered sequentially in the order they are sent.
    pub(crate) oneway_node: Option<DArc<Node>>,
    /// Zero the data in the buffer on free.
    pub(crate) clear_on_free: bool,
}

/// Represents an allocation that the kernel is currently using.
///
/// When allocations are idle, the range allocator holds the data related to them.
pub(crate) struct Allocation {
    pub(crate) offset: usize,
    size: usize,
    pub(crate) ptr: usize,
    pages: Arc<Vec<Page>>,
    pub(crate) process: Arc<Process>,
    allocation_info: Option<AllocationInfo>,
    free_on_drop: bool,
}

impl Allocation {
    pub(crate) fn new(
        process: Arc<Process>,
        offset: usize,
        size: usize,
        ptr: usize,
        pages: Arc<Vec<Page>>,
    ) -> Self {
        Self {
            process,
            offset,
            size,
            ptr,
            pages,
            allocation_info: None,
            free_on_drop: true,
        }
    }

    fn iterate<T>(&self, mut offset: usize, mut size: usize, mut cb: T) -> Result
    where
        T: FnMut(&Page, usize, usize) -> Result,
    {
        use kernel::page::PAGE_SHIFT;

        // Check that the request is within the buffer.
        if offset.checked_add(size).ok_or(EINVAL)? > self.size {
            return Err(EINVAL);
        }
        offset += self.offset;
        let mut page_index = offset >> PAGE_SHIFT;
        offset &= (1 << PAGE_SHIFT) - 1;
        while size > 0 {
            let available = core::cmp::min(size, (1 << PAGE_SHIFT) - offset);
            cb(&self.pages[page_index], offset, available)?;
            size -= available;
            page_index += 1;
            offset = 0;
        }
        Ok(())
    }

    pub(crate) fn copy_into(
        &self,
        reader: &mut UserSliceReader,
        offset: usize,
        size: usize,
    ) -> Result {
        self.iterate(offset, size, |page, offset, to_copy| {
            page.copy_into_page(reader, offset, to_copy)
        })
    }

    pub(crate) fn write<T: ?Sized>(&self, offset: usize, obj: &T) -> Result {
        let mut obj_offset = 0;
        self.iterate(offset, size_of_val(obj), |page, offset, to_copy| {
            // SAFETY: The sum of `offset` and `to_copy` is bounded by the size of T.
            let obj_ptr = unsafe { (obj as *const T as *const u8).add(obj_offset) };
            // SAFETY: We have a reference to the object, so the pointer is valid.
            unsafe { page.write(obj_ptr, offset, to_copy) }?;
            obj_offset += to_copy;
            Ok(())
        })
    }

    pub(crate) fn fill_zero(&self) -> Result {
        self.iterate(0, self.size, |page, offset, len| {
            page.fill_zero(offset, len)
        })
    }

    pub(crate) fn keep_alive(mut self) {
        self.process
            .buffer_make_freeable(self.offset, self.allocation_info.take());
        self.free_on_drop = false;
    }

    pub(crate) fn set_info(&mut self, info: AllocationInfo) {
        self.allocation_info = Some(info);
    }

    pub(crate) fn get_or_init_info(&mut self) -> &mut AllocationInfo {
        self.allocation_info.get_or_insert_with(Default::default)
    }

    pub(crate) fn set_info_oneway_node(&mut self, oneway_node: DArc<Node>) {
        self.get_or_init_info().oneway_node = Some(oneway_node);
    }

    pub(crate) fn set_info_clear_on_drop(&mut self) {
        self.get_or_init_info().clear_on_free = true;
    }

    pub(crate) fn set_info_target_node(&mut self, target_node: NodeRef) {
        self.get_or_init_info().target_node = Some(target_node);
    }
}

impl Drop for Allocation {
    fn drop(&mut self) {
        if !self.free_on_drop {
            return;
        }

        if let Some(mut info) = self.allocation_info.take() {
            if let Some(oneway_node) = info.oneway_node.as_ref() {
                oneway_node.pending_oneway_finished();
            }

            info.target_node = None;

            if info.clear_on_free {
                if let Err(e) = self.fill_zero() {
                    pr_warn!("Failed to clear data on free: {:?}", e);
                }
            }
        }

        self.process.buffer_raw_free(self.ptr);
    }
}
