# LushGate

**English** | [日本語](README.jp.md)

> 🚧 **Status: Work in Progress (Waiting for Component Orders / Prototyping)**  
> This project is currently in the work-in-progress stage while waiting for physical electronic parts to arrive. Firmware architecture, circuit schematics, and the embedded Web UI are already developed and ready for testing upon component arrival.

---

**LushGate** is an autonomous, ultra-low-power automatic irrigation system powered by the **ESP32-C3**, designed specifically for off-grid mountain regions and open-field agriculture.

It determines rainfall levels using a custom-designed corrosion-resistant polarity-reversing rain sensor, and safely actuates a 3V fuel transfer pump via an optocoupler isolation module and a mechanical relay powered by a 12V LiFePO4 solar battery system.

For on-site configuration and maintenance without internet connectivity or cellular signals, LushGate features an on-demand **Wi-Fi SoftAP + mDNS (`http://lushgate.local`) + Responsive Web UI**. You can sync RTC time with a single tap from your smartphone browser, configure schedule/sleep parameters (saved to NVS), inspect irrigation logs, and export CSV reports.

---

## 1. Key Features

- **Ultra-Low Power Operation (Deep Sleep)**:
  - Wi-Fi and Bluetooth are completely powered down during standby. The ESP32 wakes up periodically (default: every 3 minutes) for a few milliseconds to take a pulse measurement and accumulate rainfall data.
- **On-Demand Wi-Fi AP & mDNS (`http://lushgate.local`)**:
  - No external router or internet connection needed. Simply hold the physical `BOOT button` on-site for 2 seconds to launch the SoftAP and connect directly from any smartphone or PC.
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
| **GPIO9** | `USER_BUTTON` | Digital Input (Pull-up) | BOOT button (Hold for 2s to start Wi-Fi AP) |
| **GPIO2-6, 10** | *(Reserved)* | GPIO / ADC1 | Reserved for future expansion (float switch, soil moisture sensor, etc.) |
| **GPIO18/19**| `USB_D- / D+`| Native USB | Firmware flashing and USB serial debugging |

---

## 3. Circuit Schematics

### Overall Block Diagram

```mermaid
graph TD
    Solar[Solar Panel 10W] --> SolarCharger[Solar Charge Controller<br/>w/ Overcharge/Overdischarge Protection]
    SolarCharger --> Battery[LiFePO4 12V 6Ah]
    SolarCharger -->|LOAD Terminals 12V| StepDown33[DC-DC Buck 3.3V]
    SolarCharger -->|LOAD Terminals 12V| StepDownPump[DC-DC Buck 3.0V or 2x D Batteries]
    SolarCharger -->|LOAD Terminals 12V| RelayPower[Relay Power 12V]
    
    StepDown33 --> ESP32[ESP32-C3]
    
    ESP32 -->|GPIO0 (OUT) / GPIO1 (VCC)| RainSensor[J3Y Amplified Rain Sensor]
    ESP32 -->|GPIO7 / GND| OptoModule[Optocoupler Isolation Module]
    ESP32 -->|GPIO8| LED[Status LED]
    ESP32 -->|GPIO9| Button[BOOT Button / AP Launch]
    
    RelayPower --> RelayModule[Mechanical Relay 12V]
    OptoModule -->|Output Contacts / Signal| RelayModule
    StepDownPump -->|Contacts COM/NO| RelayModule --> Pump[Fuel Transfer Pump 3V]
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

## 4. Operation Modes & Web Guide

### 4.1 Operating Modes
- **Normal Autonomous Mode**:
  - Operates in Deep Sleep, waking every cycle (default: 3 minutes) to sample the rain sensor.
  - At the designated morning evaluation time (default: 07:00), if total 24h rainfall is below the threshold (default: 60 minutes), the pump runs with duty cycle control (3 min ON / 2 min OFF, net 10 min).
  - Logs the event outcome into NVS ring buffer and returns to Deep Sleep.
- **AP Maintenance Mode (Web UI)**:
  - Hold the `BOOT button (GPIO9)` for **2 seconds** to turn on the Wi-Fi SoftAP and mDNS service.
  - Connect your smartphone to SSID: `LushGate-XXXX` and open **`http://lushgate.local`** (or `http://192.168.4.1`) in your browser.
  - Returns to Deep Sleep automatically after 5 minutes of inactivity (or upon pressing "Enter Sleep" in the Web UI).

### 4.2 Web UI Features
1. **Status & Time Sync**: Displays RTC current time, daily accumulated rain, live ADC voltage, and next scheduled check. Sync RTC with one tap.
2. **Settings**: Edit watering time, rain check interval, rain skip threshold, ADC threshold, pump ON/OFF durations, and AP timeout.
3. **History**: View the last 60 irrigation runs with details and download logs as CSV.
4. **Manual Diagnostics**: Test pump for 30s with safety auto-stop and take real-time sensor readings.

---

## 5. Web REST API Specification

| Method | URI | Description |
|---|---|---|
| `GET` | `/` | Web UI Single Page Application (HTML5/CSS/JS) |
| `GET` | `/api/status` | Read RTC time, rain accumulation, live sensor data, configs |
| `POST`| `/api/time` | Synchronize time (`{"epoch": 1726904123}`) |
| `GET` | `/api/config` | Fetch current configurations |
| `POST`| `/api/config` | Update configurations in NVS |
| `GET` | `/api/history` | Irrigation logs (JSON) |
| `GET` | `/api/history/csv` | Download irrigation logs as CSV file |
| `POST`| `/api/history/clear` | Clear all irrigation logs |
| `POST`| `/api/pump/test` | Trigger manual pump test (`{"action":"start","duration_sec":30}`) |
| `POST`| `/api/system/sleep` | Stop AP mode and immediately enter Deep Sleep |

---

## 6. Directory Structure

```
LushGate/
├── README.md               # English System Specification & Documentation
├── README.jp.md            # Japanese System Specification & Documentation
├── LushGate_spec.md        # Original Specification Notes
├── CMakeLists.txt          # ESP-IDF Project CMake
├── sdkconfig.defaults      # ESP-IDF Default Configuration
├── docs/
│   └── requirements_definition.html # Requirements definition & system diagram
├── tools/
│   └── mock_web_server.py  # Python local mock server for UI testing
└── main/
    ├── CMakeLists.txt      # Component CMake & Web Assets Embedding
    ├── main.c              # Main control loop & Deep Sleep / scheduling
    ├── lushgate_pins.h     # GPIO pin assignments
    ├── rain_sensor.c/.h    # Polarity-reversing rain sensor driver
    ├── pump_control.c/.h   # Pump duty cycle & manual test driver
    ├── storage_manager.c/.h# NVS config storage & history ring buffer
    ├── wifi_ap.c/.h        # SoftAP & mDNS (lushgate.local) manager
    ├── web_server.c/.h     # Embedded HTTP server & REST API
    └── web/
        └── index.html      # Responsive Web UI SPA (HTML5/CSS/JS)
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
