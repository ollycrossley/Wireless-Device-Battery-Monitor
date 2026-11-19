# NuPhy Halo96 V2 over 2.4 GHz

This driver requires the custom Halo96 V2 QMK/VIA ANSI firmware. The original NuPhy receiver forwards the reports. With the stock keyboard firmware, use the app's Bluetooth battery support. [Download the prebuilt firmware from the latest release](https://github.com/ollycrossley/nuphy-custom-firmware/releases/latest)

The Windows driver matches the receiver at `19F5:3247` and the exact USB product name `NuPhy Halo96 V2 Dongle` because other models use the same IDs. It listens to interface 1, Consumer Control usage `000C:0001`, and accepts only the battery sequence on input report ID `03`. It opens the collection read-only and sends no reports.

The keyboard sends its radio module's 0–100 battery percentage about every 15 seconds while awake in 2.4 GHz mode. Each of the following 16-bit usages, shown in hex, occupies one report.

| Order | Usage |
| --- | --- |
| Start marker | `07` |
| High digit | `08 + (percent >> 4)` |
| Low digit | `10 + (percent & 0F)` |
| Release | `00` |

The firmware reserves at least 50 ms between reports. The driver accepts complete frames within 500 ms and ignores values over 100. It expires samples after 35 seconds and drops the reader when the receiver disappears. The tray reads the latest sample at its regular refresh interval. A single `--list` invocation can finish before the first broadcast.

The custom keyboard firmware and original receiver were tested together on a Halo96 V2 QMK/VIA ANSI. This driver reads the battery percentage from v0.1.0 firmware. Displaying charging status requires app support for the optional charging prefix.
