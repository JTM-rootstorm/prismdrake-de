# Draft interfaces

Interfaces in this directory are design contracts for review. PD0 does not
implement a D-Bus service, freeze an ABI, or claim wire compatibility.

- [`org.prismdrake.Settings1`](dbus/org.prismdrake.Settings1.xml) proposes a
  narrow versioned settings boundary.

Public Prismdrake interfaces use `org.prismdrake.*`. Methods must be typed,
bounded, cancellable where applicable, and explicit about errors, timeouts,
ownership, and versioning. Generic Glasswyrm-native `GW_*` proposals are owned
and accepted separately by Glasswyrm and are not D-Bus interfaces in this tree.
