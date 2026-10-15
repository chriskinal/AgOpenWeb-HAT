// AgOpenWeb HAT. GPL-3.0-or-later.
//
// AOW HAT: a new page, not from the firmware (the Teensy's serial ports were fixed). Where each
// serial device is (GNSS receiver, RTK radio, IMU, NMEA out, RTCM out) as a matrix, each RS-232
// port's baud rate (the firmware's RTK radio rate became its port's), NMEA out's sentences and
// rate, and what is coming in on each port: parsed for a receiver, IMU or RTCM, else the first
// 20 characters of the last lines. A Keya serial motor is not offered (settings file only). Uses the
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
        .port-matrix { width: 100%; border-collapse: collapse; font-size: 15px; table-layout: fixed; }
        .port-matrix th { font-weight: 600; font-size: 13px; color: #7f8c8d; padding: 4px 0; text-align: center; }
        .port-matrix td { text-align: center; vertical-align: middle; padding: 8px 0; border-top: 1px solid #ecf0f1; }
        .port-matrix th:first-child { width: 30%; }
        .port-matrix td:first-child { text-align: left; font-weight: 600; }
        .port-matrix input[type=radio] { width: 22px; height: 22px; margin: 0; vertical-align: middle; }
        .port-matrix th.clash { color: #c0392b; }
        .nmea-boxes { display: flex; flex-wrap: wrap; gap: 14px; }
        .nmea-boxes label { display: flex; align-items: center; gap: 6px; font-weight: normal; }
        .nmea-boxes input { width: 20px; height: 20px; }
        .port-live { font-size: 15px; padding: 10px; background: #f8f9fa; border-radius: 8px; line-height: 1.6; margin-bottom: 10px; }
        .port-live .head { font-weight: 600; }
        .port-live .head span { font-weight: normal; color: #7f8c8d; font-size: 13px; }
        .port-live pre { margin: 4px 0 0; font-family: Menlo, Consolas, monospace; font-size: 14px; white-space: pre; overflow-x: auto; }
        .port-live .quiet { color: #7f8c8d; }

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
            ports.keya = 'none';  // not on this page: a Keya serial motor is set only in the settings file
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
                const fixed = on.length === 1 && on[0] === 'imu';
                const sel = document.getElementById('baud' + i);
                sel.disabled = fixed;
                if (fixed) sel.value = 115200;
                document.getElementById('baudUse' + i).textContent =
                    on.length === 0 ? '(nothing set: listening)' : fixed ? '(IMU: always 115200)' : '(' + on.map(dev => NAMES[dev]).join(', ') + ')';
            }
            document.getElementById('nmeaGroup').style.display = whereOf('nmeaOut') === 'none' ? 'none' : '';
            const problem = portsProblem();
            const box = document.getElementById('portsError');
            box.textContent = problem;
            box.style.display = problem ? '' : 'none';
        }

        function esc(t) { return String(t).replace(/[&<>"']/g, c => '&#' + c.charCodeAt(0) + ';'); }

        function deg(v) { return (v >= 0 ? '+' : '') + v.toFixed(1) + '&deg;'; }

        const FIX = { 0: 'no fix', 1: 'GPS', 2: 'DGPS', 3: 'PPS', 4: 'RTK fixed', 5: 'RTK float', 6: 'dead reckoning' };
        const usbName = (p) => p ? esc(p.replace('/dev/serial/by-id/', '')) : '';  // USB names come from the device

        function gnssView(data) {
            const g = data.gnss || {};
            return (FIX[g.fixQuality] || 'fix ' + g.fixQuality) + ', ' + g.satellites + ' satellites';
        }

        function imuView(imu) {
            if (imu.valid)
                return '<b>' + esc(imu.type) + '</b> &nbsp; Roll ' + deg(imu.roll) + ' &nbsp; Pitch ' + deg(imu.pitch) +
                    ' &nbsp; Heading ' + imu.heading.toFixed(1) + '&deg; &nbsp; Turn ' + deg(imu.yawRate) + '/s';
            if (imu.type && imu.type !== 'None') return esc(imu.type) + ' has stopped sending.';
            return '';
        }

        function rate(bytes) {
            return bytes >= 1000 ? (bytes / 1000).toFixed(1) + ' kB/s' : bytes + ' B/s';
        }

        // What is coming in on a port: parsed when it is something the HAT reads, else the first
        // 20 characters of the last lines.
        function rawView(v, perSec) {
            if (!v.open) return '<span class="quiet">Port not available.</span>';
            if (v.msSinceIn === undefined || v.msSinceIn > 3000) return '<span class="quiet">Nothing coming in.</span>';
            if (v.rtcm && v.rtcm.length)
                return 'RTCM ' + v.rtcm.join(', ') + ' &nbsp; <span class="quiet">' + rate(perSec) + '</span>';
            return '<span class="quiet">' + rate(perSec) + '</span><pre>' + (v.lines || []).map(esc).join('\n') + '</pre>';
        }

        let lastIn = [null, null], lastPoll = 0;
        function pollStatus() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => {
                    const ser = data.serial || {};
                    const saved = window.savedPorts || {};
                    const imu = data.imu || {};
                    const now = Date.now();
                    const secs = lastPoll ? (now - lastPoll) / 1000 : 1;
                    lastPoll = now;
                    const blocks = [];
                    const block = (head, use, body) => blocks.push('<div class="port-live"><div class="head">' + head +
                        (use ? ' <span>' + use + '</span>' : '') + '</div>' + body + '</div>');

                    (ser.rs232 || []).forEach((v, i) => {
                        const perSec = lastIn[i] === null ? 0 : Math.max(0, Math.round((v.bytesIn - lastIn[i]) / secs));
                        lastIn[i] = v.bytesIn;
                        const use = v.use;
                        let body;
                        if (use === 'gnss') body = ser.gnssPort ? gnssView(data) : '<span class="quiet">Not open.</span>';
                        else if (use === 'imu') body = imuView(imu) || rawView(v, perSec);
                        else if (use === 'nmeaOut' || use === 'rtcmOut') {
                            const sent = use === 'nmeaOut' ? ser.nmeaOutSent + ' sentences sent' : ser.rtcmOutBytes + ' bytes sent';
                            body = sent + (v.bytesIn ? '<br>' + rawView(v, perSec) : '');
                        } else body = rawView(v, perSec);
                        block('Port ' + (i + 1), use === 'none' ? 'nothing set' : NAMES[use] || use, body);
                    });
                    if (saved.gnss === 'onboard') block('Onboard', 'GNSS Receiver', ser.gnssPort ? gnssView(data) : '<span class="quiet">Not open.</span>');
                    if (saved.gnss === 'usb')
                        block('USB', 'GNSS Receiver', ser.gnssPort ? usbName(ser.gnssPort) + '<br>' + gnssView(data) : '<span class="quiet">Looking for it...</span>');
                    if (saved.imu === 'usb')
                        block('USB', 'IMU', imu.port ? usbName(imu.port) + '<br>' + (imuView(imu) || '<span class="quiet">Listening...</span>')
                                                    : '<span class="quiet">Looking for a TM171...</span>');
                    document.getElementById('portLive').innerHTML = blocks.join('');
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
                        Match the device's own rate; below, what comes in should then be readable. An IMU always runs at 115200. Rates apply as soon as they are saved.
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
                    <label>Coming In Now:</label>
                    <div id="portLive">-</div>
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
