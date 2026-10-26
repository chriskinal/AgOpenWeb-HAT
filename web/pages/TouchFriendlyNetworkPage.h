// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware_Teensy_AiO-New-Dawn is copyright 2025 by the AOG Group
// Firmware_Teensy_AiO-New-Dawn is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
// Firmware_Teensy_AiO-New-Dawn is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// You should have received a copy of the GNU General Public License along with Firmware_Teensy_AiO-New-Dawn. If not, see <https://www.gnu.org/licenses/>.
// Like most Arduino code, portions of this are based on other open source Arduino code with a compatiable license.

// AOW HAT: copied from Firmware_Teensy_AiO_26 v26.5.5 web_pages/TouchFriendlyNetworkPage.h.
// HAT changes: the IP form is the firmware's (the API reports that the OS manages the address);
// a Wi-Fi card is added (/api/wifi): the CM4 as a client of known networks or as the cab's
// access point, the fallback to the access point after boot, and the networks in range.

// TouchFriendlyNetworkPage.h
// Touch-optimized network settings page with compact layout

#ifndef TOUCH_FRIENDLY_NETWORK_PAGE_H
#define TOUCH_FRIENDLY_NETWORK_PAGE_H

// #include <Arduino.h>  (AOW HAT: not needed; PROGMEM is defined by web/api.cpp)

