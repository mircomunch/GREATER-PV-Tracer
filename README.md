# ESP32-C6 ADC Acquisition Firmware

This project implements a button-triggered ADC acquisition firmware for ESP32-C6 using ESP-IDF ADC continuous mode.

The firmware reads two ADC1 channels, converts the raw samples into panel voltage and load current, and streams the processed data over the serial monitor in CSV-like format.

## Hardware

| Signal | ESP32-C6 pin | ADC channel | Direction | Notes |
| --- | --- | --- | --- | --- |
| V_panel sense | GPIO1 | ADC1 channel 1 | Input | Voltage divider input |
| I_LOAD sense | GPIO2 | ADC1 channel 2 | Input | Allegro current sensor output |
| Acquisition enable / VDAC | GPIO0 | ADC1 channel 0 pin, used as GPIO | Output | Driven high during the main acquisition window |
| Button | GPIO4 | ADC1 channel 4 pin, used as GPIO | Input | Active-low, internal pull-up enabled |

GPIO0 and GPIO4 are ADC-capable pins on ESP32-C6, but this firmware configures them as digital GPIOs. Only GPIO1 and GPIO2 are sampled by the ADC continuous driver.

## Firmware Flow

The application behaves as a small state machine:

1. `IDLE`
   Waits for a falling-edge interrupt from the active-low button on GPIO4.

2. `ACQUIRE`
   Clears the previous sample buffers, prints `START`, calibrates the current sensor offset, then starts the 100 ms ADC capture window.

3. `PROCESS`
   Converts raw ADC values to millivolts and applies the measurement formulas.

4. `TRANSMIT`
   Prints the processed samples over serial, then returns to `IDLE`.

## Acquisition Details

Current constants are defined in [main/continuous_read_main.c](main/continuous_read_main.c):

| Setting | Value |
| --- | --- |
| ADC unit | `ADC_UNIT_1` |
| ADC channels | `ADC_CHANNEL_1`, `ADC_CHANNEL_2` |
| ADC attenuation | `ADC_ATTEN_DB_12` |
| Continuous ADC sample rate | `80 kHz` total conversions/s |
| Acquisition duration | `100 ms` |
| Expected samples | About `4000` samples per channel |
| Calibration samples | `256` I_LOAD samples |
| Button debounce | `10 ms` |

Important timing note: on ESP32-C6, the configured `80 kHz` is the total continuous ADC conversion rate. Because the ADC pattern contains two channels, each channel receives about `40 kSamples/s`.

## Calibration And Processing

Before the main capture, the firmware samples `ADC_CHANNEL_2` and estimates the Allegro current sensor zero-current offset. If this sample calibration cannot collect any samples, the fallback offset is:

```c
ALLEGRO_OFFSET = 0.65 V
```

The processed values use these formulas:

```text
V_panel = V_adc_ch1 * (R1 + R2) / R2
I_load  = (V_adc_ch2 - calibrated_allegro_offset) / ALLEGRO_SENS
```

With the current constants:

```text
R1 = 27000 ohm
R2 = 2000 ohm
ALLEGRO_SENS = 0.1 V/A
```

ADC calibration uses ESP-IDF curve fitting when available. If eFuse calibration data is not available, the code falls back to a simple raw-to-mV estimate using `3300 mV`.

## Serial Output

When the button is pressed and a capture completes, the serial output has this format:

```text
START
V,I
12.345,0.123
12.347,0.124
...
STOP
```

The data rows are:

```text
V_panel_volts,I_load_amps
```

Diagnostic warnings may also appear through ESP-IDF logging, for example when calibration is incomplete, fewer samples are captured than expected, or the internal result buffers fill.

## Build And Flash

Set the target once:

```sh
idf.py set-target esp32c6
```

Build, flash, and open the monitor:

```sh
idf.py -p PORT flash monitor
```

To exit the serial monitor, press `Ctrl-]`.

If `idf.py` is not available in the shell, export the ESP-IDF environment first. On Windows PowerShell this is typically:

```powershell
. $HOME\esp\v5.5.2\esp-idf\export.ps1
```

If PowerShell blocks the script, start a temporary shell with script execution enabled:

```powershell
powershell -ExecutionPolicy Bypass -NoProfile
```

Then run the ESP-IDF export command again.

## Improvements Over The Original Example

The current firmware is no longer the stock ADC DMA example. The main changes are:

- Added a clear `IDLE -> ACQUIRE -> PROCESS -> TRANSMIT` flow.
- Added button-triggered acquisition with debounce.
- Added a GPTimer-controlled 100 ms capture window.
- Increased ADC continuous rate to `80 kHz`.
- Stores 12-bit raw samples in `uint16_t` buffers instead of truncating them to `uint8_t`.
- Counts samples independently per channel to avoid channel misalignment.
- Adds channel 2 calibration handle and per-acquisition Allegro offset calibration.
- Transmits processed voltage/current pairs in the expected serial format.

## Troubleshooting

- If the code does not build, confirm the target is `esp32c6` and the ESP-IDF environment is exported.
- If no samples are printed, verify the button wiring on GPIO4 and that it pulls the pin low when pressed.
- If `ADC ERROR: no complete V/I sample pairs` appears, check that GPIO1 and GPIO2 are valid input sources and that ADC continuous mode starts successfully.
- If current values are shifted, make sure the load current is zero during the offset calibration period immediately after pressing the button.
- If sample count warnings appear, reduce logging during acquisition or lower `ADC_SAMPLE_FREQ_HZ`.
