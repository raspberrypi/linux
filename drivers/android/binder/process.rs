// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! This module defines the `Process` type, which represents a process using a particular binder
//! context.
//!
//! The `Process` object keeps track of all of the resources that this process owns in the binder
//! context.
//!
//! There is one `Process` object for each binder fd that a process has opened, so processes using
//! several binder contexts have several `Process` objects. This ensures that the contexts are
//! fully separated.

use kernel::{
    bindings,
    cred::Credential,
    file::{self, File},
    list::{HasListLinks, List, ListArc, ListArcField, ListArcSafe, ListItem, ListLinks},
    mm,
    page::Page,
    prelude::*,
    rbtree::{self, RBTree},
    sync::poll::PollTable,
    sync::{lock::Guard, Arc, ArcBorrow, Mutex, SpinLock, UniqueArc},
    task::Task,
    types::{ARef, Either},
    uaccess::{UserSlice, UserSliceReader},
    workqueue::{self, Work},
};

use crate::{
    allocation::{Allocation, AllocationInfo},
    context::Context,
    defs::*,
    error::{BinderError, BinderResult},
    node::{Node, NodeRef},
    range_alloc::{self, RangeAllocator},
    thread::{PushWorkRes, Thread},
    DArc, DLArc, DTRWrap, DeliverToRead,
};

use core::mem::take;

struct Mapping {
    address: usize,
    alloc: RangeAllocator<AllocationInfo>,
    pages: Arc<Vec<Page>>,
}

impl Mapping {
    fn new(address: usize, size: usize, pages: Arc<Vec<Page>>) -> Result<Self> {
        let alloc = RangeAllocator::new(size)?;
        Ok(Self {
            address,
            alloc,
            pages,
        })
    }
}

const PROC_DEFER_FLUSH: u8 = 1;
const PROC_DEFER_RELEASE: u8 = 2;

/// The fields of `Process` protected by the spinlock.
pub(crate) struct ProcessInner {
    is_manager: bool,
    pub(crate) is_dead: bool,
    threads: RBTree<i32, Arc<Thread>>,
    /// INVARIANT: Threads pushed to this list must be owned by this process.
    ready_threads: List<Thread>,
    nodes: RBTree<u64, DArc<Node>>,
    mapping: Option<Mapping>,
    work: List<DTRWrap<dyn DeliverToRead>>,

    /// The number of requested threads that haven't registered yet.
    requested_thread_count: u32,
    /// The maximum number of threads used by the process thread pool.
    max_threads: u32,
    /// The number of threads the started and registered with the thread pool.
    started_thread_count: u32,

    /// Bitmap of deferred work to do.
    defer_work: u8,
}

impl ProcessInner {
    fn new() -> Self {
        Self {
            is_manager: false,
            is_dead: false,
            threads: RBTree::new(),
            ready_threads: List::new(),
            mapping: None,
            nodes: RBTree::new(),
            work: List::new(),
            requested_thread_count: 0,
            max_threads: 0,
            started_thread_count: 0,
            defer_work: 0,
        }
    }

    /// Schedule the work item for execution on this process.
    ///
    /// If any threads are ready for work, then the work item is given directly to that thread and
    /// it is woken up. Otherwise, it is pushed to the process work list.
    ///
    /// This call can fail only if the process is dead. In this case, the work item is returned to
    /// the caller so that the caller can drop it after releasing the inner process lock. This is
    /// necessary since the destructor of `Transaction` will take locks that can't necessarily be
    /// taken while holding the inner process lock.
    pub(crate) fn push_work(
        &mut self,
        work: DLArc<dyn DeliverToRead>,
    ) -> Result<(), (BinderError, DLArc<dyn DeliverToRead>)> {
        // Try to find a ready thread to which to push the work.
        if let Some(thread) = self.ready_threads.pop_front() {
            // Push to thread while holding state lock. This prevents the thread from giving up
            // (for example, because of a signal) when we're about to deliver work.
            match thread.push_work(work) {
                PushWorkRes::Ok => Ok(()),
                PushWorkRes::FailedDead(work) => Err((BinderError::new_dead(), work)),
            }
        } else if self.is_dead {
            Err((BinderError::new_dead(), work))
        } else {
            // There are no ready threads. Push work to process queue.
            self.work.push_back(work);
            Ok(())
        }
    }

