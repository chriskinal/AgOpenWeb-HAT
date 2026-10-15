// Firmware_Teensy_AiO-New-Dawn is copyright 2025 by the AOG Group
// Firmware_Teensy_AiO-New-Dawn is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
// Firmware_Teensy_AiO-New-Dawn is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// You should have received a copy of the GNU General Public License along with Firmware_Teensy_AiO-New-Dawn. If not, see <https://www.gnu.org/licenses/>.
// Like most Arduino code, portions of this are based on other open source Arduino code with a compatiable license.

// AOW HAT: copied from Firmware_Teensy_AiO_26 v26.5.5 web_pages/TouchFriendlyDeviceSettingsPage.h.
// HAT changes: no VWAS, encoder or JD PWM controls (no such hardware); adds Invert Work Switch and LED
// Brightness (firmware settings with no page); passthrough help mentions AgOpenWeb. A Serial Ports
// and IMU card (what each RS-232 port is for, a USB IMU, the IMU's live readings): the RTK radio
// baud rate moved into it, shown when a port is the radio.

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
                serialRadioBaud: parseInt(document.getElementById('serialRadioBaud').value),
                buzzerVolume: parseInt(document.getElementById('buzzerVolume').value),
                invertWorkSwitch: document.getElementById('invertWorkSwitch').checked,
                ledBrightness: parseInt(document.getElementById('ledBrightness').value),
                rs232Port1: parseInt(document.getElementById('rs232Port1').value),
                rs232Port2: parseInt(document.getElementById('rs232Port2').value),
                usbImu: document.getElementById('usbImu').checked
            };
            const clash = portsProblem();
            if (clash) {
                document.getElementById('status').innerHTML = '<div class="status error">' + clash + '</div>';
                return;
            }
            
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
                if (data.status === 'saved' && data.restartRequired) {
                    if (confirm('Saved. The serial port settings take effect when the HAT software restarts. Steering stops for a few seconds. Restart now?')) {
                        document.getElementById('status').innerHTML = '<div class="status">Restarting...</div>';
                        fetch('/api/restart', { method: 'POST' }).finally(() => setTimeout(() => location.reload(), 4000));
                    } else {
                        document.getElementById('status').innerHTML =
                            '<div class="status success">Saved. The serial port settings take effect at the next restart.</div>';
                    }
                } else if (data.status === 'saved') {
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
                    document.getElementById('serialRadioBaud').value = data.serialRadioBaud || 115200;
                    document.getElementById('buzzerVolume').value = (data.buzzerVolume !== undefined) ? data.buzzerVolume : 1;
                    document.getElementById('invertWorkSwitch').checked = data.invertWorkSwitch || false;
                    document.getElementById('ledBrightness').value = data.ledBrightness || 25;
                    document.getElementById('rs232Port1').value = (data.rs232Port1 !== undefined) ? data.rs232Port1 : 1;
                    document.getElementById('rs232Port2').value = (data.rs232Port2 !== undefined) ? data.rs232Port2 : 3;
                    document.getElementById('usbImu').checked = data.usbImu || false;
                    portsChanged();
                })
                .catch((error) => {
                    console.error('Error loading settings:', error);
                });
        }
        
        // AOW HAT: the serial ports card. Each use on one port only, and one IMU.
        function portsProblem() {
            const p1 = parseInt(document.getElementById('rs232Port1').value);
            const p2 = parseInt(document.getElementById('rs232Port2').value);
            const usb = document.getElementById('usbImu').checked;
            if (p1 !== 0 && p1 === p2) return 'Both RS-232 ports are set to the same thing.';
            if (usb && (p1 === 2 || p2 === 2)) return 'Only one IMU: turn off the USB IMU or set the RS-232 port to something else.';
            return '';
        }

        function portsChanged() {
            const p1 = parseInt(document.getElementById('rs232Port1').value);
            const p2 = parseInt(document.getElementById('rs232Port2').value);
            document.getElementById('radioBaudGroup').style.display = (p1 === 1 || p2 === 1) ? '' : 'none';
            const problem = portsProblem();
            const box = document.getElementById('portsError');
            box.textContent = problem;
            box.style.display = problem ? '' : 'none';
        }

        function esc(t) { return String(t).replace(/[&<>"']/g, c => '&#' + c.charCodeAt(0) + ';'); }

        function deg(v) { return (v >= 0 ? '+' : '') + v.toFixed(1) + '&deg;'; }

        function pollImu() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => {
                    const imu = data.imu || {};
                    const el = document.getElementById('imuLive');
                    const where = imu.port ? esc(imu.port.replace('/dev/serial/by-id/', 'USB ')) : '';  // USB names come from the device
                    if (imu.valid) {
                        el.innerHTML = '<b>' + esc(imu.type) + '</b> <span style="color: #7f8c8d; font-size: 13px;">' + where + '</span><br>' +
                            'Roll ' + deg(imu.roll) + ' &nbsp; Pitch ' + deg(imu.pitch) +
                            ' &nbsp; Heading ' + imu.heading.toFixed(1) + '&deg; &nbsp; Turn ' + deg(imu.yawRate) + '/s';
                    } else if (imu.type && imu.type !== 'None') {
                        el.innerHTML = '<b>' + esc(imu.type) + '</b> has stopped sending.';
                    } else if (imu.port) {
                        el.innerHTML = 'Listening on ' + where + ' for an IMU...';
                    } else {
                        el.textContent = 'No IMU.';
                    }
                })
                .catch(() => {})
                .finally(() => setTimeout(pollImu, 500));
        }

        window.onload = function() {
            loadSettings();
            pollImu();
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

            <div class="card">
                <h2 style="margin-top: 0;">Serial Ports and IMU</h2>
                <div class="form-group">
                    <label for="rs232Port1">RS-232 Port 1:</label>
                    <select id="rs232Port1" name="rs232Port1" onchange="portsChanged()">
                        <option value="0">Off</option>
                        <option value="1">RTK Radio (Default)</option>
                        <option value="2">IMU (BNO085 or TM171)</option>
                        <option value="3">Keya Motor</option>
                    </select>
                </div>
                <div class="form-group" style="margin-top: 15px;">
                    <label for="rs232Port2">RS-232 Port 2:</label>
                    <select id="rs232Port2" name="rs232Port2" onchange="portsChanged()">
                        <option value="0">Off</option>
                        <option value="1">RTK Radio</option>
                        <option value="2">IMU (BNO085 or TM171)</option>
                        <option value="3">Keya Motor (Default)</option>
                    </select>
                    <div class="help-text" style="margin-top: 5px;">
                        What is plugged into each RS-232 port. An IMU on RS-232 needs a TTL to RS-232 converter. A Keya motor is looked for when the HAT starts.
                    </div>
                </div>

                <div class="toggle-container" style="margin-top: 15px;">
                    <div class="toggle-info">
                        <label for="usbImu" class="toggle-label">USB IMU (TM171)</label>
                        <div class="help-text">
                            Use a TM171 plugged into a USB port. It is found by itself, and picked up again if it is unplugged and plugged back in.
                        </div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="usbImu" name="usbImu" onchange="portsChanged()">
                        <span class="toggle-slider"></span>
                    </label>
                </div>

                <div class="form-group" id="radioBaudGroup" style="margin-top: 15px;">
                    <label for="serialRadioBaud">RTK Radio Baud Rate:</label>
                    <select id="serialRadioBaud" name="serialRadioBaud">
                        <option value="4800">4800</option>
                        <option value="9600">9600</option>
                        <option value="14400">14400</option>
                        <option value="19200">19200</option>
                        <option value="38400">38400</option>
                        <option value="57600">57600</option>
                        <option value="115200">115200 (Default)</option>
                        <option value="230400">230400</option>
                        <option value="460800">460800</option>
                        <option value="921600">921600</option>
                    </select>
                    <div class="help-text" style="margin-top: 5px;">
                        Set the baud rate for the serial radio (XBee) used for RTK corrections. Some radios cannot easily change their baud rate, so match this setting to your radio's configuration.
                    </div>
                </div>

                <div id="portsError" class="status error" style="display: none; margin-top: 15px;"></div>

                <div class="form-group" style="margin-top: 15px;">
                    <label>IMU now:</label>
                    <div id="imuLive" style="font-size: 18px; padding: 10px; background: #f8f9fa; border-radius: 8px;">-</div>
                    <div class="help-text" style="margin-top: 5px;">
                        Mount the IMU with its arrow pointing to the front of the tractor. To check it: right side down (or tilt the IMU to the right) and roll goes positive; nose up and pitch goes positive; turn right and the heading goes up. If the IMU is turned 90 degrees, set "Use Y axis" in AgOpenWeb's steer settings.
                    </div>
                </div>
            </div>
        </form>
    </div>
</body>
</html>
)rawliteral";

#endif // TOUCH_FRIENDLY_DEVICE_SETTINGS_PAGE_H