// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

use kernel::{
    list::ListArcSafe,
    prelude::*,
    sync::{Arc, SpinLock},
    task::Kuid,
    types::ScopeGuard,
    uaccess::UserSliceWriter,
};

use crate::{
    allocation::Allocation,
    defs::*,
    error::{BinderError, BinderResult},
    node::{Node, NodeRef},
    process::Process,
    ptr_align,
    thread::{PushWorkRes, Thread},
    DArc, DLArc, DTRWrap, DeliverToRead,
};

#[pin_data]
pub(crate) struct Transaction {
    target_node: Option<DArc<Node>>,
    from_parent: Option<DArc<Transaction>>,
    pub(crate) from: Arc<Thread>,
    to: Arc<Process>,
    #[pin]
    allocation: SpinLock<Option<Allocation>>,
    code: u32,
    pub(crate) flags: u32,
    data_size: usize,
    offsets_size: usize,
    data_address: usize,
    sender_euid: Kuid,
    txn_security_ctx_off: Option<usize>,
}

kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for Transaction { untracked; }
}

impl Transaction {
    pub(crate) fn new(
        node_ref: NodeRef,
        from_parent: Option<DArc<Transaction>>,
        from: &Arc<Thread>,
        tr: &BinderTransactionDataSg,
    ) -> BinderResult<DLArc<Self>> {
        let trd = &tr.transaction_data;
        let txn_security_ctx = node_ref.node.flags & FLAT_BINDER_FLAG_TXN_SECURITY_CTX != 0;
        let mut txn_security_ctx_off = if txn_security_ctx { Some(0) } else { None };
        let to = node_ref.node.owner.clone();
        let mut alloc =
            match from.copy_transaction_data(to.clone(), tr, txn_security_ctx_off.as_mut()) {
                Ok(alloc) => alloc,
                Err(err) => {
                    if !err.is_dead() {
                        pr_warn!("Failure in copy_transaction_data: {:?}", err);
                    }
                    return Err(err);
                }
            };
        if trd.flags & TF_ONE_WAY != 0 {
            if from_parent.is_some() {
                pr_warn!("Oneway transaction should not be in a transaction stack.");
                return Err(EINVAL.into());
            }
            alloc.set_info_oneway_node(node_ref.node.clone());
        }
        if trd.flags & TF_CLEAR_BUF != 0 {
            alloc.set_info_clear_on_drop();
        }
        let target_node = node_ref.node.clone();
        alloc.set_info_target_node(node_ref);
        let data_address = alloc.ptr;

        Ok(DTRWrap::arc_pin_init(pin_init!(Transaction {
            target_node: Some(target_node),
            from_parent,
            sender_euid: from.process.cred.euid(),
            from: from.clone(),
            to,
            code: trd.code,
            flags: trd.flags,
            data_size: trd.data_size as _,
            offsets_size: trd.offsets_size as _,
            data_address,
            allocation <- kernel::new_spinlock!(Some(alloc), "Transaction::new"),
            txn_security_ctx_off,
        }))?)
    }

    pub(crate) fn new_reply(
        from: &Arc<Thread>,
        to: Arc<Process>,
        tr: &BinderTransactionDataSg,
    ) -> BinderResult<DLArc<Self>> {
        let trd = &tr.transaction_data;
        let mut alloc = match from.copy_transaction_data(to.clone(), tr, None) {
            Ok(alloc) => alloc,
            Err(err) => {
                pr_warn!("Failure in copy_transaction_data: {:?}", err);
                return Err(err);
            }
        };
        if trd.flags & TF_CLEAR_BUF != 0 {
            alloc.set_info_clear_on_drop();
        }
        Ok(DTRWrap::arc_pin_init(pin_init!(Transaction {
            target_node: None,
            from_parent: None,
            sender_euid: from.process.task.euid(),
            from: from.clone(),
            to,
            code: trd.code,
            flags: trd.flags,
            data_size: trd.data_size as _,
            offsets_size: trd.offsets_size as _,
            data_address: alloc.ptr,
            allocation <- kernel::new_spinlock!(Some(alloc), "Transaction::new"),
            txn_security_ctx_off: None,
        }))?)
    }

    /// Determines if the transaction is stacked on top of the given transaction.
    pub(crate) fn is_stacked_on(&self, onext: &Option<DArc<Self>>) -> bool {
        match (&self.from_parent, onext) {
            (None, None) => true,
            (Some(from_parent), Some(next)) => Arc::ptr_eq(from_parent, next),
            _ => false,
        }
    }

    /// Returns a pointer to the next transaction on the transaction stack, if there is one.
    pub(crate) fn clone_next(&self) -> Option<DArc<Self>> {
        Some(self.from_parent.as_ref()?.clone())
    }

    /// Searches in the transaction stack for a thread that belongs to the target process. This is
    /// useful when finding a target for a new transaction: if the node belongs to a process that
    /// is already part of the transaction stack, we reuse the thread.
    fn find_target_thread(&self) -> Option<Arc<Thread>> {
        let mut it = &self.from_parent;
        while let Some(transaction) = it {
            if Arc::ptr_eq(&transaction.from.process, &self.to) {
                return Some(transaction.from.clone());
            }
            it = &transaction.from_parent;
        }
        None
    }