const char TOUCH_FRIENDLY_NETWORK_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <title>Network Settings - AiO v26</title>
    <link rel="stylesheet" href="/touch.css">
    <style>
        /* Additional styles specific to network settings */
        .status-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 15px 0;
            border-bottom: 1px solid #ecf0f1;
            font-size: 18px;
        }
        
        .status-row span:first-child {
            font-weight: 500;
            color: #7f8c8d;
        }
        
        .status-row span:last-child {
            font-weight: 600;
            color: #2c3e50;
        }
        
        .ip-input-group {
            display: flex;
            align-items: center;
            justify-content: center;
            margin: 20px 0;
            font-size: 20px;
        }
        
        .ip-input-group input[type="number"] {
            width: 70px;
            padding: 15px;
            margin: 0 5px;
            font-size: 20px;
            text-align: center;
            border: 2px solid #bdc3c7;
            border-radius: 8px;
            -webkit-appearance: none;
            -moz-appearance: textfield;
        }
        
        .ip-input-group input[type="number"]::-webkit-inner-spin-button,
        .ip-input-group input[type="number"]::-webkit-outer-spin-button {
            -webkit-appearance: none;
            margin: 0;
        }
        
        .ip-input-group span {
            font-weight: 600;
            color: #34495e;
        }
        
        .fixed-octet {
            display: inline-block;
            min-width: 70px;
            text-align: center;
            font-weight: 600;
            color: #3498db;
        }
        
        .help-box {
            background: #ecf0f1;
            padding: 15px;
            border-radius: 8px;
            margin: 20px 0;
            text-align: center;
        }
        
        .help-box p {
            margin: 5px 0;
            font-size: 16px;
            color: #7f8c8d;
        }
        
        .nav-buttons {
            display: grid;
            grid-template-columns: 1fr 1fr 1fr;
            gap: 15px;
            margin-bottom: 20px;
        }
        
        @media (max-width: 600px) {
            .nav-buttons {
                grid-template-columns: 1fr;
            }
        }
        
        .reboot-btn {
            background: #e74c3c;
        }
        
        .reboot-btn:active {
            background: #c0392b;
        }
        .wifi-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #ecf0f1; font-size: 17px; }
        .wifi-row span:last-child { font-weight: 600; text-align: right; }
        .wifi-card label { display: block; font-weight: 500; margin: 14px 0 6px; }
        .wifi-card input[type="text"], .wifi-card input[type="password"], .wifi-card select {
            width: 100%; box-sizing: border-box; padding: 14px; font-size: 18px; border: 2px solid #bdc3c7; border-radius: 8px; background: white;
        }
        .wifi-modes { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
        .wifi-modes button { padding: 14px; font-size: 17px; border: 2px solid #3498db; border-radius: 8px; background: white; color: #3498db; }
        .wifi-modes button.on { background: #3498db; color: white; }
        .wifi-check { display: flex; align-items: center; gap: 10px; margin-top: 14px; font-size: 16px; }
        .wifi-check input { width: 24px; height: 24px; }
        .wifi-buttons { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 18px; }
        .wifi-msg { margin-top: 12px; font-size: 16px; color: #7f8c8d; }
        .wifi-msg.failed { color: #e74c3c; } .wifi-msg.done { color: #27ae60; }
    </style>
    <script>
        function saveIPSettings() {
            const octet1 = parseInt(document.getElementById('octet1').value);
            const octet2 = parseInt(document.getElementById('octet2').value);
            const octet3 = parseInt(document.getElementById('octet3').value);
            
            if (octet1 < 0 || octet1 > 255 || octet2 < 0 || octet2 > 255 || octet3 < 0 || octet3 > 255) {
                alert('Invalid IP address. Each octet must be between 0 and 255.');
                return false;
            }
            
            // Show saving status
            document.getElementById('status').innerHTML = 
                '<div class="status">Saving network settings...</div>';
            
            const formData = {
                ip: [octet1, octet2, octet3, 126]
            };
            
            fetch('/api/network/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(formData)
            })
            .then(response => response.json())
            .then(data => {
                if (data.status === 'ok') {
                    document.getElementById('status').innerHTML = 
                        '<div class="status success">Settings saved! Click Reboot to apply.</div>';
                    setTimeout(() => {
                        document.getElementById('status').innerHTML = '';
                    }, 5000);
                } else {
                    document.getElementById('status').innerHTML = 
                        '<div class="status error">Error saving settings: ' + data.error + '</div>';
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
            
            return false;
        }
        
        function rebootSystem() {
            if (confirm('Are you sure you want to reboot? The system will be unavailable for a moment.')) {
                document.getElementById('status').innerHTML = 
                    '<div class="status">System is rebooting...</div>';
                    
                fetch('/api/restart', {
                    method: 'POST'
                })
                .then(() => {
                    // Show countdown
                    let countdown = 10;
                    const interval = setInterval(() => {
                        document.getElementById('status').innerHTML = 
                            '<div class="status">Rebooting... Reconnect in ' + countdown + ' seconds</div>';
                        countdown--;
                        if (countdown < 0) {
                            clearInterval(interval);
                            document.getElementById('status').innerHTML = 
                                '<div class="status">You may need to refresh the page</div>';
                        }
                    }, 1000);
                })
                .catch((error) => {
                    document.getElementById('status').innerHTML = 
                        '<div class="status error">Error: ' + error + '</div>';
                    setTimeout(() => {
                        document.getElementById('status').innerHTML = '';
                    }, 5000);
                });
            }
        }
        
        function loadSettings() {
            // Load IP configuration
            fetch('/api/network/config')
            .then(response => response.json())
            .then(data => {
                if (data.ip && data.ip.length >= 3) {
                    document.getElementById('octet1').value = data.ip[0];
                    document.getElementById('octet2').value = data.ip[1];
                    document.getElementById('octet3').value = data.ip[2];
                }
            })
            .catch((error) => {
                console.error('Error loading settings:', error);
            });
            
            // Load current status
            fetch('/api/status')
            .then(response => response.json())
            .then(data => {
                if (data.network) {
                    document.getElementById('currentIP').textContent = data.network.ip || 'Unknown';
                    document.getElementById('linkSpeed').textContent = data.network.linkSpeed || '--';
                }
            })
            .catch((error) => {
                console.error('Error loading status:', error);
                document.getElementById('currentIP').textContent = 'Error';
                document.getElementById('linkSpeed').textContent = '--';
            });
        }
        
        // AOW HAT: the Wi-Fi card.
        let wifiMode = 'client', wifiLoaded = false, wifiLast = null;
        function wifiShowMode(m) {
            wifiMode = m;
            document.getElementById('wifiModeClient').className = m === 'client' ? 'on' : '';
            document.getElementById('wifiModeAp').className = m === 'ap' ? 'on' : '';
            document.getElementById('wifiClient').style.display = m === 'client' ? '' : 'none';
            document.getElementById('wifiAp').style.display = m === 'ap' ? '' : 'none';
            document.getElementById('wifiSaveAp').style.display = m === 'ap' && wifiLast && wifiLast.mode === 'client' ? '' : 'none';
        }
        function wifiChannels() {
            const band = document.getElementById('wifiBand').value;
            const sel = document.getElementById('wifiChannel');
            const keep = parseInt(sel.value);
            const list = band === '5' ? [36, 40, 44, 48] : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
            sel.innerHTML = '';
            for (const c of list) {
                const o = document.createElement('option');
                o.value = c; o.textContent = c;
                sel.appendChild(o);
            }
            sel.value = list.includes(keep) ? keep : list[band === '5' ? 0 : 5];
        }
        function wifiPickNetwork() {
            const v = document.getElementById('wifiNetwork').value;
            document.getElementById('wifiOtherRow').style.display = v === '' ? '' : 'none';
        }
        function wifiShow(d) {
            wifiLast = d;
            const now = document.getElementById('wifiNow');
            if (!d.available) now.textContent = d.unavailable;
            else if (d.apActive) now.textContent = 'Access point "' + d.ssid + '" at ' + d.ap.address + (d.fallbackActive ? ' (fallback)' : '');
            else if (d.connected) now.textContent = 'Client of "' + d.ssid + '", ' + (d.ip || 'no address');
            else now.textContent = 'Not connected';
            document.getElementById('wifiAtBoot').textContent = d.mode === 'ap' ? 'Access point' : 'Client';
            const msg = document.getElementById('wifiMsg');
            msg.textContent = d.job === 'idle' ? '' : d.message;
            msg.className = 'wifi-msg ' + d.job;
            // The network list: in range first, then the ones known but not seen.
            const sel = document.getElementById('wifiNetwork');
            const chosen = sel.value;
            sel.innerHTML = '';
            const names = [];
            for (const n of d.seen) names.push([n.ssid, n.ssid + ' (' + n.signal + '%' + (n.security ? ', ' + n.security : ', open') + (d.known.includes(n.ssid) ? ', saved' : '') + ')']);
            for (const k of d.known) if (!d.seen.some(n => n.ssid === k)) names.push([k, k + ' (saved, not in range)']);
            for (const [v, t] of names) {
                const o = document.createElement('option');
                o.value = v; o.textContent = t;
                sel.appendChild(o);
            }
            const other = document.createElement('option');
            other.value = ''; other.textContent = 'Other network...';
            sel.appendChild(other);
            const want = wifiLoaded ? chosen : (!d.apActive && d.connected ? d.ssid : (names.length ? names[0][0] : ''));
            sel.value = names.some(n => n[0] === want) ? want : (want === '' || !names.length ? '' : names[0][0]);
            wifiPickNetwork();
            if (!wifiLoaded) {
                wifiLoaded = true;
                document.getElementById('wifiApSsid').value = d.ap.ssid;
                document.getElementById('wifiApPassword').placeholder = d.ap.exists ? 'Leave blank to keep the saved one' : '8 to 63 characters';
                document.getElementById('wifiBand').value = d.ap.band;
                wifiChannels();
                document.getElementById('wifiChannel').value = d.ap.channel;
                document.getElementById('wifiFallback').checked = d.fallback;
                document.getElementById('wifiApHint').textContent = 'Tablets join it and open http://' + d.ap.address +
                    ':5174 for AgOpenWeb, or http://' + d.ap.address + '/network for this page.' +
                    (d.country && d.country !== '00' ? ' Wi-Fi country: ' + d.country + '.' : ' The Wi-Fi country is not set, so only 2.4 GHz.');
                wifiShowMode(d.mode);
            }
            document.getElementById('wifiSaveAp').style.display = wifiMode === 'ap' && d.mode === 'client' ? '' : 'none';
            const busy = !d.available || d.job === 'switching';
            document.getElementById('wifiSwitch').disabled = busy;
            document.getElementById('wifiSaveAp').disabled = busy;
        }
        function wifiPoll() {
            fetch('/api/wifi').then(r => r.json()).then(wifiShow).catch(() => {
                document.getElementById('wifiNow').textContent = 'Not reachable (the Wi-Fi may be switching)';
            });
        }
        function wifiPost(body, onOk) {
            fetch('/api/wifi', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
                .then(r => r.json()).then(d => {
                    const msg = document.getElementById('wifiMsg');
                    if (d.status === 'error') { msg.textContent = d.message; msg.className = 'wifi-msg failed'; }
                    else if (onOk) onOk();
                    wifiPoll();
                })
                .catch(e => { const msg = document.getElementById('wifiMsg'); msg.textContent = 'Error: ' + e; msg.className = 'wifi-msg failed'; });
        }
        function wifiApBody(activate) {
            return { mode: 'ap', ssid: document.getElementById('wifiApSsid').value.trim(),
                     password: document.getElementById('wifiApPassword').value,
                     band: document.getElementById('wifiBand').value,
                     channel: parseInt(document.getElementById('wifiChannel').value), activate: activate };
        }
        function wifiSwitch() {
            let body, where;
            if (wifiMode === 'ap') {
                body = wifiApBody(true);
                where = 'join the Wi-Fi "' + body.ssid + '" on this device, then open http://' + wifiLast.ap.address + '/network';
            } else {
                const pick = document.getElementById('wifiNetwork').value;
                body = { mode: 'client', ssid: pick === '' ? document.getElementById('wifiOther').value.trim() : pick,
                         password: document.getElementById('wifiPassword').value };
                where = 'join "' + body.ssid + '" on this device, then open http://' + wifiLast.hostname + '.local/network';
            }
            if (!confirm('This page loses its connection if it reached the unit over Wi-Fi. To come back, ' + where +
                         '.\n\nIf the switch fails, the unit goes back to the connection it has now.')) return;
            wifiPost(body, () => {
                const msg = document.getElementById('wifiMsg');
                msg.textContent = 'Switching in a moment. To come back, ' + where + '.';
                msg.className = 'wifi-msg';
                document.getElementById('wifiPassword').value = '';
                document.getElementById('wifiApPassword').value = '';
            });
        }
        function wifiSaveAp() { wifiPost(wifiApBody(false), () => { document.getElementById('wifiApPassword').value = ''; }); }
        function wifiSetFallback(box) { wifiPost({ fallback: box.checked }); }

        window.onload = function() { loadSettings(); wifiPoll(); setInterval(wifiPoll, 5000); };
    </script>
</head>
<body>
    <div class="container">
        <h1>Network Settings</h1>
        
        <div class="nav-buttons">
            <button type="button" class="touch-button" style="background: #7f8c8d;" 
                    onclick="window.location.href='/'">
                Back to Home
            </button>
            <button type="button" class="touch-button" onclick="saveIPSettings()">
                Apply Changes
            </button>
            <button type="button" class="touch-button reboot-btn" onclick="rebootSystem()">
                Reboot
            </button>
        </div>
        
        <div id="status"></div>
        
        <div class="card">
            <div class="status-row">
                <span>IP: <span id="currentIP">Loading...</span></span>
                <span>Speed: <span id="linkSpeed">--</span> Mbps</span>
            </div>
            
            <form onsubmit="return false;">
                <div class="form-group">
                    <label style="text-align: center; display: block; margin-bottom: 10px;">
                        Configure IP Address
                    </label>
                    <div class="ip-input-group">
                        <input type="number" id="octet1" min="0" max="255" required>
                        <span>.</span>
                        <input type="number" id="octet2" min="0" max="255" required>
                        <span>.</span>
                        <input type="number" id="octet3" min="0" max="255" required>
                        <span>.</span>
                        <span class="fixed-octet">126</span>
                    </div>
                    <div class="help-text" style="text-align: center;">
                        The last octet is fixed at 126 for the steer module
                    </div>
                </div>
            </form>
        </div>
        
        <div class="card wifi-card">
            <h2 style="margin-top: 0;">Wi-Fi</h2>
            <div class="wifi-row"><span>Now</span><span id="wifiNow">Loading...</span></div>
            <div class="wifi-row"><span>At boot</span><span id="wifiAtBoot">--</span></div>
            <div class="wifi-modes">
                <button type="button" id="wifiModeClient" onclick="wifiShowMode('client')">Client</button>
                <button type="button" id="wifiModeAp" onclick="wifiShowMode('ap')">Access point (in-cab)</button>
            </div>
            <div id="wifiClient">
                <label for="wifiNetwork">Network</label>
                <select id="wifiNetwork" onchange="wifiPickNetwork()"></select>
                <div id="wifiOtherRow" style="display: none;">
                    <label for="wifiOther">Network name (SSID)</label>
                    <input type="text" id="wifiOther" maxlength="32" autocapitalize="off" autocorrect="off">
                </div>
                <label for="wifiPassword">Password</label>
                <input type="password" id="wifiPassword" maxlength="63" placeholder="Blank: the saved one, or an open network">
                <div class="wifi-check">
                    <input type="checkbox" id="wifiFallback" onchange="wifiSetFallback(this)">
                    <span>If no known network is found after boot, start the access point</span>
                </div>
                <div class="help-text">The fallback needs the access point saved once (Access point, then Save).</div>
            </div>
            <div id="wifiAp" style="display: none;">
                <label for="wifiApSsid">Access point name (SSID)</label>
                <input type="text" id="wifiApSsid" maxlength="32" autocapitalize="off" autocorrect="off">
                <label for="wifiApPassword">Password (WPA2)</label>
                <input type="password" id="wifiApPassword" maxlength="63">
                <label for="wifiBand">Band</label>
                <select id="wifiBand" onchange="wifiChannels()">
                    <option value="2.4">2.4 GHz (longer range)</option>
                    <option value="5">5 GHz</option>
                </select>
                <label for="wifiChannel">Channel</label>
                <select id="wifiChannel"></select>
                <div class="help-text" id="wifiApHint"></div>
            </div>
            <div class="wifi-buttons">
                <button type="button" class="touch-button" id="wifiSwitch" onclick="wifiSwitch()">Switch Wi-Fi</button>
                <button type="button" class="touch-button" id="wifiSaveAp" style="background: #7f8c8d; display: none;" onclick="wifiSaveAp()">Save (no switch)</button>
            </div>
            <div class="wifi-msg" id="wifiMsg"></div>
        </div>

        <div class="help-box">
            <p><strong>Note:</strong> After saving, you must reboot for the new IP to take effect.</p>
            <p>The network will be briefly unavailable during reboot.</p>
        </div>
    </div>
</body>
</html>
)rawliteral";

#endif // TOUCH_FRIENDLY_NETWORK_PAGE_H