    pub(crate) fn remove_node(&mut self, ptr: u64) {
        self.nodes.remove(&ptr);
    }

    /// Updates the reference count on the given node.
    pub(crate) fn update_node_refcount(
        &mut self,
        node: &DArc<Node>,
        inc: bool,
        strong: bool,
        count: usize,
        othread: Option<&Thread>,
    ) {
        let push = node.update_refcount_locked(inc, strong, count, self);

        // If we decided that we need to push work, push either to the process or to a thread if
        // one is specified.
        if push {
            // It's not a problem if creating the ListArc fails, because that just means that
            // it is already queued to a worklist.
            if let Some(node) = ListArc::try_from_arc_or_drop(node.clone()) {
                if let Some(thread) = othread {
                    thread.push_work_deferred(node);
                } else {
                    let _ = self.push_work(node);
                    // Nothing to do: `push_work` may fail if the process is dead, but that's ok as in
                    // that case, it doesn't care about the notification.
                }
            }
        }
    }

    pub(crate) fn new_node_ref(
        &mut self,
        node: DArc<Node>,
        strong: bool,
        thread: Option<&Thread>,
    ) -> NodeRef {
        self.update_node_refcount(&node, true, strong, 1, thread);
        let strong_count = if strong { 1 } else { 0 };
        NodeRef::new(node, strong_count, 1 - strong_count)
    }

    /// Returns an existing node with the given pointer and cookie, if one exists.
    ///
    /// Returns an error if a node with the given pointer but a different cookie exists.
    fn get_existing_node(&self, ptr: u64, cookie: u64) -> Result<Option<DArc<Node>>> {
        match self.nodes.get(&ptr) {
            None => Ok(None),
            Some(node) => {
                let (_, node_cookie) = node.get_id();
                if node_cookie == cookie {
                    Ok(Some(node.clone()))
                } else {
                    Err(EINVAL)
                }
            }
        }
    }

    /// Returns a reference to an existing node with the given pointer and cookie. It requires a
    /// mutable reference because it needs to increment the ref count on the node, which may
    /// require pushing work to the work queue (to notify userspace of 0 to 1 transitions).
    fn get_existing_node_ref(
        &mut self,
        ptr: u64,
        cookie: u64,
        strong: bool,
        thread: Option<&Thread>,
    ) -> Result<Option<NodeRef>> {
        Ok(self
            .get_existing_node(ptr, cookie)?
            .map(|node| self.new_node_ref(node, strong, thread)))
    }

    fn register_thread(&mut self) -> bool {
        if self.requested_thread_count == 0 {
            return false;
        }

        self.requested_thread_count -= 1;
        self.started_thread_count += 1;
        true
    }
}

/// Used to keep track of a node that this process has a handle to.
#[pin_data]
pub(crate) struct NodeRefInfo {
    /// The refcount that this process owns to the node.
    node_ref: ListArcField<NodeRef, { Self::LIST_PROC }>,
    /// Used to store this `NodeRefInfo` in the node's `refs` list.
    #[pin]
    links: ListLinks<{ Self::LIST_NODE }>,
    /// The handle for this `NodeRefInfo`.
    handle: u32,
    /// The process that has a handle to the node.
    process: Arc<Process>,
}

impl NodeRefInfo {
    /// The id used for the `Node::refs` list.
    pub(crate) const LIST_NODE: u64 = 0x2da16350fb724a10;
    /// The id used for the `ListArc` in `ProcessNodeRefs`.
    const LIST_PROC: u64 = 0xd703a5263dcc8650;

    fn new(node_ref: NodeRef, handle: u32, process: Arc<Process>) -> impl PinInit<Self> {
        pin_init!(Self {
            node_ref: ListArcField::new(node_ref),
            links <- ListLinks::new(),
            handle,
            process,
        })
    }

    kernel::list::define_list_arc_field_getter! {
        pub(crate) fn node_ref(&mut self<{Self::LIST_PROC}>) -> &mut NodeRef { node_ref }
        pub(crate) fn node_ref2(&self<{Self::LIST_PROC}>) -> &NodeRef { node_ref }
    }
}

kernel::list::impl_has_list_links! {
    impl HasListLinks<{Self::LIST_NODE}> for NodeRefInfo { self.links }
}
kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<{Self::LIST_NODE}> for NodeRefInfo { untracked; }
    impl ListArcSafe<{Self::LIST_PROC}> for NodeRefInfo { untracked; }
}
kernel::list::impl_list_item! {
    impl ListItem<{Self::LIST_NODE}> for NodeRefInfo {
        using ListLinks;
    }
}

