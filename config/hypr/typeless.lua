-- Typeless (typeless-omarchy). Installed to ~/.config/hypr/typeless.lua and
-- loaded with require("hypr.typeless") from hyprland.lua.

-- Floating bar: a transparent 750x500 XWayland canvas that Typeless positions
-- itself (bottom-centre) with the pill drawn inside, so no move rule. Keep it
-- on every workspace, undecorated, and out of focus and default opacity.
o.window({ class = "^Typeless-floating-bar$" }, {
  float = true,
  pin = true,
  decorate = false,
  no_shadow = true,
  no_focus = true,
  tag = "-default-opacity",
})

-- Virtual key from typeless-mic-button (the USB mic's mute button). Typeless
-- reads it straight from /dev/input; keep it away from apps.
hl.device({ name = "typeless-mic-button", enabled = false })
