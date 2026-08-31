// SPDX-License-Identifier: GPL-3.0-or-later

//! Toolkit-neutral settings publication contracts.

#![allow(clippy::module_name_repetitions)]

use crate::{Error, ErrorCode, Generation};
use std::{str::FromStr, sync::Arc};

/// Schema version emitted by the current complete runtime snapshot.
pub const RUNTIME_SNAPSHOT_SCHEMA_VERSION: u32 = 2;

/// Maximum serialized size of one complete runtime snapshot.
pub const MAXIMUM_RUNTIME_SNAPSHOT_BYTES: usize = 1024 * 1024;

/// Closed profile identifiers accepted at the settings boundary.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ProfileId {
    /// Prismdrake Lustre.
    Lustre,
    /// Prismdrake Forge.
    Forge,
}

impl ProfileId {
    /// Returns the stable wire identifier.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Lustre => "lustre",
            Self::Forge => "forge",
        }
    }
}

impl FromStr for ProfileId {
    type Err = Error;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "lustre" => Ok(Self::Lustre),
            "forge" => Ok(Self::Forge),
            _ => Err(Error {
                code: ErrorCode::InvalidArgument,
                message: "profile must be exactly lustre or forge".into(),
                recovery: "request exactly lustre or forge".into(),
            }),
        }
    }
}

/// Closed configuration provenance identifiers emitted in runtime snapshots.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ConfigurationSourceId {
    /// The accepted per-user configuration.
    User,
    /// The last-known-valid recovery copy.
    LastKnownValid,
    /// The packaged default configuration.
    PackagedDefault,
}

impl ConfigurationSourceId {
    /// Returns the stable wire identifier.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::User => "user",
            Self::LastKnownValid => "last_known_valid",
            Self::PackagedDefault => "packaged_default",
        }
    }
}

/// Settings domains in the canonical publication and change-reporting order.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
#[repr(u8)]
pub enum SettingsDomain {
    /// Selected profile.
    Profile,
    /// Appearance configuration.
    Appearance,
    /// Panel configuration.
    Panel,
    /// Launcher configuration.
    Launcher,
    /// Notification configuration.
    Notifications,
    /// Desktop configuration.
    Desktop,
    /// External integration configuration.
    Integration,
    /// Accessibility configuration.
    Accessibility,
    /// Keyboard configuration.
    Keyboard,
    /// Developer configuration.
    Developer,
    /// Resolved theme.
    Theme,
}

impl SettingsDomain {
    /// Every settings domain in deterministic wire order.
    pub const ALL: [Self; 11] = [
        Self::Profile,
        Self::Appearance,
        Self::Panel,
        Self::Launcher,
        Self::Notifications,
        Self::Desktop,
        Self::Integration,
        Self::Accessibility,
        Self::Keyboard,
        Self::Developer,
        Self::Theme,
    ];

    /// Returns the stable wire identifier.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Profile => "profile",
            Self::Appearance => "appearance",
            Self::Panel => "panel",
            Self::Launcher => "launcher",
            Self::Notifications => "notifications",
            Self::Desktop => "desktop",
            Self::Integration => "integration",
            Self::Accessibility => "accessibility",
            Self::Keyboard => "keyboard",
            Self::Developer => "developer",
            Self::Theme => "theme",
        }
    }
}

/// Stable warnings associated with settings operations and snapshots.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum SettingsWarning {
    /// The user configuration was invalid and fallback policy was used.
    InvalidUserConfiguration,
    /// The last-known-valid configuration was invalid.
    InvalidLastKnownValidConfiguration,
    /// Publication succeeded but last-known-valid persistence failed.
    LastKnownValidPersistenceFailed,
}

/// Stable warnings emitted by resolved themes.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ThemeWarning {
    /// High contrast suppressed the configured accent treatment.
    AccentSuppressedHighContrast,
    /// Blur was unavailable and a non-blurred material was selected.
    BlurFallbackActive,
    /// Thumbnail capture was unavailable and a fallback was selected.
    ThumbnailFallbackActive,
    /// Development safe mode overrides are active.
    SafeModeActive,
}