/// Keeps track of references this process has to nodes owned by other processes.
///
/// TODO: Currently, the rbtree requires two allocations per node reference, and two tree
/// traversals to look up a node by `Node::global_id`. Once the rbtree is more powerful, these
/// extra costs should be eliminated.
struct ProcessNodeRefs {
    /// Used to look up nodes using the 32-bit id that this process knows it by.
    by_handle: RBTree<u32, ListArc<NodeRefInfo, { NodeRefInfo::LIST_PROC }>>,
    /// Used to look up nodes without knowing their local 32-bit id. The usize is the address of
    /// the underlying `Node` struct as returned by `Node::global_id`.
    by_node: RBTree<usize, u32>,
}

impl ProcessNodeRefs {
    fn new() -> Self {
        Self {
            by_handle: RBTree::new(),
            by_node: RBTree::new(),
        }
    }
}

/// A process using binder.
///
/// Strictly speaking, there can be multiple of these per process. There is one for each binder fd
/// that a process has opened, so processes using several binder contexts have several `Process`
/// objects. This ensures that the contexts are fully separated.
#[pin_data]
pub(crate) struct Process {
    pub(crate) ctx: Arc<Context>,

    // The task leader (process).
    pub(crate) task: ARef<Task>,

    // Credential associated with file when `Process` is created.
    pub(crate) cred: ARef<Credential>,

    #[pin]
    pub(crate) inner: SpinLock<ProcessInner>,

    // Node references are in a different lock to avoid recursive acquisition when
    // incrementing/decrementing a node in another process.
    #[pin]
    node_refs: Mutex<ProcessNodeRefs>,

    // Work node for deferred work item.
    #[pin]
    defer_work: Work<Process>,

    // Links for process list in Context.
    #[pin]
    links: ListLinks,
}

kernel::impl_has_work! {
    impl HasWork<Process> for Process { self.defer_work }
}

kernel::list::impl_has_list_links! {
    impl HasListLinks<0> for Process { self.links }
}
kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for Process { untracked; }
}
kernel::list::impl_list_item! {
    impl ListItem<0> for Process {
        using ListLinks;
    }
}

impl workqueue::WorkItem for Process {
    type Pointer = Arc<Process>;

    fn run(me: Arc<Self>) {
        let defer;
        {
            let mut inner = me.inner.lock();
            defer = inner.defer_work;
            inner.defer_work = 0;
        }

        if defer & PROC_DEFER_FLUSH != 0 {
            me.deferred_flush();
        }
        if defer & PROC_DEFER_RELEASE != 0 {
            me.deferred_release();
        }
    }
}

impl Process {
    fn new(ctx: Arc<Context>, cred: ARef<Credential>) -> Result<Arc<Self>> {
        let list_process = ListArc::pin_init(pin_init!(Process {
            ctx,
            cred,
            inner <- kernel::new_spinlock!(ProcessInner::new(), "Process::inner"),
            node_refs <- kernel::new_mutex!(ProcessNodeRefs::new(), "Process::node_refs"),
            task: kernel::current!().group_leader().into(),
            defer_work <- kernel::new_work!("Process::defer_work"),
            links <- ListLinks::new(),
        }))?;

        let process = list_process.clone_arc();
        process.ctx.register_process(list_process);

        Ok(process)
    }

    /// Attempts to fetch a work item from the process queue.
    pub(crate) fn get_work(&self) -> Option<DLArc<dyn DeliverToRead>> {
        self.inner.lock().work.pop_front()
    }

