// SPDX-License-Identifier: GPL-2.0

//! Generic devices that are part of the kernel's driver model.
//!
//! C header: [`include/linux/device.h`](../../../../include/linux/device.h)

use crate::{
    bindings,
    error::Result,
    macros::pin_data,
    pin_init, pr_crit,
    str::CStr,
    sync::{lock::mutex, lock::Guard, LockClassKey, Mutex, UniqueArc},
};
use core::{
    fmt,
    ops::{Deref, DerefMut},
    pin::Pin,
};

use crate::init::InPlaceInit;

#[cfg(CONFIG_PRINTK)]
use crate::c_str;

/// A raw device.
///
/// # Safety
///
/// Implementers must ensure that the `*mut device` returned by [`RawDevice::raw_device`] is
/// related to `self`, that is, actions on it will affect `self`. For example, if one calls
/// `get_device`, then the refcount on the device represented by `self` will be incremented.
///
/// Additionally, implementers must ensure that the device is never renamed. Commit a5462516aa99
/// ("driver-core: document restrictions on device_rename()") has details on why `device_rename`
/// should not be used.
pub unsafe trait RawDevice {
    /// Returns the raw `struct device` related to `self`.
    fn raw_device(&self) -> *mut bindings::device;

    /// Returns the name of the device.
    fn name(&self) -> &CStr {
        let ptr = self.raw_device();

        // SAFETY: `ptr` is valid because `self` keeps it alive.
        let name = unsafe { bindings::dev_name(ptr) };

        // SAFETY: The name of the device remains valid while it is alive (because the device is
        // never renamed, per the safety requirement of this trait). This is guaranteed to be the
        // case because the reference to `self` outlives the one of the returned `CStr` (enforced
        // by the compiler because of their lifetimes).
        unsafe { CStr::from_char_ptr(name) }
    }

    /// Prints an emergency-level message (level 0) prefixed with device information.
    ///
    /// More details are available from [`dev_emerg`].
    ///
    /// [`dev_emerg`]: crate::dev_emerg
    fn pr_emerg(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_EMERG, args) };
    }

    /// Prints an alert-level message (level 1) prefixed with device information.
    ///
    /// More details are available from [`dev_alert`].
    ///
    /// [`dev_alert`]: crate::dev_alert
    fn pr_alert(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_ALERT, args) };
    }

    /// Prints a critical-level message (level 2) prefixed with device information.
    ///
    /// More details are available from [`dev_crit`].
    ///
    /// [`dev_crit`]: crate::dev_crit
    fn pr_crit(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_CRIT, args) };
    }

    /// Prints an error-level message (level 3) prefixed with device information.
    ///
    /// More details are available from [`dev_err`].
    ///
    /// [`dev_err`]: crate::dev_err
    fn pr_err(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_ERR, args) };
    }

    /// Prints a warning-level message (level 4) prefixed with device information.
    ///
    /// More details are available from [`dev_warn`].
    ///
    /// [`dev_warn`]: crate::dev_warn
    fn pr_warn(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_WARNING, args) };
    }

    /// Prints a notice-level message (level 5) prefixed with device information.
    ///
    /// More details are available from [`dev_notice`].
    ///
    /// [`dev_notice`]: crate::dev_notice
    fn pr_notice(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_NOTICE, args) };
    }

    /// Prints an info-level message (level 6) prefixed with device information.
    ///
    /// More details are available from [`dev_info`].
    ///
    /// [`dev_info`]: crate::dev_info
    fn pr_info(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_INFO, args) };
    }

    /// Prints a debug-level message (level 7) prefixed with device information.
    ///
    /// More details are available from [`dev_dbg`].
    ///
    /// [`dev_dbg`]: crate::dev_dbg
    fn pr_dbg(&self, args: fmt::Arguments<'_>) {
        if cfg!(debug_assertions) {
            // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
            unsafe { self.printk(bindings::KERN_DEBUG, args) };
        }
    }

    /// Prints the provided message to the console.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `klevel` is null-terminated; in particular, one of the
    /// `KERN_*`constants, for example, `KERN_CRIT`, `KERN_ALERT`, etc.
    #[cfg_attr(not(CONFIG_PRINTK), allow(unused_variables))]
    unsafe fn printk(&self, klevel: &[u8], msg: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated and one of the kernel constants. `self.raw_device`
        // is valid because `self` is valid. The "%pA" format string expects a pointer to
        // `fmt::Arguments`, which is what we're passing as the last argument.
        #[cfg(CONFIG_PRINTK)]
        unsafe {
            bindings::_dev_printk(
                klevel as *const _ as *const core::ffi::c_char,
                self.raw_device(),
                c_str!("%pA").as_char_ptr(),
                &msg as *const _ as *const core::ffi::c_void,
            )
        };
    }
}

