# Tutorial

[Description](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/Description) | [Items](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/Items) | [References](https://github.com/mircomunch/GREATER-PV-Tracer/wiki/References)

The construction and configuration of the kit involve three main stages:

1.  **Mount the hardware**

2.  **Upload the firmware**

3.  **Setup the Node-RED interface**

# Step 1: Mount the Hardware

Open the enclosure by removing the four screws. Insert the ESP32-C6 development board into the two female headers, checking that every pin is aligned and that the USB-C connector faces the opening in the enclosure.

Keep the external switch off while wiring the module. Connect the blue lead between the negative terminal of the board and the negative terminal of the PV module. Connect the module positive terminal to the switch with the red lead, then connect the switch to the board positive terminal. Connect the photoresistor to J2 and mount it where it receives light representative of the module surface. Verify polarity and terminal security before closing the box. See [Figure 1](#fig1).

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/box_open.png" width="612" />
<a name="fig1"></a>

_Fig.1 Mounted hardware_

# Step 2: Upload the Firmware

The ESP32-C6 firmware binary is available in the project GitHub repository.

It can be installed via the online web tool offered by the producer: <https://espressif.github.io/esptool-js/>

1.  Connect the ESP32-C6 to the computer with a data-capable USB-C cable.

2.  In the Program section, set the baud rate to 115200 ([Figure 2](#fig2)).

    <img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/esp_tool.png" width="400" />
    <a name="fig2"></a>

    _Fig.2 ESP Tool, baud rate selection_

3.  Select Connect, choose the USB serial port associated with the ESP32-C6, and confirm the connection ([Figure 3](#fig3)).

    <img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/esp_tool_connect.png" width="241" />
    <a name="fig3"></a>

    _Fig.3 Serial port selection_

4.  Set Flash Address to 0x0000 and select the supplied .bin file. Leave the remaining flash parameters at their default values unless the firmware release specifies otherwise ([Figure 4](#fig4)).

    <img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/esp_tool_flash.png" width="400" />
    <a name="fig4"></a>

    _Fig.4 Flash address and firmware file_

5.  Select Program and wait until writing and verification are complete. Do not disconnect the USB cable during the process. Reset the board and confirm that the status LED reaches green.

# Step 3: Setup Node-RED interface

1.  Download and install Node-RED on the laboratory computer.

2.  Before starting the application, enable the Projects feature in `settings.js`. Open `C:\Users\<your_user_name>\.node-red\settings.js`, apply the configuration shown in [Figure 5](#fig5), save the file, and restart Node-RED.

<img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image35.png" width="165" />
<a name="fig5"></a>

_Fig.5 Enable projects setting_

3.  When Node-RED restarts with Projects enabled, follow the startup procedure shown in the following figures.

<table>
<colgroup>
<col style="width: 65%" />
<col style="width: 35%" />
</colgroup>

<tbody>
<tr class="odd">
<td><p><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image36.png" width="283" /></p>
<p><i>Project startup</i></p></td>
<td>Select “Clone Repository”</td>
</tr>
<tr class="even">
<td><p><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image37.png" width="283" /></p>
<p><i>Project username</i></p></td>
<td>Enter a username and email address. This is only required for Git
operations; <strong>no GitHub account is needed</strong>.</td>
</tr>
<tr class="odd">
<td><p><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image38.png" width="283" /></p>
<p><i>Project GitHub source code link</i></p></td>
<td><p>Insert in the “Git repository URL” the Git project url:</p>
<p>https://github.com/mircomunch/GREATER-PV-Tracer.git</p></td>
</tr>
<tr class="even">
<td><p><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image39.png" width="283" /></p>
<p><i>Package update folder</i></p></td>
<td>In Menù → Projects → Projects Settings → Settings update the
package.json path by selecting the one inside the “Node-RED application”
folder</td>
</tr>
<tr class="odd">
<td><p><img src="https://github.com/mircomunch/GREATER-PV-Tracer/blob/main/wiki/images/image40.png" width="283" /></p>
<p><i>Project update dependencies</i></p></td>
<td>In Menù → Projects → Projects Settings → Dependencies install the
required plugins</td>
</tr>
</tbody>
</table>

If the application cannot deploy, check the Configuration Nodes in the right sidebar. Remove only unused numeric configuration nodes, because obsolete references can prevent deployment.

4.  Complete the configuration by opening the project environment settings from the top-right menu. Enter values that match the laboratory database, MQTT broker, credentials, and topic hierarchy. Do not store passwords in the public repository. Configure the following variables:

    - DB_PATH

    - MQTT_HOSTNAME

    - MQTT_PORT

    - MQTT_USR

    - MQTT_PSW

    - MQTT_TOPIC_DATA

    - MQTT_TOPIC_DATETIME

    - MQTT_TOPIC_CMD