    /// Attempts to fetch a work item from the process queue. If none is available, it registers the
    /// given thread as ready to receive work directly.
    ///
    /// This must only be called when the thread is not participating in a transaction chain; when
    /// it is, work will always be delivered directly to the thread (and not through the process
    /// queue).
    pub(crate) fn get_work_or_register<'a>(
        &'a self,
        thread: &'a Arc<Thread>,
    ) -> Either<DLArc<dyn DeliverToRead>, Registration<'a>> {
        let mut inner = self.inner.lock();
        // Try to get work from the process queue.
        if let Some(work) = inner.work.pop_front() {
            return Either::Left(work);
        }

        // Register the thread as ready.
        Either::Right(Registration::new(thread, &mut inner))
    }

    fn get_current_thread(self: ArcBorrow<'_, Self>) -> Result<Arc<Thread>> {
        let id = {
            let current = kernel::current!();
            if !core::ptr::eq(current.group_leader(), &*self.task) {
                pr_err!("get_current_thread was called from the wrong process.");
                return Err(EINVAL);
            }
            current.pid()
        };

        {
            let inner = self.inner.lock();
            if let Some(thread) = inner.threads.get(&id) {
                return Ok(thread.clone());
            }
        }

        // Allocate a new `Thread` without holding any locks.
        let reservation = RBTree::try_reserve_node()?;
        let ta: Arc<Thread> = Thread::new(id, self.into())?;

        let mut inner = self.inner.lock();
        match inner.threads.entry(id) {
            rbtree::Entry::Vacant(entry) => {
                entry.insert(ta.clone(), reservation);
                Ok(ta)
            }
            rbtree::Entry::Occupied(_entry) => {
                pr_err!("Cannot create two threads with the same id.");
                Err(EINVAL)
            }
        }
    }

    fn set_as_manager(
        self: ArcBorrow<'_, Self>,
        info: Option<FlatBinderObject>,
        thread: &Thread,
    ) -> Result {
        let (ptr, cookie, flags) = if let Some(obj) = info {
            (
                // SAFETY: The object type for this ioctl is implicitly `BINDER_TYPE_BINDER`, so it
                // is safe to access the `binder` field.
                unsafe { obj.__bindgen_anon_1.binder },
                obj.cookie,
                obj.flags,
            )
        } else {
            (0, 0, 0)
        };
        let node_ref = self.get_node(ptr, cookie, flags as _, true, Some(thread))?;
        let node = node_ref.node.clone();
        self.ctx.set_manager_node(node_ref)?;
        self.inner.lock().is_manager = true;

        // Force the state of the node to prevent the delivery of acquire/increfs.
        let mut owner_inner = node.owner.inner.lock();
        node.force_has_count(&mut owner_inner);
        Ok(())
    }

    pub(crate) fn get_node(
        self: ArcBorrow<'_, Self>,
        ptr: u64,
        cookie: u64,
        flags: u32,
        strong: bool,
        thread: Option<&Thread>,
    ) -> Result<NodeRef> {
        // Try to find an existing node.
        {
            let mut inner = self.inner.lock();
            if let Some(node) = inner.get_existing_node_ref(ptr, cookie, strong, thread)? {
                return Ok(node);
            }
        }

        // Allocate the node before reacquiring the lock.
        let node = DTRWrap::arc_pin_init(Node::new(ptr, cookie, flags, self.into()))?.into_arc();
        let rbnode = RBTree::try_allocate_node(ptr, node.clone())?;
        let mut inner = self.inner.lock();
        if let Some(node) = inner.get_existing_node_ref(ptr, cookie, strong, thread)? {
            return Ok(node);
        }

        inner.nodes.insert(rbnode);
        Ok(inner.new_node_ref(node, strong, thread))
    }

    fn insert_or_update_handle(
        self: ArcBorrow<'_, Process>,
        node_ref: NodeRef,
        is_mananger: bool,
    ) -> Result<u32> {
        {
            let mut refs = self.node_refs.lock();

            // Do a lookup before inserting.
            if let Some(handle_ref) = refs.by_node.get(&node_ref.node.global_id()) {
                let handle = *handle_ref;
                let info = refs.by_handle.get_mut(&handle).unwrap();
                info.node_ref().absorb(node_ref);
                return Ok(handle);
            }
        }

        // Reserve memory for tree nodes.
        let reserve1 = RBTree::try_reserve_node()?;
        let reserve2 = RBTree::try_reserve_node()?;
        let info = UniqueArc::try_new_uninit()?;

        let mut refs = self.node_refs.lock();

        // Do a lookup again as node may have been inserted before the lock was reacquired.
        if let Some(handle_ref) = refs.by_node.get(&node_ref.node.global_id()) {
            let handle = *handle_ref;
            let info = refs.by_handle.get_mut(&handle).unwrap();
            info.node_ref().absorb(node_ref);
            return Ok(handle);
        }

        // Find id.
        let mut target: u32 = if is_mananger { 0 } else { 1 };
        for handle in refs.by_handle.keys() {
            if *handle > target {
                break;
            }
            if *handle == target {
                target = target.checked_add(1).ok_or(ENOMEM)?;
            }
        }

        let gid = node_ref.node.global_id();
        let (info_proc, info_node) = {
            let info_init = NodeRefInfo::new(node_ref, target, self.into());
            match info.pin_init_with(info_init) {
                Ok(info) => ListArc::pair_from_pin_unique(info),
                // error is infallible
                Err(err) => match err {},
            }
        };

        // Ensure the process is still alive while we insert a new reference.
        //
        // This releases the lock before inserting the nodes, but since `is_dead` is set as the
        // first thing in `deferred_release`, process cleanup will not miss the items inserted into
        // `refs` below.
        if self.inner.lock().is_dead {
            return Err(ESRCH);
        }

        // SAFETY: `info_proc` and `info_node` reference the same node, so we are inserting
        // `info_node` into the right node's `refs` list.
        unsafe { info_proc.node_ref2().node.insert_node_info(info_node) };

        refs.by_node.insert(reserve1.into_node(gid, target));
        refs.by_handle.insert(reserve2.into_node(target, info_proc));
        Ok(target)
    }

    pub(crate) fn get_transaction_node(&self, handle: u32) -> BinderResult<NodeRef> {
        // When handle is zero, try to get the context manager.
        if handle == 0 {
            Ok(self.ctx.get_manager_node(true)?)
        } else {
            Ok(self.get_node_from_handle(handle, true)?)
        }
    }

    pub(crate) fn get_node_from_handle(&self, handle: u32, strong: bool) -> Result<NodeRef> {
        self.node_refs
            .lock()
            .by_handle
            .get_mut(&handle)
            .ok_or(ENOENT)?
            .node_ref()
            .clone(strong)
    }

    pub(crate) fn update_ref(
        self: ArcBorrow<'_, Process>,
        handle: u32,
        inc: bool,
        strong: bool,
    ) -> Result {
        if inc && handle == 0 {
            if let Ok(node_ref) = self.ctx.get_manager_node(strong) {
                if core::ptr::eq(&*self, &*node_ref.node.owner) {
                    return Err(EINVAL);
                }
                let _ = self.insert_or_update_handle(node_ref, true);
                return Ok(());
            }
        }

        // To preserve original binder behaviour, we only fail requests where the manager tries to
        // increment references on itself.
        let mut refs = self.node_refs.lock();
        if let Some(info) = refs.by_handle.get_mut(&handle) {
            if info.node_ref().update(inc, strong) {
                // Remove reference from process tables, and from the node's `refs` list.

                // SAFETY: We are removing the `NodeRefInfo` from the right node.
                unsafe { info.node_ref2().node.remove_node_info(&info) };

                let id = info.node_ref().node.global_id();
                refs.by_handle.remove(&handle);
                refs.by_node.remove(&id);
            }
        }
        Ok(())
    }

    pub(crate) fn inc_ref_done(&self, reader: &mut UserSliceReader, strong: bool) -> Result {
        let ptr = reader.read::<u64>()?;
        let cookie = reader.read::<u64>()?;
        let mut inner = self.inner.lock();
        if let Ok(Some(node)) = inner.get_existing_node(ptr, cookie) {
            if node.inc_ref_done_locked(strong, &mut inner) {
                // It's not a problem if creating the ListArc fails, because that just means that
                // it is already queued to a worklist.
                if let Some(node) = ListArc::try_from_arc_or_drop(node) {
                    // This only fails if the process is dead.
                    let _ = inner.push_work(node);
                }
            }
        }
        Ok(())
    }

    pub(crate) fn buffer_alloc(
        self: &Arc<Self>,
        size: usize,
        is_oneway: bool,
    ) -> BinderResult<Allocation> {
        let alloc = range_alloc::ReserveNewBox::try_new()?;
        let mut inner = self.inner.lock();
        let mapping = inner.mapping.as_mut().ok_or_else(BinderError::new_dead)?;
        let offset = mapping.alloc.reserve_new(size, is_oneway, alloc)?;
        Ok(Allocation::new(
            self.clone(),
            offset,
            size,
            mapping.address + offset,
            mapping.pages.clone(),
        ))
    }

    pub(crate) fn buffer_get(self: &Arc<Self>, ptr: usize) -> Option<Allocation> {
        let mut inner = self.inner.lock();
        let mapping = inner.mapping.as_mut()?;
        let offset = ptr.checked_sub(mapping.address)?;
        let (size, odata) = mapping.alloc.reserve_existing(offset).ok()?;
        let mut alloc = Allocation::new(self.clone(), offset, size, ptr, mapping.pages.clone());
        if let Some(data) = odata {
            alloc.set_info(data);
        }
        Some(alloc)
    }

    pub(crate) fn buffer_raw_free(&self, ptr: usize) {
        let mut inner = self.inner.lock();
        if let Some(ref mut mapping) = &mut inner.mapping {
            if ptr < mapping.address
                || mapping
                    .alloc
                    .reservation_abort(ptr - mapping.address)
                    .is_err()
            {
                pr_warn!(
                    "Pointer {:x} failed to free, base = {:x}\n",
                    ptr,
                    mapping.address
                );
            }
        }
    }

    pub(crate) fn buffer_make_freeable(&self, offset: usize, data: Option<AllocationInfo>) {
        let mut inner = self.inner.lock();
        if let Some(ref mut mapping) = &mut inner.mapping {
            if mapping.alloc.reservation_commit(offset, data).is_err() {
                pr_warn!("Offset {} failed to be marked freeable\n", offset);
            }
        }
    }

    fn create_mapping(&self, vma: &mut mm::virt::Area) -> Result {
        use kernel::page::PAGE_SIZE;
        let size = core::cmp::min(vma.end() - vma.start(), bindings::SZ_4M as usize);
        let page_count = size / PAGE_SIZE;

        // Allocate and map all pages.
        //
        // N.B. If we fail halfway through mapping these pages, the kernel will unmap them.
        let mut pages = Vec::new();
        pages.try_reserve_exact(page_count)?;
        let mut address = vma.start();
        for _ in 0..page_count {
            let page = Page::new()?;
            vma.insert_page(address, &page)?;
            pages.try_push(page)?;
            address += PAGE_SIZE;
        }

        let ref_pages = Arc::try_new(pages)?;
        let mapping = Mapping::new(vma.start(), size, ref_pages)?;

        // Save pages for later.
        let mut inner = self.inner.lock();
        match &inner.mapping {
            None => inner.mapping = Some(mapping),
            Some(_) => {
                drop(inner);
                drop(mapping);
                return Err(EBUSY);
            }
        }
        Ok(())
    }

    fn version(&self, data: UserSlice) -> Result {
        data.writer().write(&BinderVersion::current())
    }

    pub(crate) fn register_thread(&self) -> bool {
        self.inner.lock().register_thread()
    }

    fn remove_thread(&self, thread: Arc<Thread>) {
        self.inner.lock().threads.remove(&thread.id);
        thread.release();
    }

    fn set_max_threads(&self, max: u32) {
        self.inner.lock().max_threads = max;
    }

    fn get_node_debug_info(&self, data: UserSlice) -> Result {
        let (mut reader, mut writer) = data.reader_writer();

        // Read the starting point.
        let ptr = reader.read::<BinderNodeDebugInfo>()?.ptr;
        let mut out = BinderNodeDebugInfo::default();

        {
            let inner = self.inner.lock();
            for (node_ptr, node) in &inner.nodes {
                if *node_ptr > ptr {
                    node.populate_debug_info(&mut out, &inner);
                    break;
                }
            }
        }

        writer.write(&out)
    }

    fn get_node_info_from_ref(&self, data: UserSlice) -> Result {
        let (mut reader, mut writer) = data.reader_writer();
        let mut out = reader.read::<BinderNodeInfoForRef>()?;

        if out.strong_count != 0
            || out.weak_count != 0
            || out.reserved1 != 0
            || out.reserved2 != 0
            || out.reserved3 != 0
        {
            return Err(EINVAL);
        }

        // Only the context manager is allowed to use this ioctl.
        if !self.inner.lock().is_manager {
            return Err(EPERM);
        }

        let node_ref = self
            .get_node_from_handle(out.handle, true)
            .or(Err(EINVAL))?;
        // Get the counts from the node.
        {
            let owner_inner = node_ref.node.owner.inner.lock();
            node_ref.node.populate_counts(&mut out, &owner_inner);
        }

        // Write the result back.
        writer.write(&out)
    }

    pub(crate) fn needs_thread(&self) -> bool {
        let mut inner = self.inner.lock();
        let ret = inner.requested_thread_count == 0
            && inner.ready_threads.is_empty()
            && inner.started_thread_count < inner.max_threads;
        if ret {
            inner.requested_thread_count += 1
        }
        ret
    }

    fn deferred_flush(&self) {
        let inner = self.inner.lock();
        for thread in inner.threads.values() {
            thread.exit_looper();
        }
    }

    fn deferred_release(self: Arc<Self>) {
        let is_manager = {
            let mut inner = self.inner.lock();
            inner.is_dead = true;
            inner.is_manager
        };

        if is_manager {
            self.ctx.unset_manager_node();
        }

        self.ctx.deregister_process(&self);

        // Move the threads out of `inner` so that we can iterate over them without holding the
        // lock.
        let mut inner = self.inner.lock();
        let threads = take(&mut inner.threads);
        drop(inner);

        // Release all threads.
        for thread in threads.values() {
            thread.release();
        }

        // Cancel all pending work items.
        while let Some(work) = self.get_work() {
            work.into_arc().cancel();
        }

        // Free any resources kept alive by allocated buffers.
        let omapping = self.inner.lock().mapping.take();
        if let Some(mut mapping) = omapping {
            let address = mapping.address;
            let pages = mapping.pages.clone();
            mapping.alloc.take_for_each(|offset, size, odata| {
                let ptr = offset + address;
                let mut alloc = Allocation::new(self.clone(), offset, size, ptr, pages.clone());
                if let Some(data) = odata {
                    alloc.set_info(data);
                }
                drop(alloc)
            });
        }

        // Drop all references. We do this dance with `swap` to avoid destroying the references
        // while holding the lock.
        let mut refs = self.node_refs.lock();
        let node_refs = take(&mut refs.by_handle);
        drop(refs);
        for info in node_refs.values() {
            // SAFETY: We are removing the `NodeRefInfo` from the right node.
            unsafe { info.node_ref2().node.remove_node_info(&info) };
        }
        drop(node_refs);
    }

    pub(crate) fn flush(this: ArcBorrow<'_, Process>) -> Result {
        let should_schedule;
        {
            let mut inner = this.inner.lock();
            should_schedule = inner.defer_work == 0;
            inner.defer_work |= PROC_DEFER_FLUSH;
        }

        if should_schedule {
            // Ignore failures to schedule to the workqueue. Those just mean that we're already
            // scheduled for execution.
            let _ = workqueue::system().enqueue(Arc::from(this));
        }
        Ok(())
    }
}

