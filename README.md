# ESP32-C6 Zigbee mmWave Distance Sensor

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Zigbee](https://img.shields.io/badge/Zigbee-ZHA-green)
![Platform](https://img.shields.io/badge/Platform-ESP32--C6-orange)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

A **minimal Zigbee distance sensor** using an **ESP32-C6** and a **Waveshare HMMD mmWave radar module**.

The device reads distance measurements over **UART** and reports them to **Home Assistant (ZHA)** over Zigbee.

Distance is published using the **Temperature Measurement cluster**, allowing the device to work **without custom ZHA quirks or converters**.

---

# Overview

This project provides a simple way to integrate **mmWave distance sensing** into a Zigbee network.

Typical use cases:

• Presence detection  
• Room occupancy logic  
• Lighting automation  
• Smart heating control  
• Distance-based triggers

Example automation logic:

---

# Hardware

### Supported ESP32 Boards

- ESP32-C6 DevKitC-1  
- ESP32-C6 WROOM modules  
- ESP32-C6 development boards using ESP-IDF

### Radar Sensor

---

# Home Assistant Integration

Pair the device using **ZHA**.

Once joined, Home Assistant will detect:

| HMMD Pin | ESP32-C6 |
|---------|----------|
| 3V3 | 3V3 |
| GND | GND |
| TX | GPIO10 |
| RX | GPIO11 |

UART configuration:

Waveshare **HMMD mmWave radar module**

Sensor range:
0m – ~4m

---

# Wiring

| HMMD Pin | ESP32-C6 |
|---------|----------|
| 3V3 | 3V3 |
| GND | GND |
| TX | GPIO10 |
| RX | GPIO11 |

UART configuration:
115200 baud
8 data bits
no parity
1 stop bit
---

# Home Assistant Integration

Pair the device using **ZHA**.


Once joined, Home Assistant will detect:
ChrisLabs C6_HMMD_Distance

A sensor entity will appear:
sensor.c6_hmmd_distance_temperature




2.35 °C


This actually represents:


2.35 meters


---

# Why Temperature?

Zigbee has **no standard cluster for distance measurement**.

To ensure compatibility with Home Assistant without writing custom integrations, the device uses the **Temperature Measurement cluster (0x0402)**.

Zigbee temperature values use **0.01 units**, so we simply map:


distance_cm → temperature value


Example:

| Distance | Zigbee Value | HA Display |
|--------|-------------|-----------|
| 1.5 m | 150 | 1.50 °C |
| 2.0 m | 200 | 2.00 °C |
| 3.8 m | 380 | 3.80 °C |

Home Assistant shows **°C**, but the number represents **meters**.

---

# Project Structure


zigbee_mmwave_distance
│
├── CMakeLists.txt
├── sdkconfig.defaults
│
└── main
├── CMakeLists.txt
├── idf_component.yml
└── main.c


All application logic is contained in:


main/main.c


---

# Zigbee Device Configuration

Device Role:


Router (ZR)


Endpoint:


1


Clusters:

| Cluster | Role |
|-------|------|
Basic | Server |
Identify | Server |
Temperature Measurement | Server |

---

# How It Works

1. ESP32 boots
2. Zigbee stack starts
3. Device joins the Zigbee network
4. UART receives mmWave radar data
5. Distance parsed from strings like:


Range 178


6. Distance converted to Zigbee temperature units
7. Attribute updated and reported periodically

---

# Reporting Frequency

Default update rate:


1 second


---

# Building

Requires:


ESP-IDF v5.x


Set the target:


idf.py set-target esp32c6


Build the project:


idf.py build


Flash the device:


idf.py flash monitor


---

# Pairing

1. Enable **Permit Join** in Home Assistant ZHA
2. Power on the ESP32 device
3. Device will automatically join the network

Manufacturer:


ChrisLabs


Model:


C6_HMMD_Distance


---

# Example Automation

Turn on a light when someone enters the room:

```yaml
trigger:
  - platform: numeric_state
    entity_id: sensor.c6_hmmd_distance_temperature
    below: 2.0

action:
  - service: light.turn_on
    target:
      entity_id: light.room
```


Limitations

Home Assistant displays the value as °C.

The value actually represents meters.

This approach avoids requiring:

custom Zigbee clusters

custom ZHA quirks

Zigbee2MQTT converters

Future Improvements

Planned ideas:

Binary occupancy sensor

Distance smoothing

Configurable range thresholds

OTA firmware support

Adjustable reporting interval

License

MIT License

Author

Chris Wilson
ChrisLabs