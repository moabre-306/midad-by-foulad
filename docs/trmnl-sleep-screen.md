# TRMNL sleep screen

Midad can show a [TRMNL](https://usetrmnl.com) dashboard while the device sleeps. It is a sleep screen mode, so you
choose it in **Settings → Display → Sleep Screen → TRMNL**. Other sleep modes do not change.

## Set up

1. Save a Wi-Fi network (**Settings → Wi-Fi Networks**).
2. Open **Settings → Apps → TRMNL** (X4 Pro: **Settings → Device & System → TRMNL**). The **Device MAC** row shows this device's ID.
3. For trmnl.com (BYOD licence): add a device with that MAC address in your TRMNL account. For a self-hosted server
   (Terminus, LaraPaper, …): set **Server URL** first.
4. Select **Link Device**. The device calls `GET /api/setup` and keeps the access token and friendly ID.
5. Select **Show Screen Now** to test. Use **Flip Screen** if the dashboard is upside down.
6. Set **Sleep Screen** to **TRMNL**.

## How it works

- At sleep entry, the last downloaded screen is shown, and the device sleeps with a 2 s timer.
- On each timer wake, the device boots headless (no UI, front light off), connects to the saved Wi-Fi network, calls
  `GET /api/display`, downloads the image, and repaints only if the filename changed. Then it sleeps for the
  server's `refresh_rate` (60 s to 24 h).
- After a failure it retries after 1 min, then 5 min, then at the normal refresh rate. The last screen stays visible.
- The power button wakes into Midad as usual.
- Below 5 % battery (and not on USB), the refresh stops until you press the power button.

Images can be BMP, PNG or JPEG. PNG and JPEG are converted to a 2-bit grayscale BMP on the SD card
(`/.crosspoint/trmnl/screen.bmp`). The device reports the `xteink_x4` model (or `xteink_x3` on an X3), so the server
renders for the 800×480 panel. The `ID` and `Access-Token` headers go only to the API host, never to a different
image host.

The device ignores `update_firmware` from the server, so a TRMNL server cannot replace Midad.

Configuration is stored in `/.crosspoint/trmnl.json` (the token is obfuscated like the other stored credentials).

## Battery

Each refresh is a Wi-Fi connection of a few seconds. A long refresh rate (15–30 min or more) keeps the battery drain
low. On the ESP32-C3 X4/X3 the battery latch must stay on for a timer wake, so deep sleep draws a few mA more in this
mode.
