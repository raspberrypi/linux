// SPDX-License-Identifier: GPL-2.0

//! Files and file descriptors.
//!
//! C headers: [`include/linux/fs.h`](srctree/include/linux/fs.h) and
//! [`include/linux/file.h`](srctree/include/linux/file.h)

use crate::{
    bindings,
    cred::Credential,
    error::{code::*, Error, Result},
    types::{ARef, AlwaysRefCounted, NotThreadSafe, Opaque},
};
use alloc::boxed::Box;
use core::{alloc::AllocError, mem, ptr};

/// Flags associated with a [`File`].
pub mod flags {
    /// File is opened in append mode.
    pub const O_APPEND: u32 = bindings::O_APPEND;

    /// Signal-driven I/O is enabled.
    pub const O_ASYNC: u32 = bindings::FASYNC;

    /// Close-on-exec flag is set.
    pub const O_CLOEXEC: u32 = bindings::O_CLOEXEC;

    /// File was created if it didn't already exist.
    pub const O_CREAT: u32 = bindings::O_CREAT;

    /// Direct I/O is enabled for this file.
    pub const O_DIRECT: u32 = bindings::O_DIRECT;

    /// File must be a directory.
    pub const O_DIRECTORY: u32 = bindings::O_DIRECTORY;

    /// Like [`O_SYNC`] except metadata is not synced.
    pub const O_DSYNC: u32 = bindings::O_DSYNC;

    /// Ensure that this file is created with the `open(2)` call.
    pub const O_EXCL: u32 = bindings::O_EXCL;

    /// Large file size enabled (`off64_t` over `off_t`).
    pub const O_LARGEFILE: u32 = bindings::O_LARGEFILE;

    /// Do not update the file last access time.
    pub const O_NOATIME: u32 = bindings::O_NOATIME;

    /// File should not be used as process's controlling terminal.
    pub const O_NOCTTY: u32 = bindings::O_NOCTTY;

    /// If basename of path is a symbolic link, fail open.
    pub const O_NOFOLLOW: u32 = bindings::O_NOFOLLOW;

    /// File is using nonblocking I/O.
    pub const O_NONBLOCK: u32 = bindings::O_NONBLOCK;

    /// File is using nonblocking I/O.
    ///
    /// This is effectively the same flag as [`O_NONBLOCK`] on all architectures
    /// except SPARC64.
    pub const O_NDELAY: u32 = bindings::O_NDELAY;

    /// Used to obtain a path file descriptor.
    pub const O_PATH: u32 = bindings::O_PATH;

    /// Write operations on this file will flush data and metadata.
    pub const O_SYNC: u32 = bindings::O_SYNC;

    /// This file is an unnamed temporary regular file.
    pub const O_TMPFILE: u32 = bindings::O_TMPFILE;

    /// File should be truncated to length 0.
    pub const O_TRUNC: u32 = bindings::O_TRUNC;

    /// Bitmask for access mode flags.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::file;
    /// # fn do_something() {}
    /// # let flags = 0;
    /// if (flags & file::flags::O_ACCMODE) == file::flags::O_RDONLY {
    ///     do_something();
    /// }
    /// ```
    pub const O_ACCMODE: u32 = bindings::O_ACCMODE;

    /// File is read only.
    pub const O_RDONLY: u32 = bindings::O_RDONLY;

    /// File is write only.
    pub const O_WRONLY: u32 = bindings::O_WRONLY;

    /// File can be both read and written.
    pub const O_RDWR: u32 = bindings::O_RDWR;
}