/// A ref-counted device.
///
/// # Invariants
///
/// `ptr` is valid, non-null, and has a non-zero reference count. One of the references is owned by
/// `self`, and will be decremented when `self` is dropped.
pub struct Device {
    pub(crate) ptr: *mut bindings::device,
}

// SAFETY: `Device` only holds a pointer to a C device, which is safe to be used from any thread.
unsafe impl Send for Device {}

// SAFETY: `Device` only holds a pointer to a C device, references to which are safe to be used
// from any thread.
unsafe impl Sync for Device {}

impl Device {
    /// Creates a new device instance.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count.
    pub unsafe fn new(ptr: *mut bindings::device) -> Self {
        // SAFETY: By the safety requirements, ptr is valid and its refcounted will be incremented.
        unsafe { bindings::get_device(ptr) };
        // INVARIANT: The safety requirements satisfy all but one invariant, which is that `self`
        // owns a reference. This is satisfied by the call to `get_device` above.
        Self { ptr }
    }

    /// Creates a new device instance from an existing [`RawDevice`] instance.
    pub fn from_dev(dev: &dyn RawDevice) -> Self {
        // SAFETY: The requirements are satisfied by the existence of `RawDevice` and its safety
        // requirements.
        unsafe { Self::new(dev.raw_device()) }
    }
}

// SAFETY: The device returned by `raw_device` is the one for which we hold a reference.
unsafe impl RawDevice for Device {
    fn raw_device(&self) -> *mut bindings::device {
        self.ptr
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // relinquish it now.
        unsafe { bindings::put_device(self.ptr) };
    }
}

impl Clone for Device {
    fn clone(&self) -> Self {
        Device::from_dev(self)
    }
}

/// Device data.
///
/// When a device is removed (for whatever reason, for example, because the device was unplugged or
/// because the user decided to unbind the driver), the driver is given a chance to clean its state
/// up, and all io resources should ideally not be used anymore.
///
/// However, the device data is reference-counted because other subsystems hold pointers to it. So
/// some device state must be freed and not used anymore, while others must remain accessible.
///
/// This struct separates the device data into three categories:
///   1. Registrations: are destroyed when the device is removed, but before the io resources
///      become inaccessible.
///   2. Io resources: are available until the device is removed.
///   3. General data: remain available as long as the ref count is nonzero.
///
/// This struct implements the `DeviceRemoval` trait so that it can clean resources up even if not
/// explicitly called by the device drivers.
#[pin_data]
pub struct Data<T, U, V> {
    #[pin]
    registrations: Mutex<T>,
    resources: U,
    general: V,
}

/// Safely creates an new reference-counted instance of [`Data`].
#[doc(hidden)]
#[macro_export]
macro_rules! new_device_data {
    ($reg:expr, $res:expr, $gen:expr, $name:literal) => {{
        static CLASS1: $crate::sync::LockClassKey = $crate::static_lock_class!();
        let regs = $reg;
        let res = $res;
        let gen = $gen;
        let name = $crate::c_str!($name);
        $crate::device::Data::try_new(regs, res, gen, name, CLASS1)
    }};
}

