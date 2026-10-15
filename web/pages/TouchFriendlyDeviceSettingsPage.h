// Firmware_Teensy_AiO-New-Dawn is copyright 2025 by the AOG Group
// Firmware_Teensy_AiO-New-Dawn is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
// Firmware_Teensy_AiO-New-Dawn is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// You should have received a copy of the GNU General Public License along with Firmware_Teensy_AiO-New-Dawn. If not, see <https://www.gnu.org/licenses/>.
// Like most Arduino code, portions of this are based on other open source Arduino code with a compatiable license.

// AOW HAT: copied from Firmware_Teensy_AiO_26 v26.5.5 web_pages/TouchFriendlyDeviceSettingsPage.h.
// HAT changes: no VWAS, encoder or JD PWM controls (no such hardware); adds Invert Work Switch and LED
// Brightness (firmware settings with no page); passthrough help mentions AgOpenWeb. The RTK Radio
// Baud Rate moved to the Serial Communications page (/serial), as its RS-232 port's rate.

// TouchFriendlyDeviceSettingsPage.h
// Touch-optimized device settings page maintaining all existing functionality

#ifndef TOUCH_FRIENDLY_DEVICE_SETTINGS_PAGE_H
#define TOUCH_FRIENDLY_DEVICE_SETTINGS_PAGE_H

// #include <Arduino.h>  (AOW HAT: not needed; PROGMEM is defined by web/api.cpp)