/// Wraps the kernel's `struct file`.
///
/// This represents an open file rather than a file on a filesystem. Processes generally reference
/// open files using file descriptors. However, file descriptors are not the same as files. A file
/// descriptor is just an integer that corresponds to a file, and a single file may be referenced
/// by multiple file descriptors.
///
/// # Refcounting
///
/// Instances of this type are reference-counted. The reference count is incremented by the
/// `fget`/`get_file` functions and decremented by `fput`. The Rust type `ARef<File>` represents a
/// pointer that owns a reference count on the file.
///
/// Whenever a process opens a file descriptor (fd), it stores a pointer to the file in its `struct
/// files_struct`. This pointer owns a reference count to the file, ensuring the file isn't
/// prematurely deleted while the file descriptor is open. In Rust terminology, the pointers in
/// `struct files_struct` are `ARef<File>` pointers.
///
/// ## Light refcounts
///
/// Whenever a process has an fd to a file, it may use something called a "light refcount" as a
/// performance optimization. Light refcounts are acquired by calling `fdget` and released with
/// `fdput`. The idea behind light refcounts is that if the fd is not closed between the calls to
/// `fdget` and `fdput`, then the refcount cannot hit zero during that time, as the `struct
/// files_struct` holds a reference until the fd is closed. This means that it's safe to access the
/// file even if `fdget` does not increment the refcount.
///
/// The requirement that the fd is not closed during a light refcount applies globally across all
/// threads - not just on the thread using the light refcount. For this reason, light refcounts are
/// only used when the `struct files_struct` is not shared with other threads, since this ensures
/// that other unrelated threads cannot suddenly start using the fd and close it. Therefore,
/// calling `fdget` on a shared `struct files_struct` creates a normal refcount instead of a light
/// refcount.
///
/// Light reference counts must be released with `fdput` before the system call returns to
/// userspace. This means that if you wait until the current system call returns to userspace, then
/// all light refcounts that existed at the time have gone away.
///
/// ## Rust references
///
/// The reference type `&File` is similar to light refcounts:
///
/// * `&File` references don't own a reference count. They can only exist as long as the reference
///   count stays positive, and can only be created when there is some mechanism in place to ensure
///   this.
///
/// * The Rust borrow-checker normally ensures this by enforcing that the `ARef<File>` from which
///   a `&File` is created outlives the `&File`.
///
/// * Using the unsafe [`File::from_ptr`] means that it is up to the caller to ensure that the
///   `&File` only exists while the reference count is positive.
///
/// * You can think of `fdget` as using an fd to look up an `ARef<File>` in the `struct
///   files_struct` and create an `&File` from it. The "fd cannot be closed" rule is like the Rust
///   rule "the `ARef<File>` must outlive the `&File`".
///
/// # Invariants
///
/// * Instances of this type are refcounted using the `f_count` field.
/// * If an fd with active light refcounts is closed, then it must be the case that the file
///   refcount is positive until all light refcounts of the fd have been dropped.
/// * A light refcount must be dropped before returning to userspace.
#[repr(transparent)]
pub struct File(Opaque<bindings::file>);

// SAFETY:
// - `File::dec_ref` can be called from any thread.
// - It is okay to send ownership of `struct file` across thread boundaries.
unsafe impl Send for File {}

// SAFETY: All methods defined on `File` that take `&self` are safe to call even if other threads
// are concurrently accessing the same `struct file`, because those methods either access immutable
// properties or have proper synchronization to ensure that such accesses are safe.
unsafe impl Sync for File {}

impl File {
    /// Constructs a new `struct file` wrapper from a file descriptor.
    ///
    /// The file descriptor belongs to the current process.
    pub fn fget(fd: u32) -> Result<ARef<Self>, BadFdError> {
        // SAFETY: FFI call, there are no requirements on `fd`.
        let ptr = ptr::NonNull::new(unsafe { bindings::fget(fd) }).ok_or(BadFdError)?;

        // SAFETY: `bindings::fget` either returns null or a valid pointer to a file, and we
        // checked for null above.
        //
        // INVARIANT: `bindings::fget` creates a refcount, and we pass ownership of the refcount to
        // the new `ARef<File>`.
        Ok(unsafe { ARef::from_raw(ptr.cast()) })
    }

    /// Creates a reference to a [`File`] from a valid pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` points at a valid file and that the file's refcount is
    /// positive for the duration of 'a.
    pub unsafe fn from_ptr<'a>(ptr: *const bindings::file) -> &'a File {
        // SAFETY: The caller guarantees that the pointer is not dangling and stays valid for the
        // duration of 'a. The cast is okay because `File` is `repr(transparent)`.
        //
        // INVARIANT: The safety requirements guarantee that the refcount does not hit zero during
        // 'a.
        unsafe { &*ptr.cast() }
    }

