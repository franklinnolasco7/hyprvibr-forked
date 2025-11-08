# hyprvibr

No-effort Hyprland plugin for achieving the same "vibrant" color effect to X11
libvibrant and Windows VibranceGUI utility. This tool will change the Color
Transformation Matrix (CTM) dynamically of a monitor where a window that is tracked
and focused by the plugin is displayed, and will clean up the CTM configuration
of the monitor when the window is no longer focused.

## Configuration

```
plugin {
    hyprvibr {
      hyprvibr-app = <app initial class>, <saturation value>
      hyprvibr-app = ...
    }
}
```

For example:

```
plugin {
    hyprvibr {
      hyprvibr-app = cs2, 3.3
    }
}
```

Use `hyprctl clients` to see the current opened windows in Hyprland and check the initial class of each window.

## Compatibility

As I've spent almost no effort on this plugin, I haven't either figure out ways
to make it compatible with other components that mess around with the monitors
CTM, such as the hyprland_ctm_control_manager_v1 protocol. That means that this
plugin will interfer with things like hyprsunset.
