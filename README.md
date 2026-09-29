# LushGate

**English** | [日本語](README.jp.md)

> ⚡ **Status: Implementation Complete — Field Testing in Progress**  
> Firmware, circuit, and BLE Web UI are fully implemented and currently undergoing field testing.

---

**LushGate** is an autonomous, ultra-low-power automatic irrigation system powered by the **ESP32-C3**, designed specifically for off-grid mountain regions and open-field agriculture.

It determines rainfall levels using a custom-designed corrosion-resistant polarity-reversing rain sensor, and safely actuates a 3V fuel transfer pump via an optocoupler isolation module and a mechanical relay powered independently by 4x AA batteries (6V via USB) for the ESP32 and 2x AA batteries (3V) for the pump.

For on-site configuration and maintenance without internet connectivity or cellular signals, LushGate features an on-demand **BLE (Bluetooth Low Energy) + Web Bluetooth UI** hosted as a PWA at `https://amekusa03.github.io/LushGate/`. Connect directly from your smartphone browser — no Wi-Fi router or SoftAP required. Sync RTC time with a single tap, configure schedule/sleep parameters (saved to NVS), inspect irrigation logs, and send pump commands.

---

## 1. Key Features

- **Ultra-Low Power Operation (Deep Sleep)**:
  - Wi-Fi and Bluetooth are completely powered down during standby. The ESP32 wakes up periodically (default: every 3 minutes) for a few milliseconds to take a pulse measurement and accumulate rainfall data.
- **On-Demand BLE Mode (Web Bluetooth)**:
  - No external router or internet connection needed. Press the `BOOT button` to start BLE advertising, then open `https://amekusa03.github.io/LushGate/` on your smartphone and tap "Connect to LushGate".
- **One-Tap RTC Time Sync**:
  - Synchronizes the ESP32 internal Real-Time Clock with your smartphone's browser clock in one tap.
- **Web-Based Configuration (NVS Persistence)**:
  - Adjust watering schedule time, rainfall skip threshold, sleep interval, pump duty cycle (ON / OFF / total duration), and AP auto-timeout directly from the web interface. All settings persist in Non-Volatile Storage.
- **Irrigation History & CSV Export**:
  - Stores up to 60 historical irrigation events (timestamp, 24h accumulated rain duration, run/skip status, pump run seconds). Viewable in a table or downloadable as CSV.
- **Manual Testing & Real-Time Diagnostics**:
  - Includes a 30-second manual pump test (with auto-cutoff safety) and live rain sensor voltage/ADC measurements for easy field debugging.

---

## 2. Hardware Specifications & I/O Pin Assignment

### Microcontroller: ESP32-C3 (3.3V Logic)

| GPIO | Signal Name | I/O / Type | Description & Connection |
|---|---|---|---|
| **GPIO0** | `RAIN_SENSE_A` | ADC1_CH0 / InOut | Rain Sensor Electrode A (Pulse drive / ADC read) |
| **GPIO1** | `RAIN_SENSE_B` | ADC1_CH1 / InOut | Rain Sensor Electrode B (Pulse drive / ADC read) |
| **GPIO7** | `PUMP_CTRL` | Digital Output | Pump control output (Active-High: Optocoupler module input) |
| **GPIO8** | `STATUS_LED` | Digital Output | Status indicator LED (Blinks during AP mode) |
| **GPIO9** | `USER_BUTTON` | Digital Input (Pull-up) | BOOT button (Press to start BLE advertising mode) |
| **GPIO2-6, 10** | *(Reserved)* | GPIO / ADC1 | Reserved for future expansion (float switch, soil moisture sensor, etc.) |
| **GPIO18/19**| `USB_D- / D+`| Native USB | Firmware flashing and USB serial debugging |

---

## 3. Circuit Schematics

### Overall Block Diagram