    /// Returns a raw pointer to the inner C struct.
    #[inline]
    pub fn as_ptr(&self) -> *mut bindings::file {
        self.0.get()
    }

    /// Returns the credentials of the task that originally opened the file.
    pub fn cred(&self) -> &Credential {
        // SAFETY: It's okay to read the `f_cred` field without synchronization because `f_cred` is
        // never changed after initialization of the file.
        let ptr = unsafe { (*self.as_ptr()).f_cred };

        // SAFETY: The signature of this function ensures that the caller will only access the
        // returned credential while the file is still valid, and the C side ensures that the
        // credential stays valid at least as long as the file.
        unsafe { Credential::from_ptr(ptr) }
    }

    /// Returns the flags associated with the file.
    ///
    /// The flags are a combination of the constants in [`flags`].
    pub fn flags(&self) -> u32 {
        // This `read_volatile` is intended to correspond to a READ_ONCE call.
        //
        // SAFETY: The file is valid because the shared reference guarantees a nonzero refcount.
        //
        // FIXME(read_once): Replace with `read_once` when available on the Rust side.
        unsafe { core::ptr::addr_of!((*self.as_ptr()).f_flags).read_volatile() }
    }
}

// SAFETY: The type invariants guarantee that `File` is always ref-counted. This implementation
// makes `ARef<File>` own a normal refcount.
unsafe impl AlwaysRefCounted for File {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::get_file(self.as_ptr()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<File>) {
        // SAFETY: To call this method, the caller passes us ownership of a normal refcount, so we
        // may drop it. The cast is okay since `File` has the same representation as `struct file`.
        unsafe { bindings::fput(obj.cast().as_ptr()) }
    }
}

/// A file descriptor reservation.
///
/// This allows the creation of a file descriptor in two steps: first, we reserve a slot for it,
/// then we commit or drop the reservation. The first step may fail (e.g., the current process ran
/// out of available slots), but commit and drop never fail (and are mutually exclusive).
///
/// Dropping the reservation happens in the destructor of this type.
///
/// # Invariants
///
/// The fd stored in this struct must correspond to a reserved file descriptor of the current task.
pub struct FileDescriptorReservation {
    fd: u32,
    /// Prevent values of this type from being moved to a different task.
    ///
    /// The `fd_install` and `put_unused_fd` functions assume that the value of `current` is
    /// unchanged since the call to `get_unused_fd_flags`. By adding this marker to this type, we
    /// prevent it from being moved across task boundaries, which ensures that `current` does not
    /// change while this value exists.
    _not_send: NotThreadSafe,
}

impl FileDescriptorReservation {
    /// Creates a new file descriptor reservation.
    pub fn get_unused_fd_flags(flags: u32) -> Result<Self> {
        // SAFETY: FFI call, there are no safety requirements on `flags`.
        let fd: i32 = unsafe { bindings::get_unused_fd_flags(flags) };
        if fd < 0 {
            return Err(Error::from_errno(fd));
        }
        Ok(Self {
            fd: fd as u32,
            _not_send: NotThreadSafe,
        })
    }

    /// Returns the file descriptor number that was reserved.
    pub fn reserved_fd(&self) -> u32 {
        self.fd
    }

    /// Commits the reservation.
    ///
    /// The previously reserved file descriptor is bound to `file`. This method consumes the
    /// [`FileDescriptorReservation`], so it will not be usable after this call.
    pub fn fd_install(self, file: ARef<File>) {
        // SAFETY: `self.fd` was previously returned by `get_unused_fd_flags`. We have not yet used
        // the fd, so it is still valid, and `current` still refers to the same task, as this type
        // cannot be moved across task boundaries.
        //
        // Furthermore, the file pointer is guaranteed to own a refcount by its type invariants,
        // and we take ownership of that refcount by not running the destructor below.
        unsafe { bindings::fd_install(self.fd, file.as_ptr()) };

        // `fd_install` consumes both the file descriptor and the file reference, so we cannot run
        // the destructors.
        core::mem::forget(self);
        core::mem::forget(file);
    }
}

