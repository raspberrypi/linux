//! Defines the OS Layper general irq Api
//!

#[cfg(feature = "linux")]
pub use crate::linux::driver::irq::Return;

/// The general irq return type.
/// The return value from interrupt handlers.
///
/// Every OS should provide Return type and an ReturnEnum conversion method
/// such as the following implementation on Linux.
///
pub enum ReturnEnum {
    /// The interrupt was not from this device or was not handled.
    None,
    /// The interrupt was handled by this device.
    Handled,
    /// The handler wants the handler thread to wake up.
    WakeThread,
}

/// Give an errno, return OS Error
///
/// #Examples 
///
/// ```
/// use osl::driver::irq;
///
/// irq_handler() -> irq::Return {
///     return to_irq_return(irq::ReturnEnum::None); 
/// }
/// ```
///
pub fn to_irq_return(val: ReturnEnum) -> Return {
    Return::from(val)
}
