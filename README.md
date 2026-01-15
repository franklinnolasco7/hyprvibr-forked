# hyprvibr

Hyprland plugin for adjusting monitor color saturation per-window or globally. Forked to add persistent global saturation.

## Features

- Global saturation applied to all monitors at all times
- Per-app saturation overrides
- Optional resolution switching per-app
- Automatic restoration of original settings

## Installation
```bash
# cd /tmp
git clone https://github.com/franklinnolasco7/hyprvibr-forked
cd hyprvibr-forked
make all

# mkdir -p ~/.config/hypr/plugins
# find . -name "hyprvibr.so" -exec cp {} ~/.config/hypr/plugins/ \;
```

Load the plugin in `hyprland.conf`:
```bash
plugin = /path/to/hyprvibr.so
```

## Configuration

### Basic Example
```bash
plugin = $HOME/hyprvibr-forked/out/hyprvibr.so
hyprvibr-saturation = 1.5  # global setting
hyprvibr-app = firefox,1.5  # app specific
```

### Syntax
```bash
# Global saturation (always active)
hyprvibr-saturation = <value>

# Per-app (overrides global when focused)
hyprvibr-app = <class>,<saturation>
hyprvibr-app = <class>,<saturation>,<width>,<height>
hyprvibr-app = <class>,<saturation>,<width>,<height>,<refresh_rate>
```

### Full Example
```bash
plugin = /path/to/hyprvibr.so

hyprvibr-saturation = 1.3

hyprvibr-app = cs2,2.5,1920,1080,144
hyprvibr-app = firefox,1.5
hyprvibr-app = discord,1.2
```

Use `hyprctl clients` to find window class names.

## Saturation Values

- `1.0` = normal
- `1.2-1.5` = subtle boost
- `1.5-2.0` = noticeable enhancement
- `2.0+` = high saturation

## Changes from Original

The original plugin only applied saturation when specific windows were focused. This fork adds `hyprvibr-saturation` for persistent global saturation, useful for displays with poor color calibration.

## Compatibility

May conflict with other tools that modify the Color Transformation Matrix (CTM):
- hyprsunset
- hyprland_ctm_control_manager_v1 protocol

## Credits

- Original: [devcexx](https://github.com/devcexx/hyprvibr)
- Fork: Added global saturation support