/// The ioctl handler.
impl Process {
    /// Ioctls that are write-only from the perspective of userspace.
    ///
    /// The kernel will only read from the pointer that userspace provided to us.
    fn ioctl_write_only(
        this: ArcBorrow<'_, Process>,
        _file: &File,
        cmd: u32,
        reader: &mut UserSliceReader,
    ) -> Result<i32> {
        let thread = this.get_current_thread()?;
        match cmd {
            bindings::BINDER_SET_MAX_THREADS => this.set_max_threads(reader.read()?),
            bindings::BINDER_THREAD_EXIT => this.remove_thread(thread),
            bindings::BINDER_SET_CONTEXT_MGR => this.set_as_manager(None, &thread)?,
            bindings::BINDER_SET_CONTEXT_MGR_EXT => {
                this.set_as_manager(Some(reader.read()?), &thread)?
            }
            _ => return Err(EINVAL),
        }
        Ok(0)
    }

    /// Ioctls that are read/write from the perspective of userspace.
    ///
    /// The kernel will both read from and write to the pointer that userspace provided to us.
    fn ioctl_write_read(
        this: ArcBorrow<'_, Process>,
        file: &File,
        cmd: u32,
        data: UserSlice,
    ) -> Result<i32> {
        let thread = this.get_current_thread()?;
        let blocking = (file.flags() & file::flags::O_NONBLOCK) == 0;
        match cmd {
            bindings::BINDER_WRITE_READ => thread.write_read(data, blocking)?,
            bindings::BINDER_GET_NODE_DEBUG_INFO => this.get_node_debug_info(data)?,
            bindings::BINDER_GET_NODE_INFO_FOR_REF => this.get_node_info_from_ref(data)?,
            bindings::BINDER_VERSION => this.version(data)?,
            bindings::BINDER_GET_EXTENDED_ERROR => thread.get_extended_error(data)?,
            _ => return Err(EINVAL),
        }
        Ok(0)
    }
}