impl ThemeWarning {
    /// Returns the stable wire identifier.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::AccentSuppressedHighContrast => "accent_suppressed_high_contrast",
            Self::BlurFallbackActive => "blur_fallback_active",
            Self::ThumbnailFallbackActive => "thumbnail_fallback_active",
            Self::SafeModeActive => "safe_mode_active",
        }
    }
}

impl SettingsWarning {
    /// Returns the stable wire identifier.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::InvalidUserConfiguration => "invalid_user_configuration",
            Self::InvalidLastKnownValidConfiguration => {
                "invalid_last_known_valid_configuration"
            }
            Self::LastKnownValidPersistenceFailed => "last_known_valid_persistence_failed",
        }
    }
}

/// Candidate behavior required by the publication state machine.
pub trait SettingsPublicationCandidate: Eq {
    /// Reports whether one canonical domain differs from the prior candidate.
    #[must_use]
    fn domain_changed(&self, previous: &Self, domain: SettingsDomain) -> bool;
}

/// One complete immutable settings publication within a service-owner epoch.
#[derive(Debug)]
pub struct SettingsSnapshot<C> {
    snapshot_schema_version: u32,
    generation: Generation,
    candidate: C,
    serialized_json: String,
}

impl<C> SettingsSnapshot<C> {
    /// Returns the runtime snapshot schema version.
    #[must_use]
    pub const fn snapshot_schema_version(&self) -> u32 {
        self.snapshot_schema_version
    }

    /// Returns this publication's nonzero generation.
    #[must_use]
    pub const fn generation(&self) -> Generation {
        self.generation
    }

    /// Returns the complete candidate owned by this snapshot.
    #[must_use]
    pub const fn candidate(&self) -> &C {
        &self.candidate
    }

    /// Returns the bounded complete serialized runtime snapshot.
    #[must_use]
    pub const fn serialized_json(&self) -> &str {
        &self.serialized_json
    }
}

/// Result of one publication request.
#[derive(Debug)]
pub struct PublicationOutcome<C> {
    /// Authoritative snapshot after the request.
    pub snapshot: Arc<SettingsSnapshot<C>>,
    /// Changed domains in canonical order.
    pub changed_domains: Vec<SettingsDomain>,
    /// Whether a new generation was published.
    pub published: bool,
    /// Stable warnings caused by work after the authoritative swap.
    pub operation_warnings: Vec<SettingsWarning>,
}

/// Single-owner atomic publication state.
#[derive(Debug)]
pub struct SettingsPublicationState<C> {
    current: Option<Arc<SettingsSnapshot<C>>>,
    previous: Option<Arc<SettingsSnapshot<C>>>,
}

impl<C> SettingsPublicationState<C> {
    /// Creates unpublished state for one new service-owner epoch.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            current: None,
            previous: None,
        }
    }

    /// Returns the authoritative snapshot, if publication has occurred.
    #[must_use]
    pub const fn current(&self) -> Option<&Arc<SettingsSnapshot<C>>> {
        self.current.as_ref()
    }

    /// Returns the immediately preceding snapshot, if one exists.
    #[must_use]
    pub const fn previous(&self) -> Option<&Arc<SettingsSnapshot<C>>> {
        self.previous.as_ref()
    }
}

impl<C: SettingsPublicationCandidate> SettingsPublicationState<C> {
    /// Serializes and atomically publishes a complete candidate.
    ///
    /// Equal candidates are no-ops. Serialization and size validation finish
    /// before either snapshot pointer changes, so failure cannot consume a
    /// generation or disturb the authoritative publication.
    ///
    /// # Errors
    ///
    /// Returns the serializer's error unchanged, [`ErrorCode::TooLarge`] when
    /// the serialized snapshot exceeds one MiB, or the generation exhaustion
    /// error from [`Generation::checked_next`].
    pub fn publish<F>(
        &mut self,
        candidate: C,
        serialize: F,
    ) -> Result<PublicationOutcome<C>, Error>
    where
        F: FnOnce(Generation, &C) -> Result<String, Error>,
    {
        if let Some(current) = &self.current
            && current.candidate == candidate
        {
            return Ok(PublicationOutcome {
                snapshot: Arc::clone(current),
                changed_domains: Vec::new(),
                published: false,
                operation_warnings: Vec::new(),
            });
        }

        let generation = self.current.as_ref().map_or(
            Ok(Generation::FIRST_PUBLISHED),
            |current| current.generation.checked_next(),
        )?;
        let changed_domains = self.current.as_ref().map_or_else(
            || SettingsDomain::ALL.to_vec(),
            |current| {
                SettingsDomain::ALL
                    .into_iter()
                    .filter(|domain| candidate.domain_changed(&current.candidate, *domain))
                    .collect()
            },
        );
        let serialized_json = serialize(generation, &candidate)?;
        if serialized_json.len() > MAXIMUM_RUNTIME_SNAPSHOT_BYTES {
            return Err(Error {
                code: ErrorCode::TooLarge,
                message: "runtime snapshot exceeds the 1 MiB serialization limit".into(),
                recovery: "reduce the complete snapshot before publication".into(),
            });
        }

        let next = Arc::new(SettingsSnapshot {
            snapshot_schema_version: RUNTIME_SNAPSHOT_SCHEMA_VERSION,
            generation,
            candidate,
            serialized_json,
        });
        self.previous = self.current.replace(Arc::clone(&next));
        Ok(PublicationOutcome {
            snapshot: next,
            changed_domains,
            published: true,
            operation_warnings: Vec::new(),
        })
    }
}