```mermaid
graph TD
    BatteryESP[4x AA Batteries (DC 6V)] -->|USB Power| ESP32[ESP32-C3]
    
    ESP32 -->|GPIO0 (OUT) / GPIO1 (VCC)| RainSensor[J3Y Amplified Rain Sensor]
    ESP32 -->|GPIO7 (IN) / 3.3V / GND| RelayModule[JQC-3F 3V Relay Module]
    ESP32 -->|GPIO8| LED[Status LED]
    ESP32 -->|GPIO9| Button[BOOT Button / BLE Start]
    
    BatteryPump[2x AA Batteries (DC 3V)] -->|Relay COM/NO| RelayModule --> Pump[3V Fuel Transfer Pump]
```

---

### (1) Rain Sensor Circuit (J3Y NPN Current Amplification & Pulsed Power)

```
       [ + Pin / VCC ] (ESP32 GPIO1: 3.3V Pulsed Power)
           │
           ├─────────────────────────+
           │                         │ (Collector)
           ├──────────────+          │
           │              │          │
        [ 1kΩ ]        [ 100Ω ]      │
           │              │          │
        [ LED1 ]      [ Sense Trace(+) ]
        (Power LED)       : (Raindrop)
           │          [ Sense Trace(-) ]
           │              │          │
           │              │ (Base)   │
           │              +──────[ J3Y (NPN) ]
           │                         │ (Emitter)
           │                         ├──────────────> [ S Pin / OUT ] ──> ESP32 GPIO0 (ADC1_CH0)
           │                         │
           │                      [ 100Ω ]
           │                         │
           ├─────────────────────────+
           │
       [ - Pin / GND ] (ESP32 GND)
```
- **Signal Amplification**: NPN transistor (J3Y / S8050) amplifies minute conduction currents from raindrops on the sensor to produce a solid, detectable voltage across the 100Ω emitter resistor.
- **Ultra-Low Power & Anti-Corrosion**: VCC power is supplied via ESP32-C3 **GPIO1** only during measurement (5ms pulse). During idle/Deep Sleep, power is cut and pins are held in High-Z, eliminating quiescent current and electrochemical corrosion.

---

### (2) Pump Driver Circuit (Optocoupler Isolation Module + Relay)

```
[ ESP32 Control Side (3.3V) ]    [ Optocoupler Isolation Module ]     [ Relay Driver Side (12V) ]
                                      +------------------+
ESP32 3.3V (Power) ----------------> | VCC (Input 3.3V) |
GPIO7 (PUMP_CTRL) -----------------> | IN1+ (or IN1)    |
ESP32 GND (Signal GND) ------------> | IN1- (or GND)    |         +12V (Controller LOAD+)
                                      |                  |          |
                                      |     JD-VCC/OutVCC| <--------+
                                      |                  |          +-------------+
                                      |             OUT1 | -------------------> | / | (1N4007)
                                      |                  |        [ Relay Coil ] |/  | (Flyback Diode)
                                      |       Output GND | <---+  [ 12V Relay  ] +---+
                                      +------------------+     |                  |
                                                               +------------------+
                                                               |
                                                           12V GND (Controller LOAD-)
```
*Note: The optocoupler input (primary) side is powered and driven entirely by ESP32 3.3V and GPIO7, maintaining full galvanic isolation from the 12V/3V relay coil and motor circuitry.*

---

## 4. Operation Modes & BLE Guide

### 4.1 Operating Modes
- **Normal Autonomous Mode**:
  - Operates in Light Sleep, waking every cycle (default: 3 minutes) to sample the rain sensor.
  - At the designated morning evaluation time (default: 07:00), if total 24h rainfall is below the threshold (default: 60 minutes), the pump runs with duty cycle control (3 min ON / 2 min OFF, net 10 min).
  - Logs the event outcome into NVS ring buffer and returns to Light Sleep.