    /// Searches in the transaction stack for a transaction originating at the given thread.
    pub(crate) fn find_from(&self, thread: &Thread) -> Option<DArc<Transaction>> {
        let mut it = &self.from_parent;
        while let Some(transaction) = it {
            if core::ptr::eq(thread, transaction.from.as_ref()) {
                return Some(transaction.clone());
            }

            it = &transaction.from_parent;
        }
        None
    }

    /// Submits the transaction to a work queue. Uses a thread if there is one in the transaction
    /// stack, otherwise uses the destination process.
    ///
    /// Not used for replies.
    pub(crate) fn submit(self: DLArc<Self>) -> BinderResult {
        let oneway = self.flags & TF_ONE_WAY != 0;
        let process = self.to.clone();
        let mut process_inner = process.inner.lock();

        if oneway {
            if let Some(target_node) = self.target_node.clone() {
                match target_node.submit_oneway(self, &mut process_inner) {
                    Ok(()) => return Ok(()),
                    Err((err, work)) => {
                        drop(process_inner);
                        // Drop work after releasing process lock.
                        drop(work);
                        return Err(err);
                    }
                }
            } else {
                pr_err!("Failed to submit oneway transaction to node.");
            }
        }

        let res = if let Some(thread) = self.find_target_thread() {
            match thread.push_work(self) {
                PushWorkRes::Ok => Ok(()),
                PushWorkRes::FailedDead(me) => Err((BinderError::new_dead(), me)),
            }
        } else {
            process_inner.push_work(self)
        };
        drop(process_inner);

        match res {
            Ok(()) => Ok(()),
            Err((err, work)) => {
                // Drop work after releasing process lock.
                drop(work);
                Err(err)
            }
        }
    }
}

impl DeliverToRead for Transaction {
    fn do_work(self: DArc<Self>, thread: &Thread, writer: &mut UserSliceWriter) -> Result<bool> {
        let send_failed_reply = ScopeGuard::new(|| {
            if self.target_node.is_some() && self.flags & TF_ONE_WAY == 0 {
                let reply = Err(BR_FAILED_REPLY);
                self.from.deliver_reply(reply, &self);
            }
        });

        let mut tr_sec = BinderTransactionDataSecctx::default();
        let tr = tr_sec.tr_data();
        if let Some(target_node) = &self.target_node {
            let (ptr, cookie) = target_node.get_id();
            tr.target.ptr = ptr as _;
            tr.cookie = cookie as _;
        };
        tr.code = self.code;
        tr.flags = self.flags;
        tr.data_size = self.data_size as _;
        tr.data.ptr.buffer = self.data_address as _;
        tr.offsets_size = self.offsets_size as _;
        if tr.offsets_size > 0 {
            tr.data.ptr.offsets = (self.data_address + ptr_align(self.data_size)) as _;
        }
        tr.sender_euid = self.sender_euid.into_uid_in_current_ns();
        tr.sender_pid = 0;
        if self.target_node.is_some() && self.flags & TF_ONE_WAY == 0 {
            // Not a reply and not one-way.
            tr.sender_pid = self.from.process.task.pid_in_current_ns();
        }
        let code = if self.target_node.is_none() {
            BR_REPLY
        } else if self.txn_security_ctx_off.is_some() {
            BR_TRANSACTION_SEC_CTX
        } else {
            BR_TRANSACTION
        };

        // Write the transaction code and data to the user buffer.
        writer.write(&code)?;
        if let Some(off) = self.txn_security_ctx_off {
            tr_sec.secctx = (self.data_address + off) as u64;
            writer.write(&tr_sec)?;
        } else {
            writer.write(&*tr)?;
        }

        // Dismiss the completion of transaction with a failure. No failure paths are allowed from
        // here on out.
        send_failed_reply.dismiss();

        // It is now the user's responsibility to clear the allocation.
        let alloc = self.allocation.lock().take();
        if let Some(alloc) = alloc {
            alloc.keep_alive();
        }

        // When this is not a reply and not a oneway transaction, update `current_transaction`. If
        // it's a reply, `current_transaction` has already been updated appropriately.
        if self.target_node.is_some() && tr_sec.transaction_data.flags & TF_ONE_WAY == 0 {
            thread.set_current_transaction(self);
        }

        Ok(false)
    }

    fn cancel(self: DArc<Self>) {
        drop(self.allocation.lock().take());

        // If this is not a reply or oneway transaction, then send a dead reply.
        if self.target_node.is_some() && self.flags & TF_ONE_WAY == 0 {
            let reply = Err(BR_DEAD_REPLY);
            self.from.deliver_reply(reply, &self);
        }
    }

    fn should_sync_wakeup(&self) -> bool {
        self.flags & TF_ONE_WAY == 0
    }
}