const char TOUCH_FRIENDLY_DEVICE_SETTINGS_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <title>Device Settings - AiO v26</title>
    <link rel="stylesheet" href="/touch.css">
    <style>
        /* Additional styles specific to device settings */
        .toggle-container {
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 12px 0;
            border-bottom: 1px solid #ecf0f1;
        }

        .toggle-container:last-child {
            border-bottom: none;
        }

        .toggle-label {
            font-size: 18px;
            font-weight: 500;
            flex: 1;
            margin-right: 15px;
        }

        .toggle-info {
            flex: 1;
        }
        
        .help-text {
            font-size: 14px;
            color: #7f8c8d;
            margin-top: 5px;
            line-height: 1.3;
        }
        
        select {
            width: 100%;
            padding: 15px;
            font-size: 18px;
            border: 2px solid #bdc3c7;
            border-radius: 8px;
            background: white;
            -webkit-appearance: none;
            appearance: none;
            background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath fill='%23333' d='M10.293 3.293L6 7.586 1.707 3.293A1 1 0 00.293 4.707l5 5a1 1 0 001.414 0l5-5a1 1 0 10-1.414-1.414z'/%3E%3C/svg%3E");
            background-repeat: no-repeat;
            background-position: right 15px center;
            padding-right: 45px;
        }
        
        .slider-container {
            margin: 10px 0;
        }
        
        .slider-value {
            font-size: 20px;
            font-weight: 600;
            color: #3498db;
            text-align: center;
            margin-bottom: 5px;
        }
        
        input[type="range"] {
            -webkit-appearance: none;
            appearance: none;
            width: 100%;
            height: 44px;
            background: transparent;
            margin: 10px 0;
            outline: none;
            cursor: pointer;
        }

        input[type="range"]::-webkit-slider-track {
            width: 100%;
            height: 12px;
            background: #e0e0e0;
            border-radius: 6px;
            border: 1px solid #bdc3c7;
            box-shadow: inset 0 1px 2px rgba(0,0,0,0.1);
        }

        input[type="range"]::-webkit-slider-runnable-track {
            width: 100%;
            height: 12px;
            background: #e0e0e0;
            border-radius: 6px;
            border: 1px solid #bdc3c7;
            box-shadow: inset 0 1px 2px rgba(0,0,0,0.1);
            cursor: pointer;
        }
        
        input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            appearance: none;
            width: 44px;
            height: 44px;
            background: #3498db;
            border-radius: 50%;
            cursor: pointer;
            margin-top: -16px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.2);
            border: 2px solid #fff;
            position: relative;
        }

        input[type="range"]:focus {
            outline: none;
        }

        input[type="range"]:focus::-webkit-slider-thumb {
            box-shadow: 0 0 0 3px rgba(52, 152, 219, 0.3), 0 2px 5px rgba(0,0,0,0.2);
        }

        /* Firefox */
        input[type="range"]::-moz-range-track {
            width: 100%;
            height: 12px;
            background: #e0e0e0;
            border-radius: 6px;
            border: 1px solid #bdc3c7;
            box-shadow: inset 0 1px 2px rgba(0,0,0,0.1);
        }

        input[type="range"]::-moz-range-thumb {
            width: 44px;
            height: 44px;
            background: #3498db;
            border-radius: 50%;
            cursor: pointer;
            border: none;
            box-shadow: 0 2px 5px rgba(0,0,0,0.2);
        }

        /* IE/Edge */
        input[type="range"]::-ms-track {
            width: 100%;
            height: 12px;
            background: transparent;
            border-color: transparent;
            border-width: 16px 0;
            color: transparent;
        }

        input[type="range"]::-ms-fill-lower {
            background: #bdc3c7;
            border-radius: 6px;
        }

        input[type="range"]::-ms-fill-upper {
            background: #bdc3c7;
            border-radius: 6px;
        }

        input[type="range"]::-ms-thumb {
            width: 44px;
            height: 44px;
            background: #3498db;
            border-radius: 50%;
            cursor: pointer;
            margin-top: 0px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.2);
        }
        
        .sensitivity-labels {
            display: flex;
            justify-content: space-between;
            font-size: 12px;
            color: #7f8c8d;
            margin-top: 3px;
        }
        
        #jdPWMSensitivityGroup {
            background: #f8f9fa;
            padding: 10px;
            border-radius: 8px;
            margin-top: 8px;
        }
        
        .nav-buttons {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-bottom: 20px;
        }
    </style>
    <script>
        function saveSettings() {
            const settings = {
                udpPassthrough: document.getElementById('udpPassthrough').checked,
                pwmBrakeMode: document.getElementById('pwmBrakeMode').checked,
                softStartDuration: parseInt(document.getElementById('softStartDuration').value),
                buzzerVolume: parseInt(document.getElementById('buzzerVolume').value),
                invertWorkSwitch: document.getElementById('invertWorkSwitch').checked,
                ledBrightness: parseInt(document.getElementById('ledBrightness').value)
            };
            
            // Show saving status
            document.getElementById('status').innerHTML = 
                '<div class="status">Saving settings...</div>';
            
            fetch('/api/device/settings', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(settings)
            })
            .then(response => response.json())
            .then(data => {
                if (data.status === 'saved') {
                    document.getElementById('status').innerHTML = 
                        '<div class="status success">Settings saved successfully!</div>';
                    setTimeout(() => {
                        document.getElementById('status').innerHTML = '';
                    }, 5000);
                } else {
                    document.getElementById('status').innerHTML = 
                        '<div class="status error">' + (data.message || 'Error saving settings') + '</div>';
                    setTimeout(() => {
                        document.getElementById('status').innerHTML = '';
                    }, 5000);
                }
            })
            .catch((error) => {
                document.getElementById('status').innerHTML = 
                    '<div class="status error">Error: ' + error + '</div>';
                setTimeout(() => {
                    document.getElementById('status').innerHTML = '';
                }, 5000);
            });
        }
        
        function loadSettings() {
            fetch('/api/device/settings')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('udpPassthrough').checked = data.udpPassthrough || false;
                    document.getElementById('pwmBrakeMode').checked = data.pwmBrakeMode || false;
                    document.getElementById('softStartDuration').value = data.softStartDuration || 500;
                    document.getElementById('buzzerVolume').value = (data.buzzerVolume !== undefined) ? data.buzzerVolume : 1;
                    document.getElementById('invertWorkSwitch').checked = data.invertWorkSwitch || false;
                    document.getElementById('ledBrightness').value = data.ledBrightness || 25;
                })
                .catch((error) => {
                    console.error('Error loading settings:', error);
                });
        }
        
        window.onload = function() {
            loadSettings();
        };
    </script>