- **BLE Mode (Web Bluetooth UI)**:
  - Press the `BOOT button (GPIO9)` — the LED starts blinking and BLE advertising begins.
  - Open `https://amekusa03.github.io/LushGate/` in your smartphone browser and tap **"Connect to LushGate"**. BLE connection is established with no Wi-Fi or router needed.
  - Returns to Light Sleep automatically after client disconnection or BLE session ends.

### 4.2 Web Bluetooth UI Features
1. **Status & Time Sync**: Displays RTC current time, daily accumulated rain, live sensor voltage, and pump status in real time. Sync RTC with one tap.
2. **Settings (NVS Persistent)**: Edit watering time, rain check interval, rain skip threshold, ADC threshold, and pump ON/OFF durations. All changes saved to NVS.
3. **History**: Retrieve and display the last 60 irrigation log entries by index.
4. **Manual Pump Command**: Send pump on/off commands for any duration via the `PUMP_CMD` GATT characteristic.

---

## 5. BLE GATT Interface Specification

**Service UUID**: `12340000-5678-1234-5678-000000000000`

| Characteristic | UUID (last 4) | Properties | Description |
|---|---|---|---|
| `CONFIG`   | `0001` | Read / Write | `lushgate_config_t` binary — read/write device settings (NVS-persisted) |
| `TIMESYNC` | `0002` | Write        | Write UNIX Epoch as `uint32LE` to update the ESP32 RTC |
| `PUMP_CMD` | `0003` | Write        | `0x00`=OFF, `0x01`=ON, `[0x02, sec_lo, sec_hi]`=ON for N seconds |
| `STATUS`   | `0004` | Read / Notify| JSON string with rain accumulation, pump state, RTC time, sensor voltage |
| `HISTORY`  | `0005` | Read / Write | Write: index as `uint16LE` / Read: that log entry as JSON |

---

## 6. Directory Structure

```
LushGate/
├── README.md               # English System Specification & Documentation
├── README.jp.md            # Japanese System Specification & Documentation
├── LushGate_spec.md        # Original Specification Notes
├── CMakeLists.txt          # ESP-IDF Project CMake
├── sdkconfig.defaults      # ESP-IDF Default Configuration (NimBLE / Light Sleep, etc.)
├── partitions.csv          # Custom Partition Table
├── docs/                   # GitHub Pages deployment directory
│   ├── index.html          # Web Bluetooth PWA application
│   ├── manifest.json       # PWA manifest
│   ├── sw.js               # Service Worker (offline cache)
│   ├── icon-192.png        # PWA app icon (192px)
│   ├── icon-512.png        # PWA app icon (512px)
│   ├── qrcode.png          # App URL QR code
│   ├── qrcode_print.png    # Print-optimized QR code
│   ├── qrcode.svg          # SVG QR code
│   ├── requirements_definition.html # Requirements definition & system diagram
│   └── qr/
│       ├── print_label_ble.html     # BLE label print page for control box
│       └── qr_ble_app.png           # BLE app URL QR code
├── tools/
│   └── lushgate_ble_app.html        # BLE app standalone version (dev / distribution)
└── main/
    ├── CMakeLists.txt      # Component CMake
    ├── idf_component.yml   # NimBLE component dependency definition
    ├── main.c              # Main control loop & Light Sleep / scheduling
    ├── lushgate_pins.h     # GPIO pin assignments
    ├── ble_gatt.c/.h       # BLE GATT service (NimBLE) & characteristic definitions
    ├── rain_sensor.c/.h    # Polarity-reversing rain sensor driver
    ├── pump_control.c/.h   # Pump duty cycle & manual command driver
    └── storage_manager.c/.h# NVS config storage & history ring buffer
```

---

## 7. Build & Flash (ESP-IDF)

```bash
# Set target chip to ESP32-C3
idf.py set-target esp32c3

# Build firmware and web assets
idf.py build

# Flash to device and monitor serial output
idf.py -p /dev/ttyACM0 flash monitor
```

---

## License

This project is licensed under the MIT License.