impl Drop for FileDescriptorReservation {
    fn drop(&mut self) {
        // SAFETY: By the type invariants of this type, `self.fd` was previously returned by
        // `get_unused_fd_flags`. We have not yet used the fd, so it is still valid, and `current`
        // still refers to the same task, as this type cannot be moved across task boundaries.
        unsafe { bindings::put_unused_fd(self.fd) };
    }
}

/// Helper used for closing file descriptors in a way that is safe even if the file is currently
/// held using `fdget`.
///
/// Additional motivation can be found in commit 80cd795630d6 ("binder: fix use-after-free due to
/// ksys_close() during fdget()") and in the comments on `binder_do_fd_close`.
pub struct DeferredFdCloser {
    inner: Box<DeferredFdCloserInner>,
}

/// SAFETY: This just holds an allocation with no real content, so there's no safety issue with
/// moving it across threads.
unsafe impl Send for DeferredFdCloser {}
unsafe impl Sync for DeferredFdCloser {}

/// # Invariants
///
/// If the `file` pointer is non-null, then it points at a `struct file` and owns a refcount to
/// that file.
#[repr(C)]
struct DeferredFdCloserInner {
    twork: mem::MaybeUninit<bindings::callback_head>,
    file: *mut bindings::file,
}

impl DeferredFdCloser {
    /// Create a new [`DeferredFdCloser`].
    pub fn new() -> Result<Self, AllocError> {
        Ok(Self {
            // INVARIANT: The `file` pointer is null, so the type invariant does not apply.
            inner: Box::try_new(DeferredFdCloserInner {
                twork: mem::MaybeUninit::uninit(),
                file: core::ptr::null_mut(),
            })?,
        })
    }

    /// Schedule a task work that closes the file descriptor when this task returns to userspace.
    ///
    /// Fails if this is called from a context where we cannot run work when returning to
    /// userspace. (E.g., from a kthread.)
    pub fn close_fd(self, fd: u32) -> Result<(), DeferredFdCloseError> {
        use bindings::task_work_notify_mode_TWA_RESUME as TWA_RESUME;

        // In this method, we schedule the task work before closing the file. This is because
        // scheduling a task work is fallible, and we need to know whether it will fail before we
        // attempt to close the file.

        // Task works are not available on kthreads.
        let current = crate::current!();
        if current.is_kthread() {
            return Err(DeferredFdCloseError::TaskWorkUnavailable);
        }

        // Transfer ownership of the box's allocation to a raw pointer. This disables the
        // destructor, so we must manually convert it back to a Box to drop it.
        //
        // Until we convert it back to a `Box`, there are no aliasing requirements on this
        // pointer.
        let inner = Box::into_raw(self.inner);

        // The `callback_head` field is first in the struct, so this cast correctly gives us a
        // pointer to the field.
        let callback_head = inner.cast::<bindings::callback_head>();
        // SAFETY: This pointer offset operation does not go out-of-bounds.
        let file_field = unsafe { core::ptr::addr_of_mut!((*inner).file) };

        let current = current.as_raw();

        // SAFETY: This function currently has exclusive access to the `DeferredFdCloserInner`, so
        // it is okay for us to perform unsynchronized writes to its `callback_head` field.
        unsafe { bindings::init_task_work(callback_head, Some(Self::do_close_fd)) };

        // SAFETY: This inserts the `DeferredFdCloserInner` into the task workqueue for the current
        // task. If this operation is successful, then this transfers exclusive ownership of the
        // `callback_head` field to the C side until it calls `do_close_fd`, and we don't touch or
        // invalidate the field during that time.
        //
        // When the C side calls `do_close_fd`, the safety requirements of that method are
        // satisfied because when a task work is executed, the callback is given ownership of the
        // pointer.
        //
        // The file pointer is currently null. If it is changed to be non-null before `do_close_fd`
        // is called, then that change happens due to the write at the end of this function, and
        // that write has a safety comment that explains why the refcount can be dropped when
        // `do_close_fd` runs.
        let res = unsafe { bindings::task_work_add(current, callback_head, TWA_RESUME) };

        if res != 0 {
            // SAFETY: Scheduling the task work failed, so we still have ownership of the box, so
            // we may destroy it.
            unsafe { drop(Box::from_raw(inner)) };

            return Err(DeferredFdCloseError::TaskWorkUnavailable);
        }

        // This removes the fd from the fd table in `current`. The file is not fully closed until
        // `filp_close` is called. We are given ownership of one refcount to the file.
        //
        // SAFETY: This is safe no matter what `fd` is. If the `fd` is valid (that is, if the
        // pointer is non-null), then we call `filp_close` on the returned pointer as required by
        // `close_fd_get_file`.
        let file = unsafe { bindings::close_fd_get_file(fd) };
        if file.is_null() {
            // We don't clean up the task work since that might be expensive if the task work queue
            // is long. Just let it execute and let it clean up for itself.
            return Err(DeferredFdCloseError::BadFd);
        }

        // Acquire a second refcount to the file.
        //
        // SAFETY: The `file` pointer points at a file with a non-zero refcount.
        unsafe { bindings::get_file(file) };

        // This method closes the fd, consuming one of our two refcounts. There could be active
        // light refcounts created from that fd, so we must ensure that the file has a positive
        // refcount for the duration of those active light refcounts. We do that by holding on to
        // the second refcount until the current task returns to userspace.
        //
        // SAFETY: The `file` pointer is valid. Passing `current->files` as the file table to close
        // it in is correct, since we just got the `fd` from `close_fd_get_file` which also uses
        // `current->files`.
        //
        // Note: fl_owner_t is currently a void pointer.
        unsafe { bindings::filp_close(file, (*current).files as bindings::fl_owner_t) };

        // We update the file pointer that the task work is supposed to fput. This transfers
        // ownership of our last refcount.
        //
        // INVARIANT: This changes the `file` field of a `DeferredFdCloserInner` from null to
        // non-null. This doesn't break the type invariant for `DeferredFdCloserInner` because we
        // still own a refcount to the file, so we can pass ownership of that refcount to the
        // `DeferredFdCloserInner`.
        //
        // When `do_close_fd` runs, it must be safe for it to `fput` the refcount. However, this is
        // the case because all light refcounts that are associated with the fd we closed
        // previously must be dropped when `do_close_fd`, since light refcounts must be dropped
        // before returning to userspace.
        //
        // SAFETY: Task works are executed on the current thread right before we return to
        // userspace, so this write is guaranteed to happen before `do_close_fd` is called, which
        // means that a race is not possible here.
        unsafe { *file_field = file };

        Ok(())
    }