/// The file operations supported by `Process`.
impl Process {
    pub(crate) fn open(ctx: ArcBorrow<'_, Context>, file: &File) -> Result<Arc<Process>> {
        Self::new(ctx.into(), ARef::from(file.cred()))
    }

    pub(crate) fn release(this: Arc<Process>, _file: &File) {
        let should_schedule;
        {
            let mut inner = this.inner.lock();
            should_schedule = inner.defer_work == 0;
            inner.defer_work |= PROC_DEFER_RELEASE;
        }

        if should_schedule {
            // Ignore failures to schedule to the workqueue. Those just mean that we're already
            // scheduled for execution.
            let _ = workqueue::system().enqueue(this);
        }
    }

    pub(crate) fn ioctl(
        this: ArcBorrow<'_, Process>,
        file: &File,
        cmd: u32,
        arg: *mut core::ffi::c_void,
    ) -> Result<i32> {
        use kernel::ioctl::{_IOC_DIR, _IOC_SIZE};
        use kernel::uapi::{_IOC_READ, _IOC_WRITE};

        let user_slice = UserSlice::new(arg, _IOC_SIZE(cmd));

        const _IOC_READ_WRITE: u32 = _IOC_READ | _IOC_WRITE;

        match _IOC_DIR(cmd) {
            _IOC_WRITE => Self::ioctl_write_only(this, file, cmd, &mut user_slice.reader()),
            _IOC_READ_WRITE => Self::ioctl_write_read(this, file, cmd, user_slice),
            _ => Err(EINVAL),
        }
    }

