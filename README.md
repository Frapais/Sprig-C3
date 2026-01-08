# Sprig-C3 ESP32 Development Board 
This is the repository of the **Sprig-C3** project. An ESP32 development board made to simplify the creation of small battery-powered devices, especially useful for the Home Assistant platform.
| <img src="PCB/052.jpg" alt="Photo 1" width="600"/> | <img src="PCB/053.jpg" alt="Photo 2" width="600"/> |
|-------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| <img src="PCB/Screenshot%20from%202024-04-12%2010-49-21.png" alt="Pinout" width="600"/> | <img src="PCB/048.jpg" alt="photo 3" width="600"/> |


## Description
### Hardware
#### Main Features
* Li-ion/Li-po battery charging & protection ICs
* Accurate battery capacity measurement IC, accessible via I2C on pins 2 and 3.
* USB Type-C port for charging and uploading firmware
* RESET and BOOT pushbuttons
* LED indicators for "charging" and "USB-power".
* Breadboard-compatible pin headers breaking out all the pins of ESP32-C3, as well as the USB, Battery, and Vcc voltage.

#### I2C Pins of the Sprig-C3 board
| Function | Pin No |
|----------|--------|
| SDA      | 2      |
| SCL      | 3      |

### Tests
| Idle Power Consumption: 66.1μA                                                                                                                          | Max Charging Current: 464mA                                                                                         | VCC Output @ 4.2V input: 3.356V                                                                                       | VCC Output @ 3V input: ~3.359V                                                                                       |
|-------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------|
| <img src="Tests/Idle%20consumption%20at%204.2V.jpg" alt="Idle Power Consumption" width="300"/> | <img src="Tests/Max%20charging%20current.jpg" width="300"/> | <img src="Tests/VCC%20voltage%20at%204.2V.jpg" width="300"/> | <img src="Tests/VCC%20voltage%20at%203V.jpg" width="300"/> |

The Idle Power Consumption image shows the consumption of all the components of the board except the ESP32 module, as its consumption fluctuates depending on the tasks it's running. Therefore, if you want to calculate the battery life of your project, you should expect at least **66μA** of current, in addition to the current consumption of the ESP32 module.

### Home Assistant setup using ESPHome
#### Requirements:
* Access to Home Assistant Dashboard
* Installed ESPHome addon

#### Main process:
1. Connect the Sprig-C3 board on the computer running Home Assistant, while pressing the BOOT button (required only the first time).
2. Open ESPHome, and add a new ESP32-C3 device as shown [here](https://esphome.io/guides/getting_started_hassio#dashboard-interface).
3. Copy the contents of the provided [configuration file](Home%20Assistant%20Setup/Sprig-c3-config.yaml) for the Sprig-C3, to the generated `YAML` of your ESPHome device.
4. Upload the firmware on the board while selecting the correct port from the popup window. (It usually shows up as a "**USB JTAG**" device).
5. Last step, is to go to your Home Assistant settings, and Configure your newly discovered device (assuming it is powered on and visible on the network).

#### Video Tutorial: [Sprig-C3 first setup](https://youtu.be/UaIIV4CaRA4)

#### Battery Capacity Measurement
The Sprig-C3 board features the MAX17048 battery capacity measurement IC connected to the respective I2C pins. As there is not complete support for this IC in the [ESPHome](https://esphome.io/index.html), you need to add it as a custom [ESPHome Component](https://esphome.io/components/sensor/custom), or just add a [MAX17043](https://esphome.io/components/sensor/max17043/) component which is compatible.  

#### Enabling fuel-gauge in ESPHome
Inside the [Home Assistant Setup](Home%20Assistant%20Setup) folder, you will find a `YAML` file.
* First, you have to create a new ESP32-C3 device from the ESPHome plugin, as described above. (If you haven't already).
* Inside the created YAML file, you must paste the contents of this repo's [YAML](Home%20Assistant%20Setup/Sprig-c3-config.yaml) file.

**Voila!** You can now monitor your battery status from Home Assistant with accuracy and precision!

### Home Assistant Setup using MQTT
#### Requirements:
* Access to Home Assistant Dashboard
* Installed [MQTT](https://www.home-assistant.io/integrations/mqtt/) integration (Tested with Mosquitto Broker).
* Installed [Arduino IDE](https://www.arduino.cc/en/software/) on your PC.

#### Main process:
1. Download the provided [Arduino scetch](Home%20Assistant%20Setup/MQTT%Setup/Sprig_mqtt_basic.ino) and the corresponding [secrets.h](Home%20Assistant%20Setup/MQTT%Setup/secrets.h) header file.
2. Open the `secrets.h` and replace the contents with your WiFi and your MQTT server credentials.
3. Connect your Sprig-C3 to your PC via USB and select the detected COM port from the Arduino IDE.
4. Upload the code to the Sprig-C3.
5. Go the MQTT integration on your Home Assistant and manually add a new device. By default the code publishes the battery level and battery voltage to the `sprigC3/batVoltage` and `sprigC3/batLevel` topics respectively, so you need to add these topics while creating the MQTT device to immediately see the measured battery status.
#### Of course the provided Arduino code is just a sample, publishing the measured battery status. You can use it as a base for building your custom MQTT device with the Sprig-C3.

  
## Availability
You can get the assembled boards in my [Tindie](https://www.tindie.com/products/34523/), [Elecrow](https://www.elecrow.com/nanocell-c3.html), or my [official website](https://sprig-labs.com/) stores.

<a href="https://www.tindie.com/stores/spriglabs/?ref=offsite_badges&utm_source=sellers_Frapais&utm_medium=badges&utm_campaign=badge_large"><img src="https://d2ss6ovg47m0r5.cloudfront.net/badges/tindie-larges.png" alt="I sell on Tindie" width="200" height="104"></a>

## Certifications
This project is certified by the [Open Source Hardware Association (OSHWA)](https://certification.oshwa.org/gr000008.html)

<img src="PCB/Certifications/certification-mark-GR000008-stacked.png" alt="Open Source Hardware Association Certification" width="300" allign="left"/>





## Support
If you find this project useful, please consider supporting me on any of the following platforms:
* PayPal:
  * <a href="https://www.paypal.com/paypalme/kostasparaskevas">
    <img src="https://img.shields.io/badge/$-donate-ff69b4.svg?maxAge=2592000&style=flat">
* Buy Me a Coffe:
  * <a href="https://www.buymeacoffee.com/spriglabs" target="_blank"><img src="https://www.buymeacoffee.com/assets/img/custom_images/purple_img.png" alt="Buy Me A Coffee" style="height: 41px !important;width: 174px !important;box-shadow: 0px     3px 2px 0px rgba(190, 190, 190, 0.5) !important;-webkit-box-shadow: 0px 3px 2px 0px rgba(190, 190, 190, 0.5) !important;" ></a>
* Instagram:
  * [@frapais.lab](https://www.instagram.com/sprig_labs/)



