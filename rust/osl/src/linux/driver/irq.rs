//! Linux irq implementation
//!

pub use kernel::irq::Return;

impl From<crate::driver::irq::ReturnEnum> for Return {
    fn from(ret: crate::driver::irq::ReturnEnum) -> Self {
        match ret {
            crate::driver::irq::ReturnEnum::None => Return::None,
            crate::driver::irq::ReturnEnum::Handled => Return::Handled,
            crate::driver::irq::ReturnEnum::WakeThread => Return::WakeThread,
        }
    }
}