    pub(crate) fn compat_ioctl(
        this: ArcBorrow<'_, Process>,
        file: &File,
        cmd: u32,
        arg: *mut core::ffi::c_void,
    ) -> Result<i32> {
        Self::ioctl(this, file, cmd, arg)
    }

    pub(crate) fn mmap(
        this: ArcBorrow<'_, Process>,
        _file: &File,
        vma: &mut mm::virt::Area,
    ) -> Result {
        // We don't allow mmap to be used in a different process.
        if !core::ptr::eq(kernel::current!().group_leader(), &*this.task) {
            return Err(EINVAL);
        }
        if vma.start() == 0 {
            return Err(EINVAL);
        }
        let mut flags = vma.flags();
        use mm::virt::flags::*;
        if flags & WRITE != 0 {
            return Err(EPERM);
        }
        flags |= DONTCOPY | MIXEDMAP;
        flags &= !MAYWRITE;
        vma.set_flags(flags);
        // TODO: Set ops. We need to learn when the user unmaps so that we can stop using it.
        this.create_mapping(vma)
    }

    pub(crate) fn poll(
        _this: ArcBorrow<'_, Process>,
        _file: &File,
        _table: &mut PollTable,
    ) -> Result<u32> {
        Err(EINVAL)
    }
}

/// Represents that a thread has registered with the `ready_threads` list of its process.
///
/// The destructor of this type will unregister the thread from the list of ready threads.
pub(crate) struct Registration<'a> {
    thread: &'a Arc<Thread>,
}

impl<'a> Registration<'a> {
    fn new(
        thread: &'a Arc<Thread>,
        guard: &mut Guard<'_, ProcessInner, kernel::sync::lock::spinlock::SpinLockBackend>,
    ) -> Self {
        assert!(core::ptr::eq(&thread.process.inner, guard.lock()));
        // INVARIANT: We are pushing this thread to the right `ready_threads` list.
        if let Ok(list_arc) = ListArc::try_from_arc(thread.clone()) {
            guard.ready_threads.push_front(list_arc);
        } else {
            // It is an error to hit this branch, and it should not be reachable. We try to do
            // something reasonable when the failure path happens. Most likely, the thread in
            // question will sleep forever.
            pr_err!("Same thread registered with `ready_threads` twice.");
        }
        Self { thread }
    }
}

impl Drop for Registration<'_> {
    fn drop(&mut self) {
        let mut inner = self.thread.process.inner.lock();
        // SAFETY: The thread has the invariant that we never push it to any other linked list than
        // the `ready_threads` list of its parent process. Therefore, the thread is either in that
        // list, or in no list.
        unsafe { inner.ready_threads.remove(self.thread) };
    }
}
