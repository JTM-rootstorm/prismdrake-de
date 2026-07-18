# Failure behavior and fallbacks

Prismdrake fails softly for optional visuals and services, but fails closed for
authorization and unsupported security claims. Diagnostics identify component,
failure class, and recovery without logging private content.

| Scenario | Preserved authority | Recovery and fallback |
|---|---|---|
| Shell crash | WM/compositor retains windows, focus, stacking, workspaces, and composition | Bounded shell restart using the current complete generation; basic WM operations remain usable |
| Settings daemon crash | Consumers retain their last immutable generation | Restart and revalidate from disk; reject partial writes and fall back to packaged defaults only when no user generation is valid |
| Decorator crash | WM retains window state and policy | WM-provided or client-side move, resize, close, and focus path; restart discards stale window IDs |
| Notification daemon crash | Session and shell remain usable; settingsd retains configured do-not-disturb baseline | Reacquire service name with bounded recovery, clear the ephemeral runtime override, reload valid persistent history, and never replay uncertain actions |
| Missing compositor blur | Compositor remains effect authority | Select alpha-tinted or opaque token fallback; never capture and blur the desktop in shell code |
| Missing native Glasswyrm capability | Standards path remains available | Use ICCCM/EWMH/freedesktop behavior; thumbnails become icons and titles; unsupported enhancements are disabled |
| Invalid user configuration | Previous valid generation remains authoritative | Report file and field; preserve input for repair; use packaged defaults only when required for recovery |
| Valid version-1 user configuration | Original source bytes remain authoritative input | Normalize to complete version-2 typed defaults in memory; do not rewrite until an explicit atomic write succeeds |
| Complete settings reload during ephemeral do-not-disturb override | Settingsd remains configured-baseline authority | Notifyd clears the override, applies the new baseline, and publishes one complete notification-center generation |
| Invalid pin state | Shell and application catalog remain usable | Retain the invalid private file for diagnosis, publish an empty runtime pin generation, and execute no stored content |
| Invalid notification history | Live notification service remains usable | Reject the private file, start with empty history, and emit one redacted diagnostic without rejecting new live notifications |
| Notification sender disappears | Notifyd remains action authority | Disable its actions immediately; never execute, activate, or replay a replacement command |
| Stale notification content generation | Current record remains authoritative | Reject dismiss or action invocation with a typed redacted error and publish no new generation |
| Corrupted theme data | Previous valid theme remains authoritative | Reject the candidate before publication and use a validated packaged fallback |
| Profile switch during component restart | Settings daemon owns a single current generation | Restarted component fetches the latest complete generation; obsolete generations are never partially applied |
| Output removal with a shell popup | WM remains output and focus authority | Transfer the one global surface to current primary or dismiss it if no output remains; remove only matching desktop/panel instances |
| Shell crash during logout confirmation | Session process retains the transaction | Restarted shell fetches current transaction state and reconstructs cancellable presentation; only a matching transaction ID may confirm or cancel |
| Reduced motion enabled | User accessibility preference is authoritative | Remove or replace nonessential motion; input and essential actions are never delayed |
| Transparency disabled | User accessibility preference is authoritative | Use declared opaque materials while preserving hierarchy, borders, focus, and state cues |
| External adapter unavailable | External service remains authoritative | Disable only its control, show actionable status, and retry only on meaningful service lifecycle events |
| Authorization agent failure | External authorization authority remains authoritative | Fail closed; discard credentials and require a fresh request |

Safe mode uses packaged defaults, opaque materials, reduced animation, and no
optional adapters. It is recovery behavior, not a substitute for reporting or
fixing a crash loop.

These rules address `PD-COMP-003`, `PD-COMP-004`, `PD-CONFIG-002`,
`PD-CONFIG-003`, `PD-CONFIG-006`, `PD-NOTIFY-002` through `PD-NOTIFY-007`,
`PD-OUTPUT-001` through `PD-OUTPUT-004`, `PD-PLAT-002` through `PD-PLAT-004`,
`PD-SEC-004`, `PD-SEC-007`, and `PD-REL-001` through `PD-REL-010`.
