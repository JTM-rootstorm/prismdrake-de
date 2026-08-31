// SPDX-License-Identifier: GPL-3.0-or-later

//! Toolkit-neutral foundation types shared by Prismdrake's Rust components.

#![forbid(unsafe_code)]

pub mod settings;

use std::num::NonZeroU64;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

/// Stable machine-readable failure categories at Prismdrake boundaries.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
#[repr(u8)]
pub enum ErrorCode {
    /// A caller supplied an invalid value.
    InvalidArgument,
    /// Input syntax could not be parsed.
    SyntaxError,
    /// Parsed input failed semantic validation.
    ValidationError,
    /// Required environment state is absent or invalid.
    InvalidEnvironment,
    /// A required object was not found.
    NotFound,
    /// The caller lacks permission for the operation.
    PermissionDenied,
    /// A bounded resource limit was exceeded.
    TooLarge,
    /// An input/output operation failed.
    IoError,
    /// A write may have completed without the required durability proof.
    DurabilityUncertain,
    /// The requested behavior is unsupported.
    Unsupported,
    /// The operation was cancelled.
    Cancelled,
}

/// Bounded actionable failure details for internal foundation operations.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Error {
    /// Stable failure category.
    pub code: ErrorCode,
    /// Bounded user-facing failure summary.
    pub message: String,
    /// Bounded recovery guidance.
    pub recovery: String,
}

impl Error {
    fn generation(code: ErrorCode, message: &'static str, recovery: &'static str) -> Self {
        Self {
            code,
            message: message.into(),
            recovery: recovery.into(),
        }
    }
}

/// Identifies one complete, published settings or theme snapshot.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Generation(NonZeroU64);

impl Generation {
    /// The first value that may identify a published snapshot.
    pub const FIRST_PUBLISHED: Self = Self(NonZeroU64::MIN);

    /// Validates a raw published generation.
    ///
    /// # Errors
    ///
    /// Returns [`ErrorCode::InvalidArgument`] when `value` is zero, which is
    /// reserved for unpublished state.
    pub fn from_published(value: u64) -> Result<Self, Error> {
        NonZeroU64::new(value).map_or_else(
            || {
                Err(Error::generation(
                    ErrorCode::InvalidArgument,
                    "generation zero is not published",
                    "publish a complete snapshot before assigning its generation",
                ))
            },
            |value| Ok(Self(value)),
        )
    }

    /// Returns the nonzero wire value.
    #[must_use]
    pub const fn value(self) -> u64 {
        self.0.get()
    }

    /// Returns the next published generation without wrapping.
    ///
    /// # Errors
    ///
    /// Returns [`ErrorCode::TooLarge`] when the counter is exhausted.
    pub fn checked_next(self) -> Result<Self, Error> {
        self.value().checked_add(1).map_or_else(
            || {
                Err(Error::generation(
                    ErrorCode::TooLarge,
                    "generation counter is exhausted",
                    "stop publication and restart through a reviewed recovery path",
                ))
            },
            Self::from_published,
        )
    }
}

/// Stable process statuses used at Prismdrake executable boundaries.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
#[repr(i32)]
pub enum ExitStatus {
    /// Successful completion.
    Success = 0,
    /// Unclassified failure.
    GeneralFailure = 1,
    /// Invalid input or usage.
    InvalidUsage = 2,
    /// Required environment or implementation is unavailable.
    Unavailable = 3,
    /// Permission was denied.
    PermissionDenied = 4,
    /// A resource bound was exceeded.
    ResourceLimit = 5,
    /// Input/output or durability failure.
    IoFailure = 6,
    /// The operation was cancelled.
    Cancelled = 7,
}

impl ExitStatus {
    /// Returns the integer used by process boundaries.
    #[must_use]
    pub const fn process_code(self) -> i32 {
        self as i32
    }
}

impl From<ErrorCode> for ExitStatus {
    fn from(code: ErrorCode) -> Self {
        match code {
            ErrorCode::InvalidArgument | ErrorCode::SyntaxError | ErrorCode::ValidationError => {
                Self::InvalidUsage
            }
            ErrorCode::InvalidEnvironment | ErrorCode::NotFound | ErrorCode::Unsupported => {
                Self::Unavailable
            }
            ErrorCode::PermissionDenied => Self::PermissionDenied,
            ErrorCode::TooLarge => Self::ResourceLimit,
            ErrorCode::IoError | ErrorCode::DurabilityUncertain => Self::IoFailure,
            ErrorCode::Cancelled => Self::Cancelled,
        }
    }
}