impl<T, U, V> Data<T, U, V> {
    /// Creates a new instance of `Data`.
    ///
    /// It is recommended that the [`new_device_data`] macro be used as it automatically creates
    /// the lock classes.
    pub fn try_new(
        registrations: T,
        resources: U,
        general: V,
        name: &'static CStr,
        key1: LockClassKey,
    ) -> Result<Pin<UniqueArc<Self>>> {
        let ret = UniqueArc::pin_init(pin_init!(Self {
            registrations <- Mutex::new(registrations, name, key1),
            resources,
            general,
        }))?;
        Ok(ret)
    }

    /// Returns the resources if they're still available.
    pub fn resources(&self) -> Option<&U> {
        Some(&self.resources)
    }

    /// Returns the locked registrations if they're still available.
    pub fn registrations(&self) -> Option<Guard<'_, T, mutex::MutexBackend>> {
        Some(self.registrations.lock())
    }
}

impl<T, U, V> crate::driver::DeviceRemoval for Data<T, U, V> {
    fn device_remove(&self) {
        pr_crit!("Device removal not properly implemented!\n");
    }
}

impl<T, U, V> Deref for Data<T, U, V> {
    type Target = V;

    fn deref(&self) -> &V {
        &self.general
    }
}

impl<T, U, V> DerefMut for Data<T, U, V> {
    fn deref_mut(&mut self) -> &mut V {
        &mut self.general
    }
}

#[doc(hidden)]
#[macro_export]
macro_rules! dev_printk {
    ($method:ident, $dev:expr, $($f:tt)*) => {
        {
            // We have an explicity `use` statement here so that callers of this macro are not
            // required to explicitly use the `RawDevice` trait to use its functions.
            use $crate::device::RawDevice;
            ($dev).$method(core::format_args!($($f)*));
        }
    }
}

/// Prints an emergency-level message (level 0) prefixed with device information.
///
/// This level should be used if the system is unusable.
///
/// Equivalent to the kernel's `dev_emerg` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_emerg!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_emerg {
    ($($f:tt)*) => { $crate::dev_printk!(pr_emerg, $($f)*); }
}

/// Prints an alert-level message (level 1) prefixed with device information.
///
/// This level should be used if action must be taken immediately.
///
/// Equivalent to the kernel's `dev_alert` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_alert!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_alert {
    ($($f:tt)*) => { $crate::dev_printk!(pr_alert, $($f)*); }
}

/// Prints a critical-level message (level 2) prefixed with device information.
///
/// This level should be used in critical conditions.
///
/// Equivalent to the kernel's `dev_crit` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_crit!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_crit {
    ($($f:tt)*) => { $crate::dev_printk!(pr_crit, $($f)*); }
}

/// Prints an error-level message (level 3) prefixed with device information.
///
/// This level should be used in error conditions.
///
/// Equivalent to the kernel's `dev_err` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_err!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_err {
    ($($f:tt)*) => { $crate::dev_printk!(pr_err, $($f)*); }
}

/// Prints a warning-level message (level 4) prefixed with device information.
///
/// This level should be used in warning conditions.
///
/// Equivalent to the kernel's `dev_warn` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_warn!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_warn {
    ($($f:tt)*) => { $crate::dev_printk!(pr_warn, $($f)*); }
}

/// Prints a notice-level message (level 5) prefixed with device information.
///
/// This level should be used in normal but significant conditions.
///
/// Equivalent to the kernel's `dev_notice` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_notice!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_notice {
    ($($f:tt)*) => { $crate::dev_printk!(pr_notice, $($f)*); }
}

/// Prints an info-level message (level 6) prefixed with device information.
///
/// This level should be used for informational messages.
///
/// Equivalent to the kernel's `dev_info` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_info!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_info {
    ($($f:tt)*) => { $crate::dev_printk!(pr_info, $($f)*); }
}

/// Prints a debug-level message (level 7) prefixed with device information.
///
/// This level should be used for debug messages.
///
/// Equivalent to the kernel's `dev_dbg` macro, except that it doesn't support dynamic debug yet.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and [`alloc::format!`].
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_dbg!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_dbg {
    ($($f:tt)*) => { $crate::dev_printk!(pr_dbg, $($f)*); }
}
