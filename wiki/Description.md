## Description

[Items](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/Items) | [Tutorial](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/Tutorial) | [References](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/References)

This kit supports laboratory study of photovoltaic modules by operating as a controlled electronic load. Once connected to a compatible PV module, it acquires the complete current-voltage characteristic and sends the measurements through MQTT for visualization, storage, comparison, and further analysis.

The overall architecture consists of two main layers:

1.  **Hardware Layer** – a custom PCB, signal-conditioning electronics, an electronic load, and an ESP32-C6 microcontroller responsible for control, acquisition, and wireless transmission.

2.  **Cloud/Software Layer** – a locally hosted environment based on Node-RED and a database, used to reconstruct, visualize, store, compare, and export the acquired tests.

The two layers communicate through an **MQTT broker**. Each board can publish on dedicated topics identified by the board ID, allowing several kits to operate within the same laboratory infrastructure.

The complete hardware is compact and portable, making it suitable for supervised indoor and outdoor laboratory activities with different PV modules.

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/scheme.png" width="605" />
<a name="fig1"></a>

_Fig.1 Architecture_

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/box.png" width="401" />
<a name="fig2"></a>

_Fig.2 Closed box_

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/box_open.png" width="514" />
<a name="fig3"></a>

_Fig.3 Opened box_

### Board schematic

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/schematic.png" width="605" />
<a name="fig4"></a>

_Fig.4 Board schematic (V2)_

