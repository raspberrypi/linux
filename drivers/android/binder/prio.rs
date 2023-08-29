// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! This module defines the types and methods relevant to priority inheritance.

use kernel::bindings;

pub(crate) type Policy = core::ffi::c_uint;
pub(crate) type Priority = core::ffi::c_int;
pub(crate) type Nice = core::ffi::c_int;

pub(crate) const SCHED_NORMAL: Policy = bindings::SCHED_NORMAL;
pub(crate) const SCHED_FIFO: Policy = bindings::SCHED_FIFO;
pub(crate) const MIN_NICE: Nice = bindings::MIN_NICE as _;
pub(crate) const MAX_NICE: Nice = bindings::MAX_NICE as _;
pub(crate) const DEFAULT_PRIO: Priority = bindings::DEFAULT_PRIO as _;
pub(crate) const MAX_RT_PRIO: Priority = bindings::MAX_RT_PRIO as _;

/// Scheduler policy and priority.
///
/// The binder driver supports inheriting the following scheduler policies:
/// * SCHED_NORMAL
/// * SCHED_BATCH
/// * SCHED_FIFO
/// * SCHED_RR
#[derive(Copy, Clone, Default)]
pub(crate) struct BinderPriority {
    pub(crate) sched_policy: Policy,
    pub(crate) prio: Priority,
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub(crate) enum PriorityState {
    Set,
    Pending,
    Abort,
}

pub(crate) fn get_default_prio_from_task(task: &kernel::task::Task) -> BinderPriority {
    if is_supported_policy(task.policy()) {
        BinderPriority {
            sched_policy: task.policy(),
            prio: task.normal_prio(),
        }
    } else {
        BinderPriority {
            sched_policy: SCHED_NORMAL,
            prio: DEFAULT_PRIO,
        }
    }
}

pub(crate) fn is_rt_policy(policy: Policy) -> bool {
    policy == bindings::SCHED_FIFO || policy == bindings::SCHED_RR
}

pub(crate) fn is_fair_policy(policy: Policy) -> bool {
    policy == bindings::SCHED_NORMAL || policy == bindings::SCHED_BATCH
}

pub(crate) fn is_supported_policy(policy: Policy) -> bool {
    is_fair_policy(policy) || is_rt_policy(policy)
}

pub(crate) fn to_userspace_prio(policy: Policy, prio: Priority) -> Nice {
    if is_fair_policy(policy) {
        prio - DEFAULT_PRIO
    } else {
        MAX_RT_PRIO - 1 - prio
    }
}

pub(crate) fn to_kernel_prio(policy: Policy, prio: Nice) -> Priority {
    if is_fair_policy(policy) {
        prio + DEFAULT_PRIO
    } else {
        MAX_RT_PRIO - 1 - prio
    }
}
