# Naming contract

The following names are Accepted and exact:

| Meaning | Canonical form |
|---|---|
| Product | **Prismdrake Desktop Environment** |
| Short name | **Prismdrake** |
| Translucent profile | **Prismdrake Lustre** |
| Classic profile | **Prismdrake Forge** |
| Profile identifiers | `lustre`, `forge` |
| Package and executable prefix | `prismdrake-*` |
| D-Bus namespace | `org.prismdrake.*` |
| Glasswyrm-native family | Generic `GW_*` |
| Canonical repository | `JTM-rootstorm/prismdrake-de` |

User-facing prose uses the capitalization above. Machine identifiers are
lowercase ASCII. Component names describe a narrow responsibility and use the
`prismdrake-` prefix; new daemons need an architectural reason beyond cosmetic
modularity.

Public Prismdrake D-Bus names begin with `org.prismdrake.` and carry explicit
interface versions. Native Glasswyrm proposals remain generic, versioned
`GW_*` names because Glasswyrm owns and may reuse those capabilities.

Do not introduce alternate spellings or user-facing abbreviations without an
Accepted ADR. “Aero,” “Luna,” “Windows,” and other Microsoft product names must
not name Prismdrake components, profiles, themes, or features. Comparative
research may mention third-party products accurately, but project vocabulary
must communicate inspiration without implying replication or affiliation.

This contract implements `PD-ID-001` through `PD-ID-010` and is governed by
[ADR 0001](../adr/0001-project-identity.md).
