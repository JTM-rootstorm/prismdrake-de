# PD1 Qt Quick and X11 evidence spike

This directory is a removable, non-installed experiment that supplied evidence
for PD1-WP1 and Accepted ADR 0003. It is not production Prismdrake architecture.
The standalone CMake project does not participate in the repository root build
and contains no `install()` rules.

The experiment reads the committed Prismdrake Lustre and Prismdrake Forge token
files through a small C++ evidence loader. Presentation and accessibility state
remain in the C++ model. QML contains layout, visual bindings, focus navigation,
and interruptible animation only. The X11 adapter is isolated and applies the
standard `_NET_WM_WINDOW_TYPE_DOCK`, `_NET_WM_STRUT`, and
`_NET_WM_STRUT_PARTIAL` properties using checked requests before the window is
mapped. It does not implement window-manager policy, Glasswyrm-native protocols,
compositor blur, scene capture, or screenshot blur.

`tests/inspect_atspi.py` queries the live AT-SPI tree through the system Python
GI bindings. It verifies the expected control names, can assert the focused
control after externally injected X11 input, and prints the observed roles,
states, actions, interfaces, and descriptions as JSON. Run it only inside the
same isolated D-Bus session as the spike; source metadata alone is not treated
as accessibility evidence.

See [`docs/research/pd1-toolkit-spike.md`](../../docs/research/pd1-toolkit-spike.md)
for reproducible commands and observed-versus-pending evidence.
