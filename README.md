# TI DS250DFX10 Retimer Driver

This is an out-of-tree linux driver for the TI DS250DF410 and DS250DF810 Retimers for High-speed Network Interfaces.

It can be compiled like any out-of-tree kernel modules, by invoking `make` in the project directory.

## Device tree properties

| Property | Meaning |
|---|---|
| `ti,crosspoint-enable` | crosspoint data path (channel 0 = RX1 -> TX0, channel 1 = RX0 -> TX1) |
| `ti,cal-clock-frequency` | CAL_CLK_IN frequency in Hz (default 25 MHz) |
| `ti,optional` | the retimer is only fitted on some board variants: if it does not answer on I2C at probe, pass-through PHYs (no operations) are registered instead of failing, so consumers listing its channels in `phys` still probe |