    /// # Safety
    ///
    /// The provided pointer must point at the `twork` field of a `DeferredFdCloserInner` stored in
    /// a `Box`, and the caller must pass exclusive ownership of that `Box`. Furthermore, if the
    /// file pointer is non-null, then it must be okay to release the refcount by calling `fput`.
    unsafe extern "C" fn do_close_fd(inner: *mut bindings::callback_head) {
        // SAFETY: The caller just passed us ownership of this box.
        let inner = unsafe { Box::from_raw(inner.cast::<DeferredFdCloserInner>()) };
        if !inner.file.is_null() {
            // SAFETY: By the type invariants, we own a refcount to this file, and the caller
            // guarantees that dropping the refcount now is okay.
            unsafe { bindings::fput(inner.file) };
        }
        // The allocation is freed when `inner` goes out of scope.
    }
}

/// Represents a failure to close an fd in a deferred manner.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum DeferredFdCloseError {
    /// Closing the fd failed because we were unable to schedule a task work.
    TaskWorkUnavailable,
    /// Closing the fd failed because the fd does not exist.
    BadFd,
}

impl From<DeferredFdCloseError> for Error {
    fn from(err: DeferredFdCloseError) -> Error {
        match err {
            DeferredFdCloseError::TaskWorkUnavailable => ESRCH,
            DeferredFdCloseError::BadFd => EBADF,
        }
    }
}

/// Represents the `EBADF` error code.
///
/// Used for methods that can only fail with `EBADF`.
#[derive(Copy, Clone, Eq, PartialEq)]
pub struct BadFdError;

impl From<BadFdError> for Error {
    fn from(_: BadFdError) -> Error {
        EBADF
    }
}

impl core::fmt::Debug for BadFdError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.pad("EBADF")
    }
}
