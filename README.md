# Sensa — 3D Rotation Platform

A Wi-Fi-controlled rotating platform for photogrammetry / 3D-scanning workflows.
Place an object on the turntable, jog it from your phone, and capture from
consistent angles — or hit *Start Auto* and let it rotate by 10° every 2
seconds while you take photos.

## Hardware

| Part            | Role                                         |
| --------------- | -------------------------------------------- |
| ESP32-S2-WROOM  | Controller, Wi-Fi access point, web server   |
| MKS-SERVO42C    | Closed-loop NEMA17 driver (STEP/DIR mode)    |
| NEMA17 stepper  | Drives the turntable                         |
| 12–24 V PSU     | Powers the driver (not the ESP32)            |

### Wiring

| ESP32-S2 GPIO | MKS-SERVO42C |
| ------------- | ------------ |
| 40            | STP          |
| 39            | DIR          |
| 41            | ENA          |
| GND           | GND          |

Motor power goes to the driver's screw terminals, not through the ESP32.

### Driver settings (set on the MKS-SERVO42C OLED)

- Working mode: `CR_OPEN` or `CR_vFOC` (closed-loop)
- Microsteps: `16` (matches `MICROSTEPS` in `src/main.cpp`)

## Build and upload

This is a PlatformIO project.

```sh
pio run -t upload          # build + flash
pio device monitor         # serial console (115200 baud)
```

The board exposes USB CDC after boot (`-DARDUINO_USB_CDC_ON_BOOT=1`), so
subsequent uploads don't need the BOOT+RESET dance. If a flash fails because
the chip is wedged, hold BOOT → tap RESET → release BOOT, then retry.

## Use

After flashing, the board hosts a Wi-Fi access point.

1. On your phone: connect to **`RotationPlatform`** (password `rotate1234`).
2. iOS may complain about "no internet" — choose *Use without internet*.
3. Open **http://192.168.4.1** in your browser.

The page gives you:

- **Start Auto / Stop Auto** — toggles a 10°-every-2-seconds loop.
- **±10°**, **±45°**, **±1°** — manual jog.
- **Zero** — reset the cumulative position counter.

## Tuning

In `src/main.cpp`:

| Constant         | What it controls                              |
| ---------------- | --------------------------------------------- |
| `PULSE_MIN_US`   | Cruise step rate (lower = faster)             |
| `PULSE_MAX_US`   | Start/end step rate (slower = safer snap-start) |
| `ACCEL_STEPS`    | Ramp length in steps (higher = smoother)      |
| `AUTO_STEP_DEG`  | Auto-mode move size                           |
| `AUTO_PERIOD_MS` | Auto-mode interval                            |

If the motor stalls or whines at high speed, raise `PULSE_MIN_US`. If it runs
clean and you want more speed, lower it. The closed-loop driver will try to
recover from missed steps, but you'll see jerky motion if you're past the
torque/speed limit.

## License

MIT.
