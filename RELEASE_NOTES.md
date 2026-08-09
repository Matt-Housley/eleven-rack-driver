# Eleven Rack Driver — v1.0.2

A macOS Core Audio driver for the Avid **Eleven Rack** that runs **entirely in
user space** — no kernel extension, no DriverKit system extension, and no
"Reduced Security" / Recovery change. Your Eleven Rack shows up in Audio MIDI
Setup and any DAW as an **8-in / 6-out** device.

> **Requirements:** Apple Silicon Mac, macOS 13 or later.

## What's changed since 1.0.1

- **Menu-bar icon fix.** The icon could look bright/"Active" when no Eleven Rack
  was connected (a leftover buffer from a previous session was misread as a live
  device). A disconnected device now correctly shows the dimmed "device not
  connected" icon.

> **Known issue — dropouts during general-purpose playback:** when the Eleven
> Rack is used as an everyday output (e.g. streaming from Music), you may hear
> periodic dropouts, worse when another app starts playing. This is **not** fixed
> in 1.0.2 — the audio path is unchanged from 1.0.1. A proper fix (a
> hardware-timestamped timeline) is in progress for a later release. For critical
> recording/playback in a DAW the device works as before.

## Install

1. Download **`ElevenRackDriver-1.0.2.pkg`** from this release's assets.
2. Double-click it and follow the installer (it asks for your admin password
   once and restarts the audio service — **quit apps that are playing audio or
   video first**).
3. Plug in your Eleven Rack.

The installer isn't signed with a paid Apple Developer certificate, so macOS asks
you to approve it once: **right-click the `.pkg` → Open → Open**, or open
**System Settings → Privacy & Security** and click **Open Anyway**. No Terminal
needed.

## What's in it

- **8-in / 6-out** at **44.1, 48, 88.2 or 96 kHz**, with the hardware clock
  following the rate you pick in a DAW or Audio MIDI Setup.
  - Inputs: Guitar In, Mic In, Eleven Rig L/R, Digital In L/R, Line In L/R
  - Outputs: Main Out L/R, Re-Amp L/R, Digital Out L/R
- **Menu-bar app** with a status icon (active / device-not-connected / error)
  and a control panel: live per-channel meters, sample-rate picker, MIDI status,
  Open Audio MIDI Setup, Restart Engine, Launch-at-login, and Uninstall.
- **MIDI works out of the box** via macOS's built-in USB-MIDI driver — the device
  appears as the **Eleven Rack Rig** and **Eleven Rack External** ports.
- Auto-starts at login; releases the USB device cleanly on shutdown.

## Notes

- Channel names show in Audio MIDI Setup and in DAWs that read Core Audio channel
  names (Logic, Reaper, Pro Tools). Some apps (GarageBand, Audacity) label inputs
  by number regardless.
- Changing the sample rate causes a brief (~60 ms) gap while the streams restart.

## Uninstall

Click the menu-bar icon → **Uninstall…** (removes the driver, login item, and app
after one password prompt).