impl<C> Default for SettingsPublicationState<C> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::{
        ConfigurationSourceId, MAXIMUM_RUNTIME_SNAPSHOT_BYTES, ProfileId,
        RUNTIME_SNAPSHOT_SCHEMA_VERSION, SettingsDomain, SettingsPublicationCandidate,
        SettingsPublicationState, SettingsWarning, ThemeWarning,
    };
    use crate::{Error, ErrorCode};
    use std::sync::Arc;

    #[derive(Clone, Debug, Eq, PartialEq)]
    struct Candidate {
        domains: [u8; SettingsDomain::ALL.len()],
    }

    impl Candidate {
        fn new(changes: &[(SettingsDomain, u8)]) -> Self {
            let mut domains = [0; SettingsDomain::ALL.len()];
            for (domain, value) in changes {
                domains[*domain as usize] = *value;
            }
            Self { domains }
        }
    }

    impl SettingsPublicationCandidate for Candidate {
        fn domain_changed(&self, previous: &Self, domain: SettingsDomain) -> bool {
            self.domains[domain as usize] != previous.domains[domain as usize]
        }
    }

    fn serialize(generation: crate::Generation, _candidate: &Candidate) -> Result<String, Error> {
        let generation = generation.value();
        Ok(format!(r#"{{"generation":{generation}}}"#))
    }

    #[test]
    fn stable_profile_domain_and_warning_ids_match_the_native_contract() {
        assert_eq!(ProfileId::Lustre.as_str(), "lustre");
        assert_eq!(ProfileId::Forge.as_str(), "forge");
        assert_eq!("lustre".parse::<ProfileId>(), Ok(ProfileId::Lustre));
        let invalid = "Lustre"
            .parse::<ProfileId>()
            .expect_err("profile identifiers are case-sensitive");
        assert_eq!(invalid.code, ErrorCode::InvalidArgument);

        assert_eq!(
            SettingsDomain::ALL.map(SettingsDomain::as_str),
            [
                "profile",
                "appearance",
                "panel",
                "launcher",
                "notifications",
                "desktop",
                "integration",
                "accessibility",
                "keyboard",
                "developer",
                "theme",
            ]
        );
        assert_eq!(
            [
                ConfigurationSourceId::User,
                ConfigurationSourceId::LastKnownValid,
                ConfigurationSourceId::PackagedDefault,
            ]
            .map(ConfigurationSourceId::as_str),
            ["user", "last_known_valid", "packaged_default"]
        );
        assert_eq!(
            [
                SettingsWarning::InvalidUserConfiguration,
                SettingsWarning::InvalidLastKnownValidConfiguration,
                SettingsWarning::LastKnownValidPersistenceFailed,
            ]
            .map(SettingsWarning::as_str),
            [
                "invalid_user_configuration",
                "invalid_last_known_valid_configuration",
                "last_known_valid_persistence_failed",
            ]
        );
        assert_eq!(
            [
                ThemeWarning::AccentSuppressedHighContrast,
                ThemeWarning::BlurFallbackActive,
                ThemeWarning::ThumbnailFallbackActive,
                ThemeWarning::SafeModeActive,
            ]
            .map(ThemeWarning::as_str),
            [
                "accent_suppressed_high_contrast",
                "blur_fallback_active",
                "thumbnail_fallback_active",
                "safe_mode_active",
            ]
        );
    }

    #[test]
    fn initial_publication_is_complete_and_profile_theme_changes_are_ordered() {
        let mut state = SettingsPublicationState::new();
        let first = state
            .publish(Candidate::new(&[]), serialize)
            .expect("initial publication succeeds");
        assert!(first.published);
        assert_eq!(first.snapshot.generation().value(), 1);
        assert_eq!(
            first.snapshot.snapshot_schema_version(),
            RUNTIME_SNAPSHOT_SCHEMA_VERSION
        );
        assert_eq!(first.changed_domains.as_slice(), &SettingsDomain::ALL);
        assert!(first.operation_warnings.is_empty());
        assert!(state.previous().is_none());

        let captured_first = Arc::clone(&first.snapshot);
        let second = state
            .publish(
                Candidate::new(&[(SettingsDomain::Profile, 1), (SettingsDomain::Theme, 1)]),
                serialize,
            )
            .expect("changed publication succeeds");
        assert_eq!(second.snapshot.generation().value(), 2);
        assert_eq!(
            second.changed_domains.as_slice(),
            &[SettingsDomain::Profile, SettingsDomain::Theme]
        );
        assert_eq!(
            state
                .previous()
                .expect("previous retained")
                .generation()
                .value(),
            1
        );
        assert_eq!(second.snapshot.serialized_json(), r#"{"generation":2}"#);

        let third = state
            .publish(
                Candidate::new(&[
                    (SettingsDomain::Profile, 1),
                    (SettingsDomain::Panel, 1),
                    (SettingsDomain::Theme, 1),
                ]),
                serialize,
            )
            .expect("third publication succeeds");
        assert_eq!(third.snapshot.generation().value(), 3);
        assert_eq!(third.changed_domains.as_slice(), &[SettingsDomain::Panel]);
        assert_eq!(
            state
                .previous()
                .expect("second publication retained")
                .generation()
                .value(),
            2
        );
        assert_eq!(captured_first.generation().value(), 1);
    }

    #[test]
    fn equal_candidate_is_a_no_op_without_serialization() {
        let mut state = SettingsPublicationState::new();
        let candidate = Candidate::new(&[]);
        let first = state
            .publish(candidate.clone(), serialize)
            .expect("initial publication succeeds");
        let same = state
            .publish(candidate, |_, _| panic!("no-op must not serialize"))
            .expect("equal candidate succeeds");

        assert!(!same.published);
        assert!(same.changed_domains.is_empty());
        assert!(Arc::ptr_eq(&first.snapshot, &same.snapshot));
        assert!(state.previous().is_none());
    }

    #[test]
    fn serialization_failure_preserves_state_and_does_not_consume_generation() {
        let mut state = SettingsPublicationState::new();
        let first = state
            .publish(Candidate::new(&[]), serialize)
            .expect("initial publication succeeds");
        let failed = state.publish(Candidate::new(&[(SettingsDomain::Panel, 1)]), |_, _| {
            Err(Error {
                code: ErrorCode::IoError,
                message: "injected serialization failure".into(),
                recovery: "retry with a valid serializer".into(),
            })
        });

        assert_eq!(
            failed.expect_err("serialization must fail").code,
            ErrorCode::IoError
        );
        assert!(Arc::ptr_eq(
            state.current().expect("current retained"),
            &first.snapshot
        ));
        assert!(state.previous().is_none());

        let second = state
            .publish(Candidate::new(&[(SettingsDomain::Panel, 1)]), serialize)
            .expect("next valid publication succeeds");
        assert_eq!(second.snapshot.generation().value(), 2);
    }

    #[test]
    fn oversized_serialization_is_rejected_before_publication() {
        let mut state = SettingsPublicationState::new();
        let error = state
            .publish(Candidate::new(&[]), |_, _| {
                Ok("x".repeat(MAXIMUM_RUNTIME_SNAPSHOT_BYTES + 1))
            })
            .expect_err("oversized snapshot must fail");

        assert_eq!(error.code, ErrorCode::TooLarge);
        assert!(state.current().is_none());
        assert!(state.previous().is_none());
    }
}