The schematic ([Figure 4](#fig4)) shows the V2 hardware architecture of a digitally controlled electronic load designed as a PV curve tracer. The circuit controls the power transistor while simultaneously acquiring panel voltage, load current, and a relative light level.

Control and data acquisition are managed by the ESP32-C6-DEVKITC-1-N8 development board. The primary interfaces are assigned as follows:

- GPIO0 (VDAC): control output for modulating the electronic load.

- GPIO1 (V_panel): ADC input for measuring the panel voltage.

- GPIO2 (I_LOAD): ADC input for measuring the load current.

- GPIO3 (ph_res): ADC input connected to the photoresistor voltage divider.

- GPIO4 (button): digital input for the physical test trigger.

The photovoltaic module voltage enters the board through the V_panel connector (2305908-2).

- A divider formed by a 27 kOhm series resistor and a 2 kOhm resistor to ground scales the module voltage to a level compatible with the ESP32 ADC.

- The divided voltage is applied to the non-inverting input of the first TSV782 amplifier section. With OUT1 connected to IN1-, this stage operates as a voltage buffer, providing high input impedance and a stable V_panel signal for the microcontroller.

Current is measured by the ACS725LLCTR-10AU-T Hall-effect sensor, powered at 5 V.

- The measured current flows through the IP+ and IP- primary terminals, which are in series with the power transistor.

- The sensor produces a proportional analog voltage at VIOUT, labelled I_LOAD in the schematic.

- The FILTER pin uses a 5.1 kOhm resistor and a 100 pF capacitor to limit measurement noise before ADC acquisition.

In the V2 schematic, module power is controlled by the IPB016N06L3GATMA1 N-channel MOSFET. The research prototype described in the accompanying article used a different power device; replacements must therefore follow the actual PCB revision rather than the article component list.

- The MOSFET source is connected to ground, while its drain is connected to the current sensor return terminal IP-.

- The transistor conductance is controlled by its gate voltage, which moves the module through its operating range.

- The VDAC signal passes through a 200 kOhm and 100 nF RC low-pass network. This network smooths the digital transition and produces the gradual gate-voltage variation required for the curve sweep.

- The filtered signal is buffered by the second TSV782 amplifier section. OUT2 drives the MOSFET gate through a 20 Ohm resistor, which limits gate current and reduces switching oscillations.

#### Auxiliary interfaces

- **Light Sensor**: the 3-pin J2 connector accepts a photoresistor that forms a divider with a 5.1 kOhm pull-down resistor. The resulting ph_res signal provides a relative brightness value. It is not a calibrated irradiance measurement in W/m2; the article used a calibrated pyranometer for that purpose.

- **Hardware Trigger**: the push button connects the button input to ground when pressed. The firmware enables the ESP32 internal pull-up resistor so that the idle input is read as logic high.

### Explanation of the code implemented on the board

The board firmware implements a high-speed data-acquisition system for PV module characterization. It measures voltage, current, and relative brightness, then sends the completed dataset to an MQTT broker.

The implementation uses FreeRTOS tasks, queues, event groups, and task notifications, together with DMA-based ADC acquisition and hardware timers.

The first section of the source file defines the hardware interface and the main project constants:

- **Analog Conditioning**: defines the 27 kOhm and 2 kOhm voltage-divider values, the 0.2 V/A current-sensor sensitivity, and the nominal 1.5 V zero-current offset used to convert ADC values into physical quantities.

- **ADC Management**: sets an aggregate sampling rate of 80 ksample/s. Because voltage and current are sampled in sequence, each channel is acquired at approximately 40 ksample/s. DMA buffers transfer the samples without continuously occupying the CPU.

- **Connectivity**: defines the Wi-Fi and MQTT parameters. The payload is divided into approximately 10 kB chunks to limit memory use and allow the receiver to reconstruct the complete test.

The firmware uses five operating states: SYS_IDLE, SYS_ACQUIRE, SYS_PROCESS, SYS_TRANSMIT, and SYS_COOLING_DOWN.

- **Static Buffers**: the main acquisition and transmission arrays are allocated globally rather than on the task stack. This prevents stack overflow when handling a large number of samples.

- **Synchronization**: queues transfer button events from the interrupt routine, event groups track Wi-Fi and MQTT status, and task notifications signal precisely timed acquisition events.

The `status_led_init()` and `status_led_set_rgb()` functions control the built-in RGB LED through the ESP32-C6 RMT peripheral. `set_system_state()` updates both the internal state and the visible indication: green for Idle, blue for acquisition, processing, and transmission, and red for cooling down.

Signal acquisition is divided into two modes:

- **One-Shot ADC**: `read_light_percentage()` acquires 50 photoresistor samples, averages them to reduce noise, and maps the result to a relative value between 0 and 100 percent.

- **Continuous ADC with DMA**: `continuous_adc_init()` configures the voltage and current channels. During a test, `read_and_parse_adc_frame()` retrieves the DMA frames and `store_acquisition_sample()` routes each sample to the corresponding voltage or current buffer.

- **Dynamic Calibration**: the firmware applies the ESP32 eFuse calibration data to reduce ADC non-linearity. Before each test, `calibrate_allegro_offset()` measures the current-sensor output with the electronic load off and uses the result as the actual zero-current offset.

A press of the button on GPIO4 triggers the acquisition through a hardware interrupt.

- The interrupt routine places the pin identifier in a queue and returns immediately, keeping interrupt execution short.

- Two 1 MHz hardware timers define the V2 sequence. The drive state changes after 80 ms and ADC acquisition ends after 160 ms. These values belong to the supplied firmware revision and may differ from the research prototype described in the article.

Network communication is enabled only when data must be transmitted, reducing unnecessary wireless activity.

- **Event Management**: `wifi_event_handler()` and `mqtt_event_handler()` respond to connection, disconnection, IP assignment, and publication events by updating the network event group.

- **MQTT Transmission**: `mqtt_publish_samples()` converts the samples to voltage and current values and formats them as CSV data. `mqtt_append_payload()` publishes each full chunk with QoS 1, requesting broker acknowledgement and reducing the risk of losing a test during transmission.

The application runs two concurrent FreeRTOS tasks:

1.  `button_task` waits for an event from the interrupt queue, applies a 10 ms software debounce, and starts a test only when the system is in the Idle state.

2.  `acquisition_task` executes the complete measurement and transmission sequence.

    - Waits for a valid start notification.

    - Reads the relative light level and calibrates the current-sensor zero offset.

    - Clears residual samples from the continuous ADC buffer.

    - Sets GPIO0 high, starts DMA acquisition, and starts the hardware timers.

    - Continuously drains the DMA data until the 160 ms acquisition timer expires. After 80 ms, GPIO0 is set low so that both open-circuit to short-circuit and short-circuit to open-circuit sweeps are recorded.

    - Stops the acquisition hardware, connects to Wi-Fi, builds the payload, and publishes it through MQTT.

    - Enters a 60-second cooling period, during which further button presses are ignored, before returning to Idle.

At startup, `app_main()` initializes non-volatile storage, timers, analog and digital peripherals, ADC calibration, and the two FreeRTOS tasks. `app_main()` then terminates while the FreeRTOS scheduler continues to run the system.

### Working mode

> ⚠️ **Safety precautions** – Use the kit only under laboratory supervision and only with PV modules whose open-circuit voltage and short-circuit current remain below 50 V and 10 A. Switch the module off before changing connections, verify polarity, and wait for the complete cooling period after every test.

After the test button is pressed, the kit automatically acquires the complete current-voltage characteristic. The controlled load moves the module from open circuit to short circuit and back while voltage and current are sampled. The V2 acquisition lasts 160 ms, with the drive transition after 80 ms. The LED is green when the system is ready, blue during acquisition and wireless transmission, and red during the 60-second cooling period. The photoresistor value is transmitted with the test as a relative brightness indicator from 0 percent in darkness to approximately 100 percent in strong light. It must not be interpreted as calibrated irradiance. Firmware revisions may also trigger the same automatic sequence from a timer, provided that the cooling interval is retained.

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/test_signal.png" width="500" />
<a name="fig5"></a>

_Fig.5 Input pulse, gate voltage, panel voltage and device current during a test_

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/nr_interface.png" width="605" />
<a name="fig6"></a>

_Fig.6 Node-RED interface: curves of a selected test_

### Kit opportunity

The kit is a practical tool for studying PV module behaviour under different operating conditions. Its compact enclosure and light sensor support supervised indoor and outdoor experiments with different modules. The system stores each acquisition, allowing students to compare current-voltage and power-voltage curves, investigate partial shading, observe dynamic hysteresis, and export the data for further analysis.
