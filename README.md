# Smart Hydration & Break Reminder Device

An IoT device built on an ESP32 microcontroller that tracks water intake and uses smart sensors to remind users to stay hydrated and take regular desk breaks. 

## 📸 Hardware Gallery

<p align="center">
  <img src="IMG_4121.PNG" width="350" alt="Smart Hydration Device Final Build"><br>
  <em>Figure 1: Fully Assembled Smart Hydration & Break Reminder Device Prototype</em>
</p>

---

### Internal Electronics & Fabrication
Below is the hardware configuration, custom PCB, and physical component layout inside the chassis:

| Chasis Base View | Custom PCB Tracking (Top) | Custom PCB Tracks (Bottom) |
|---|---|---|
| ![Internal Assembly](IMG_4085.JPG) | ![PCB Top View](IMG_4122.JPG) | ![PCB Bottom Solder](IMG_4123.JPG) |

| Sensor Module Routing | Top Base View | Bottom Base View |
|---|---|---|
| ![Circuit Build](IMG_4073.JPG) | ![Base Enclosure](IMG_4084.JPG) | ![BME280 Environment Sensor](IMG_4086.JPG) |

---

## Abstract

Prolonged sedentary work and inadequate hydration are major contributing factors to chronic fatigue, musculoskeletal strain, and long-term metabolic health issues among students and office workers. To address these challenges, this project introduces the **Smart Hydration & Break Reminder Device**—an intelligent, hardware-integrated IoT solution designed to automate wellness coaching at the desk. By leveraging an ESP32-S3 microcontroller alongside an array of sensors, the device dynamically calculates personalized water intake targets based on user metrics and live ambient conditions (temperature and humidity). Additionally, it tracks sedentary periods using real-time motion vector analysis to adaptively schedule structural breaks. Featuring live local alerts via a TFT LCD shield and a NeoPixel layout, alongside continuous cloud synchronization to a remote Web UI dashboard, this device provides a seamless approach to fostering healthier physical habits in modern workspaces.

## System Functional Features & Specifications

The device operates via four major interdependent software subsystems that run locally on the ESP32-S3 microcontroller, synchronizing asynchronously with the Firebase backend.

### 1. Dynamic Hydration & Environmental Processing
The system updates local climate metrics and recalculates optimal hydration targets dynamically. Instead of relying on static intervals, the formula processes environmental fatigue vectors alongside user physical profiles.

* **Target Volume Base Calculation:**
  $$\text{Target Fluid (mL)} = (\text{Weight [kg]} \times 35) + \Delta H_{\text{temp}}$$
  Where $\Delta H_{\text{temp}}$ adds $250\text{ mL}$ for every $1^\circ\text{C}$ ambient temperature recorded above $30^\circ\text{C}$ to compensate for perspiration loss.
* **Hardware Execution:** The `BME280` sensor queries ambient temperature and humidity over the $I^2C$ bus every 10 seconds. If thresholds are exceeded, the calculated time window between intake alerts shrinks dynamically by up to $20\%$.

### 2. Physical Motion Vectors & Break Logic
To mitigate sedentary risks, an active motion filter classifies whether a user is working or resting. 

* **Calculated Break Time ($CBT$) Formula:**
  $$\text{CBT (minutes)} = (\text{Continuous Work Time [min]} \times 0.1) + \text{Sleep Quality Factor} + (\text{Daily Exercise [min]} \times 0.3)$$
* **Activity Tracking via MPU6050:** The 6-DOF accelerometer captures linear acceleration forces ($A_x, A_y, A_z$). The software processes these vectors using a rolling root-mean-square (RMS) movement threshold:
  $$\text{Movement Vector} = \sqrt{A_x^2 + A_y^2 + A_z^2}$$
  If the movement vector remains below a static deviation threshold for more than 45 continuous minutes, the system triggers a sedentary alarm phase (`ST_ALARM`), signaling the user via the buzzer to stand up and take a physical break.

### 3. Volumetric Liquid Detection & LED Visualizations
Fluid monitoring is handled using a top-mounted waterproof ultrasonic sensor to map the container's interior depth profile.

* **Acoustic Ranging Interferences:** To prevent signal degradation, the system triggers the custom `i2cPause` wrapper. This halts all background $I^2C$ sampling (from the BME280 and MPU6050) for the exact duration of the ultrasonic pulse-echo cycle.
* **NeoPixel Visual Feedback Array:**
  | Remaining Volume (%) | LED Color Representation | Strip Operational State |
  | :--- | :--- | :--- |
  | **75% – 100%** | Solid Vibrant Green | All 8 Addressable LEDs active |
  | **25% – 74%** | Solid Caution Yellow | 4 Addressable LEDs active |
  | **0% – 24%** | Blinking Alert Red | 1 Single LED pulsing at 2 Hz |

### 4. System Core State Machine
The core loop shifts sequentially between four operational states to ensure prompt button responses without missing scheduled sensor logs:

<p align="center">
  <img width="624" height="420" alt="Picture1" src="https://github.com/user-attachments/assets/74afb3fc-2b11-4c38-a1a8-5e7647da4124" />
</p>



* `ST_IDLE`: Standard operational mode tracking active desk hours and movement arrays.
* `ST_ALARM`: Triggers the 3.3V piezo buzzer and flashes the TFT screen when a hydration or break window opens.
* `ST_DRINK_GRACE`: A temporary grace window allowing the user to drink water or manually log progress via the 3x4 matrix keypad.
* `ST_IN_BREAK`: Locks interface tracking for the duration of the Calculated Break Time ($CBT$), prompting the user to rest.

## Features
* **Personalized Hydration Tracking:** Calculates daily water target dynamically using user profile details (age, gender, hours) alongside environmental data.
* **Environmental & Motion Sensing:** Integrates a BME280 temperature/humidity sensor, MPU6050 accelerometer, and a JSN-SR04T waterproof ultrasonic sensor to monitor real-time water levels.
* **Interactive Interface:** Displays countdown timers on an ILI9341 TFT LCD screen and accepts inputs via an attached keypad matrix.
* **Alert Notifications:** Uses a WS2812 RGB LED strip and a buzzer to provide clear visual and audible notifications.
* **Cloud Integration:** Ready for remote logging and monitoring through Firebase real-time databases.

## Hardware Components Used
* ESP32 Development Board
* Adafruit ILI9341 TFT LCD Display
* JSN-SR04T Ultrasonic Sensor
* Keypad Matrix (4x3)
* WS2812 RGB LED Strip
* Buzzer module