</head>
<body>
    <div class="container">
        <h1>Device Settings</h1>
        
        <div class="nav-buttons">
            <button type="button" class="touch-button" style="background: #7f8c8d;" 
                    onclick="window.location.href='/'">
                Back to Home
            </button>
            <button type="submit" class="touch-button" form="settingsForm">
                Apply Changes
            </button>
        </div>
        
        <div id="status"></div>
        
        <form id="settingsForm" onsubmit="saveSettings(); return false;">
            <div class="card">
                <div class="toggle-container">
                    <div class="toggle-info">
                        <label for="udpPassthrough" class="toggle-label">GPS-UDP Passthrough</label>
                        <div class="help-text">
                            Send the receiver's NMEA straight to AgIO instead of PANDA/PAOGI. AgOpenWeb reads only PANDA/PAOGI, so leave this off when using AgOpenWeb.
                        </div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="udpPassthrough" name="udpPassthrough">
                        <span class="toggle-slider"></span>
                    </label>
                </div>
                
                <div class="toggle-container">
                    <div class="toggle-info">
                        <label for="pwmBrakeMode" class="toggle-label">PWM Motor Brake Mode</label>
                        <div class="help-text">
                            When enabled, PWM motors use brake mode (active braking). When disabled, motors use coast mode (free-wheeling). Only affects PWM-based motor drivers.
                        </div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="pwmBrakeMode" name="pwmBrakeMode">
                        <span class="toggle-slider"></span>
                    </label>
                </div>

                <div class="form-group" style="margin-top: 15px;">
                    <div style="display: flex; align-items: center; gap: 10px;">
                        <label for="softStartDuration" style="margin: 0; white-space: nowrap;">Motor Soft Start:</label>
                        <select id="softStartDuration" name="softStartDuration" style="width: auto; flex: 0 0 110px;">
                            <option value="0">OFF</option>
                            <option value="100">100 ms</option>
                            <option value="200">200 ms</option>
                            <option value="300">300 ms</option>
                            <option value="400">400 ms</option>
                            <option value="500" selected>500 ms</option>
                            <option value="600">600 ms</option>
                            <option value="700">700 ms</option>
                            <option value="800">800 ms</option>
                            <option value="900">900 ms</option>
                            <option value="1000">1000 ms</option>
                        </select>
                        <span class="help-text" style="margin: 0; flex: 1; font-size: 13px;">Ramps motor power on startup and direction changes. Direction changes use half duration.</span>
                    </div>
                </div>

                <div class="form-group" style="margin-top: 15px;">
                    <label for="buzzerVolume">Buzzer Volume:</label>
                    <select id="buzzerVolume" name="buzzerVolume">
                        <option value="1">Loud</option>
                        <option value="0">Quiet</option>
                        <option value="2">Off</option>
                    </select>
                    <div class="help-text" style="margin-top: 5px;">
                        Loud plays a multi-tone startup chime. Quiet plays a brief click. Off disables the buzzer entirely.
                    </div>
                </div>

                <div class="toggle-container" style="margin-top: 15px;">
                    <div class="toggle-info">
                        <label for="invertWorkSwitch" class="toggle-label">Invert Work Switch</label>
                        <div class="help-text">
                            The work switch input reads a closed contact as "work on". Turn this on for a switch that opens when working.
                        </div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="invertWorkSwitch" name="invertWorkSwitch">
                        <span class="toggle-slider"></span>
                    </label>
                </div>

                <div class="form-group" style="margin-top: 15px;">
                    <label for="ledBrightness">Status LED Brightness:</label>
                    <select id="ledBrightness" name="ledBrightness">
                        <option value="5">5%</option>
                        <option value="10">10%</option>
                        <option value="25">25% (Default)</option>
                        <option value="50">50%</option>
                        <option value="75">75%</option>
                        <option value="100">100%</option>
                    </select>
                </div>
            </div>

        </form>
    </div>
</body>
</html>
)rawliteral";

#endif // TOUCH_FRIENDLY_DEVICE_SETTINGS_PAGE_H