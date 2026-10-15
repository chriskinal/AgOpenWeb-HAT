// AgOpenWeb HAT. GPL-3.0-or-later.
//
// AOW HAT: a new page, not from the firmware (the Teensy's serial ports were fixed). Where each
// serial device is (GNSS receiver, RTK radio, IMU, Keya, NMEA out, RTCM out) as a matrix, each
// RS-232 port's baud rate (the firmware's RTK radio rate became its port's), NMEA out's
// sentences and rate, and what each device is doing, with the IMU's live readings. Uses the
// firmware pages' stylesheet and layout. Settings from /api/serial, live state from /api/status.

#ifndef SERIAL_PAGE_H
#define SERIAL_PAGE_H

const char SERIAL_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <title>Serial Communications - AgOpenWeb HAT</title>
    <link rel="stylesheet" href="/touch.css">
    <style>
        .help-text { font-size: 14px; color: #7f8c8d; margin-top: 5px; line-height: 1.3; }
        .nav-buttons { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-bottom: 20px; }
        /* AOW HAT: the serial port matrix */
        .port-matrix { width: 100%; border-collapse: collapse; font-size: 15px; }
        .port-matrix th { font-weight: 600; font-size: 13px; color: #7f8c8d; padding: 4px 2px; }
        .port-matrix td { text-align: center; padding: 8px 2px; border-top: 1px solid #ecf0f1; }
        .port-matrix td:first-child { text-align: left; font-weight: 600; }
        .port-matrix input[type=radio] { width: 22px; height: 22px; }
        .port-matrix th.clash { color: #c0392b; }
        .nmea-boxes { display: flex; flex-wrap: wrap; gap: 14px; }
        .nmea-boxes label { display: flex; align-items: center; gap: 6px; font-weight: normal; }
        .nmea-boxes input { width: 20px; height: 20px; }
        .port-live { font-size: 15px; padding: 10px; background: #f8f9fa; border-radius: 8px; line-height: 1.6; }

    </style>
    <script>
        function saveSettings() {
            const clash = portsProblem();
            if (clash) {
                document.getElementById('status').innerHTML = '<div class="status error">' + clash + '</div>';
                return;
            }
            const msg = document.getElementById('status');
            msg.innerHTML = '<div class="status">Saving settings...</div>';
            fetch('/api/serial', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(portsFromPage())
            })
            .then(response => response.json())
            .then(data => {
                if (data.status === 'saved' && data.restartRequired) {
                    if (confirm('Saved. Moving a device takes effect when the HAT software restarts. Steering stops for a few seconds. Restart now?')) {
                        msg.innerHTML = '<div class="status">Restarting...</div>';
                        fetch('/api/restart', { method: 'POST' }).finally(() => setTimeout(() => location.reload(), 4000));
                    } else {
                        msg.innerHTML = '<div class="status success">Saved. Takes effect at the next restart.</div>';
                    }
                } else if (data.status === 'saved') {
                    msg.innerHTML = '<div class="status success">Settings saved successfully!</div>';
                    setTimeout(() => { msg.innerHTML = ''; }, 5000);
                } else {
                    msg.innerHTML = '<div class="status error">' + esc(data.message || 'Error saving settings') + '</div>';
                }
            })
            .catch(error => {
                msg.innerHTML = '<div class="status error">Error: ' + esc(error) + '</div>';
            });
        }

        function loadSettings() {
            fetch('/api/serial')
                .then(response => response.json())
                .then(data => {
                    window.savedPorts = data;
                    portsToPage(data);
                })
                .catch(error => console.error('Error loading settings:', error));
        }

        // The serial port matrix. One row per device, one column per place; a device
        // is offered only the places that suit it. Each RS-232 port takes one device.
        const DEVICES = [
            ['gnss', 'GNSS Receiver', ['none', 'onboard', 'port1', 'port2', 'usb']],
            ['radio', 'RTK Radio', ['none', 'port1', 'port2']],
            ['imu', 'IMU', ['none', 'port1', 'port2', 'usb']],
            ['keya', 'Keya Motor', ['none', 'port1', 'port2']],
            ['nmeaOut', 'NMEA Out', ['none', 'port1', 'port2']],
            ['rtcmOut', 'RTCM Out', ['none', 'port1', 'port2']]
        ];
        const PLACES = ['none', 'onboard', 'port1', 'port2', 'usb'];
        const NAMES = {};
        DEVICES.forEach(d => NAMES[d[0]] = d[1]);
        const BAUDS = [4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];

        function buildMatrix() {
            const rows = document.getElementById('portRows');
            rows.innerHTML = DEVICES.map(d => '<tr><td>' + d[1] + '</td>' + PLACES.map(p =>
                '<td>' + (d[2].includes(p) ? '<input type="radio" name="where_' + d[0] + '" value="' + p + '" onchange="portsChanged()">' : '') + '</td>'
            ).join('') + '</tr>').join('');
            for (let i = 0; i < 2; ++i)
                document.getElementById('baud' + i).innerHTML = BAUDS.map(b => '<option value="' + b + '">' + b + '</option>').join('');
        }

        function whereOf(dev) {
            const el = document.querySelector('input[name="where_' + dev + '"]:checked');
            return el ? el.value : 'none';
        }

        function portsFromPage() {
            const ports = { baud: [parseInt(document.getElementById('baud0').value), parseInt(document.getElementById('baud1').value)] };
            DEVICES.forEach(d => ports[d[0]] = whereOf(d[0]));
            let mask = 0;
            [1, 2, 4, 8, 16].forEach(b => { if (document.getElementById('nmea' + b).checked) mask |= b; });
            ports.nmeaSentences = mask;
            ports.nmeaRateHz = parseInt(document.getElementById('nmeaRate').value);
            return ports;
        }

        function portsToPage(p) {
            DEVICES.forEach(d => {
                const el = document.querySelector('input[name="where_' + d[0] + '"][value="' + (p[d[0]] || 'none') + '"]');
                if (el) el.checked = true;
            });
            const baud = p.baud || [115200, 115200];
            for (let i = 0; i < 2; ++i) document.getElementById('baud' + i).value = baud[i];
            const mask = (p.nmeaSentences !== undefined) ? p.nmeaSentences : 3;
            [1, 2, 4, 8, 16].forEach(b => document.getElementById('nmea' + b).checked = (mask & b) !== 0);
            document.getElementById('nmeaRate').value = p.nmeaRateHz || 0;
            portsChanged();
        }

        function onPort(i) {  // the devices on RS-232 port i (0, 1)
            return DEVICES.map(d => d[0]).filter(dev => whereOf(dev) === 'port' + (i + 1));
        }

        function portsProblem() {
            for (let i = 0; i < 2; ++i) {
                const on = onPort(i);
                if (on.length > 1) return 'Port ' + (i + 1) + ' has ' + on.map(dev => NAMES[dev]).join(' and ') + '. Each port takes one device.';
            }
            return '';
        }

        function portsChanged() {
            const heads = document.querySelectorAll('.port-matrix th');
            for (let i = 0; i < 2; ++i) {
                const on = onPort(i);
                heads[3 + i].classList.toggle('clash', on.length > 1);
                const fixed = on.length === 1 && (on[0] === 'imu' || on[0] === 'keya');
                const sel = document.getElementById('baud' + i);
                sel.disabled = fixed || on.length === 0;
                document.getElementById('baudUse' + i).textContent =
                    on.length === 0 ? '(nothing on it)' : fixed ? '(' + NAMES[on[0]] + ': 115200)' : '(' + on.map(dev => NAMES[dev]).join(', ') + ')';
            }
            document.getElementById('nmeaGroup').style.display = whereOf('nmeaOut') === 'none' ? 'none' : '';
            const problem = portsProblem();
            const box = document.getElementById('portsError');
            box.textContent = problem;
            box.style.display = problem ? '' : 'none';
        }

        function esc(t) { return String(t).replace(/[&<>"']/g, c => '&#' + c.charCodeAt(0) + ';'); }

        function deg(v) { return (v >= 0 ? '+' : '') + v.toFixed(1) + '&deg;'; }

        function place(w) {
            return { onboard: 'onboard', port1: 'port 1', port2: 'port 2', usb: 'USB' }[w] || w;
        }

        let lastCounts = null;
        function pollStatus() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => {
                    // Live state, from the settings in force (the saved ones) and the counters.
                    const ser = data.serial || {};
                    const lines = [];
                    const saved = window.savedPorts || {};
                    const dev = (p) => p ? esc(p.replace('/dev/serial/by-id/', 'USB ')) : '';  // USB names come from the device
                    if (saved.gnss && saved.gnss !== 'none') {
                        const fix = data.gnss ? data.gnss.fixQuality : 0;
                        lines.push('<b>GNSS:</b> ' + (ser.gnssPort ? dev(ser.gnssPort) + ', fix ' + fix
                            : saved.gnss === 'usb' ? 'looking for it on USB...' : 'not open'));
                    }
                    const moving = (k) => lastCounts && ser[k] > lastCounts[k];
                    if (saved.radio && saved.radio !== 'none')
                        lines.push('<b>RTK radio:</b> ' + (moving('radioBytes') ? 'corrections coming in' : 'nothing coming in'));
                    if (saved.nmeaOut && saved.nmeaOut !== 'none')
                        lines.push('<b>NMEA out:</b> ' + (moving('nmeaOutSent') ? 'sending' : 'nothing to send') + ' (' + ser.nmeaOutSent + ' sent)');
                    if (saved.rtcmOut && saved.rtcmOut !== 'none')
                        lines.push('<b>RTCM out:</b> ' + (moving('rtcmOutBytes') ? 'sending' : 'nothing to send'));
                    document.getElementById('portLive').innerHTML = lines.length ? lines.join('<br>') : 'No serial devices set.';
                    lastCounts = ser;

                    const imu = data.imu || {};
                    const el = document.getElementById('imuLive');
                    const where = dev(imu.port);
                    if (imu.valid) {
                        el.innerHTML = '<b>' + esc(imu.type) + '</b> <span style="color: #7f8c8d; font-size: 13px;">' + where + '</span><br>' +
                            'Roll ' + deg(imu.roll) + ' &nbsp; Pitch ' + deg(imu.pitch) +
                            ' &nbsp; Heading ' + imu.heading.toFixed(1) + '&deg; &nbsp; Turn ' + deg(imu.yawRate) + '/s';
                    } else if (imu.type && imu.type !== 'None') {
                        el.innerHTML = '<b>' + esc(imu.type) + '</b> has stopped sending.';
                    } else if (imu.port) {
                        el.innerHTML = 'Listening on ' + where + ' for an IMU...';
                    } else if (saved.imu === 'usb') {
                        el.textContent = 'Looking for a TM171 on USB...';
                    } else {
                        el.textContent = saved.imu && saved.imu !== 'none' ? 'Not open.' : 'No IMU set.';
                    }
                })
                .catch(() => {})
                .finally(() => setTimeout(pollStatus, 1000));
        }

        window.onload = function() {
            buildMatrix();
            loadSettings();
            pollStatus();
        };
    </script>
</head>
<body>
    <div class="container">
        <h1>Serial Communications</h1>

        <div class="nav-buttons">
            <button type="button" class="touch-button" style="background: #7f8c8d;" onclick="window.location.href='/'">Back to Home</button>
            <button type="submit" class="touch-button" form="settingsForm">Apply Changes</button>
        </div>

        <div id="status"></div>

        <form id="settingsForm" onsubmit="saveSettings(); return false;">
            <div class="card">
                <div class="help-text" style="margin-bottom: 10px;">
                    Tell the HAT what is plugged in where. Each RS-232 port takes one device.
                </div>
                <table class="port-matrix">
                    <thead>
                        <tr><th></th><th>None</th><th>Onboard</th><th>Port 1</th><th>Port 2</th><th>USB</th></tr>
                    </thead>
                    <tbody id="portRows"></tbody>
                </table>
                <div id="portsError" class="status error" style="display: none; margin-top: 10px;"></div>

                <div class="form-group" style="margin-top: 15px;">
                    <label for="baud0">Port 1 Baud Rate: <span id="baudUse0" class="help-text"></span></label>
                    <select id="baud0" class="baud-select"></select>
                </div>
                <div class="form-group" style="margin-top: 10px;">
                    <label for="baud1">Port 2 Baud Rate: <span id="baudUse1" class="help-text"></span></label>
                    <select id="baud1" class="baud-select"></select>
                    <div class="help-text" style="margin-top: 5px;">
                        Match the device's own rate. An IMU or Keya motor always runs at 115200. Rates apply as soon as they are saved.
                    </div>
                </div>

                <div id="nmeaGroup" class="form-group" style="margin-top: 15px;">
                    <label>NMEA Out Sentences:</label>
                    <div class="nmea-boxes">
                        <label><input type="checkbox" id="nmea1"> GGA</label>
                        <label><input type="checkbox" id="nmea2"> VTG</label>
                        <label><input type="checkbox" id="nmea4"> RMC</label>
                        <label><input type="checkbox" id="nmea8"> ZDA</label>
                        <label><input type="checkbox" id="nmea16"> HDT</label>
                    </div>
                    <label for="nmeaRate" style="margin-top: 10px;">NMEA Out Rate:</label>
                    <select id="nmeaRate">
                        <option value="0">As the receiver sends them</option>
                        <option value="10">10 Hz</option>
                        <option value="5">5 Hz</option>
                        <option value="2">2 Hz</option>
                        <option value="1">1 Hz</option>
                    </select>
                    <div class="help-text" style="margin-top: 5px;">
                        The receiver's own sentences, for a rate controller, sprayer or yield monitor.
                    </div>
                </div>

            </div>
        </form>

            <div class="card">
                <div class="form-group" style="margin-top: 0;">
                    <label>Now:</label>
                    <div id="portLive" class="port-live">-</div>
                </div>

                <div class="form-group" style="margin-top: 15px;">
                    <label>IMU:</label>
                    <div id="imuLive" class="port-live">-</div>
                    <div class="help-text" style="margin-top: 5px;">
                        Mount the IMU with its arrow pointing to the front of the tractor. To check it: right side down (or tilt the IMU to the right) and roll goes positive; nose up and pitch goes positive; turn right and the heading goes up. If the IMU is turned 90 degrees, set "Use Y axis" in AgOpenWeb's steer settings.
                    </div>
                </div>
            </div>
    </div>
</body>
</html>
)rawliteral";

#endif // SERIAL_PAGE_H
