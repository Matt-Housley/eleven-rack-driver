# Eleven Rack Driver — v1.0.2

A macOS Core Audio driver for the Avid **Eleven Rack** that runs **entirely in
user space** — no kernel extension, no DriverKit system extension, and no
"Reduced Security" / Recovery change. Your Eleven Rack shows up in Audio MIDI
Setup and any DAW as an **8-in / 6-out** device.

> **Requirements:** Apple Silicon Mac, macOS 13 or later.

## What's changed since 1.0.1

- **Playback dropout fix (the headline).** Fixes the clicks, pops and silences
  when the Eleven Rack was used as an everyday output — most obvious when a second
  app started playing (e.g. music going, then a video in a browser). The driver's
  playback path is now a proper sample-time-addressed ring, matching how Core
  Audio actually drives a device, so multiple apps mix together cleanly. Tested
  with three apps playing at once. Single-app playback and DAW use are unaffected.
- **Menu-bar icon fix.** The icon could look bright/"Active" when no Eleven Rack
  was connected (a leftover buffer from a previous session was misread as a live
  device). A disconnected device now correctly shows the dimmed "device not
  connected" icon.

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
