# Items

[Description](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/Description) | [Tutorial](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/Tutorial) | [References](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/References)

Datasheet references in the **Example** column point to the [Datasheet section](References.md#datasheet).

<table>
<colgroup>
<col style="width: 11%" />
<col style="width: 19%" />
<col style="width: 28%" />
<col style="width: 20%" />
<col style="width: 22%" />
</colgroup>
<thead>
<tr class="header">
<th><strong>Quantity</strong></th>
<th><strong>Name</strong></th>
<th><strong>Specification</strong></th>
<th><strong>Image</strong></th>
<th><strong>Example</strong></th>
</tr>
</thead>
<tbody>
<tr class="odd">
<td>1</td>
<td>Custom GREATER PCB</td>
<td>Supplied as an assembled board or as a bare PCB; see the <a href="#pcb-bill-of-material">PCB Bill of Material</a> below.</td>
<td><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/pcb.jpg" width="189" /></td>
<td></td>
</tr>
<tr class="even">
<td>1</td>
<td>ESP32-C6 development board</td>
<td>ESP32-C6-DevKitC-1-N8 with Wi-Fi, Bluetooth LE, and 8 MB flash.</td>
<td><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/esp32_devkit.jpg" width="150" /></td>
<td><p>Controls the test, processes the ADC samples, and transmits the dataset through MQTT.</p>
<a href="https://github.com/mircomunch/GREATER-PV-Tracer/wiki/References#1">[1]</a></td>
</tr>
<tr class="odd">
<td>1</td>
<td>3D-printed box</td>
<td>Pre-drilled protective enclosure with cable openings and the GREATER logo.</td>
<td><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/box.png" width="189" /></td>
<td>Protects the electronics during supervised laboratory use.</td>
</tr>
<tr class="even">
<td>1</td>
<td>3D-printed button cover</td>
<td>Mechanical cover for the test button.</td>
<td></td>
<td></td>
</tr>
<tr class="odd">
<td>1</td>
<td>Terminal block</td>
<td>External connection for the PV module leads.</td>
<td></td>
<td></td>
</tr>
<tr class="even">
<td>1</td>
<td>Switch</td>
<td>Disconnects the module positive lead from the board.</td>
<td><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image24.jpeg" width="110" /></td>
<td></td>
</tr>
<tr class="odd">
<td>1</td>
<td>Photoresistor</td>
<td>Nominal 10 kOhm light-dependent resistor for relative brightness measurements.</td>
<td></td>
<td></td>
</tr>
</tbody>
</table>

## PCB Bill of Material

| ID | QTY | Item | Name | Package |
|----|-----|------|------|---------|
| 1 | 1 | Microcontroller | ESP32C6 | Devkit C1 |
| 2 | 1 | Pin Header M, 3pin | | |
| 3 | 2 | Pin Header F, 16pin | | |
| 4 | 1 | Current Sensor | ACS725LLCTR-10AU-T | SOIC8 |
| 5 | 1 | OpAmp | TSV782IYST STMicroelectronics | SOIC8 |
| 6 | 1 | Mosfet | IPB016N06L3GATMA1 Infineon | D2PAK |
| 7 | 1 | Resistor | R200k | SMD 0805 |
| 8 | 1 | Resistor | R20 | SMD 0805 |
| 9 | 1 | Resistor | R27k | SMD 0805 |
| 10 | 1 | Resistor | R2k | SMD 0805 |
| 11 | 1 | Resistor | R10k | SMD 0805 |
| 12 | 2 | Resistor | R5.1k | SMD 0805 |
| 13 | 1 | Capacitor | C100p | SMD 0805 |
| 14 | 4 | Capacitor | C100n | SMD 0805 |
| 15 | 1 | Button | | SMD |
| 16 | 1 | Custom PCB board | | |
| 17 | 2 | Insulated wire | | |