impl From<&Error> for ExitStatus {
    fn from(error: &Error) -> Self {
        error.code.into()
    }
}

#[derive(Debug)]
struct CancellationState {
    requested: AtomicBool,
}

/// Copyable observer for one explicitly owned cancellation state.
#[derive(Clone, Debug)]
pub struct CancellationToken {
    state: Arc<CancellationState>,
}

impl CancellationToken {
    /// Reports whether the source has requested cancellation.
    #[must_use]
    pub fn is_cancellation_requested(&self) -> bool {
        self.state.requested.load(Ordering::Acquire)
    }
}

/// Sole cancellation initiator for every token sharing its atomic state.
#[derive(Debug)]
pub struct CancellationSource {
    state: Arc<CancellationState>,
}

impl CancellationSource {
    /// Creates an uncancelled source.
    #[must_use]
    pub fn new() -> Self {
        Self {
            state: Arc::new(CancellationState {
                requested: AtomicBool::new(false),
            }),
        }
    }

    /// Returns a token tied to this source's state.
    #[must_use]
    pub fn token(&self) -> CancellationToken {
        CancellationToken {
            state: Arc::clone(&self.state),
        }
    }

    /// Requests cancellation and returns `true` only for the first request.
    #[must_use]
    pub fn request_cancellation(&self) -> bool {
        self.state
            .requested
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }
}

impl Default for CancellationSource {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::{CancellationSource, ErrorCode, ExitStatus, Generation};
    use std::thread;

    #[test]
    fn generation_zero_is_unpublished() {
        let error = Generation::from_published(0).expect_err("zero must be rejected");
        assert_eq!(error.code, ErrorCode::InvalidArgument);
        assert_eq!(error.message, "generation zero is not published");
    }

    #[test]
    fn generation_is_ordered_and_advances_without_wrapping() {
        let first = Generation::FIRST_PUBLISHED;
        let second = first.checked_next().expect("one can advance");
        assert_eq!(first.value(), 1);
        assert_eq!(second.value(), 2);
        assert!(second > first);

        let exhausted = Generation::from_published(u64::MAX)
            .expect("maximum is published")
            .checked_next()
            .expect_err("maximum must not wrap");
        assert_eq!(exhausted.code, ErrorCode::TooLarge);
    }

    #[test]
    fn exit_status_mapping_matches_the_existing_process_contract() {
        let cases = [
            (ErrorCode::InvalidArgument, ExitStatus::InvalidUsage),
            (ErrorCode::SyntaxError, ExitStatus::InvalidUsage),
            (ErrorCode::ValidationError, ExitStatus::InvalidUsage),
            (ErrorCode::InvalidEnvironment, ExitStatus::Unavailable),
            (ErrorCode::NotFound, ExitStatus::Unavailable),
            (ErrorCode::Unsupported, ExitStatus::Unavailable),
            (ErrorCode::PermissionDenied, ExitStatus::PermissionDenied),
            (ErrorCode::TooLarge, ExitStatus::ResourceLimit),
            (ErrorCode::IoError, ExitStatus::IoFailure),
            (ErrorCode::DurabilityUncertain, ExitStatus::IoFailure),
            (ErrorCode::Cancelled, ExitStatus::Cancelled),
        ];

        for (code, expected) in cases {
            assert_eq!(ExitStatus::from(code), expected);
            assert_eq!(expected.process_code(), expected as i32);
        }
    }

    #[test]
    fn cancellation_is_shared_monotonic_and_first_writer_wins() {
        let source = CancellationSource::new();
        let token = source.token();
        let observer = token.clone();
        assert!(!token.is_cancellation_requested());

        let join = thread::spawn(move || {
            while !observer.is_cancellation_requested() {
                thread::yield_now();
            }
        });

        assert!(source.request_cancellation());
        assert!(!source.request_cancellation());
        join.join().expect("observer exits after cancellation");
        assert!(token.is_cancellation_requested());
    }
}
