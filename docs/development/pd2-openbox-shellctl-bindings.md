# Openbox shell-control binding example

Openbox remains the standards-capable PD2 reference window manager. It owns
global key bindings; Prismdrake does not install a global grab. The following
fixed commands may be copied into the `<keyboard>` element of an Openbox
`rc.xml` test profile:

```xml
<keybind key="W-space">
  <action name="Execute">
    <startupnotify><enabled>no</enabled></startupnotify>
    <command>prismdrake-shellctl launcher toggle</command>
  </action>
</keybind>
<keybind key="W-q">
  <action name="Execute">
    <startupnotify><enabled>no</enabled></startupnotify>
    <command>prismdrake-shellctl quick-settings open</command>
  </action>
</keybind>
<keybind key="W-n">
  <action name="Execute">
    <startupnotify><enabled>no</enabled></startupnotify>
    <command>prismdrake-shellctl notifications open</command>
  </action>
</keybind>
<keybind key="W-p">
  <action name="Execute">
    <startupnotify><enabled>no</enabled></startupnotify>
    <command>prismdrake-shellctl panel focus</command>
  </action>
</keybind>
<keybind key="W-Escape">
  <action name="Execute">
    <startupnotify><enabled>no</enabled></startupnotify>
    <command>prismdrake-shellctl session logout</command>
  </action>
</keybind>
```

Each entry contains one constant executable and constant arguments. Do not
insert an output name, selected-window text, environment value, or another
runtime value into the command. The client has no arbitrary bus destination,
method, executable, or shell mode and applies a two-second monotonic D-Bus call
deadline.

The example grants no privilege. Shell1 and Session1 validate the caller's
effective UID on the user session bus, and Openbox remains authoritative for
whether and when the key event invokes the command.
