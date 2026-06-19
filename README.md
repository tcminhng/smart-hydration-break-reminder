# Smart Hydration & Break Reminder Device

An IoT device built on an ESP32 microcontroller that tracks water intake and uses smart sensors to remind users to stay hydrated and take regular desk breaks. 

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
