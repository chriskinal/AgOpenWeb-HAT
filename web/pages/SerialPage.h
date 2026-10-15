// AgOpenWeb HAT. GPL-3.0-or-later.
//
// AOW HAT: a new page, not from the firmware (the Teensy's serial ports were fixed). What is
// plugged in is found from what it sends (a GNSS receiver, RTK radio or IMU), so only the
// outputs are set here: NMEA out and RTCM out, each on an RS-232 port or a USB serial device,
// with NMEA out's sentences and rate, whether it carries the app's corrected position (PGN 100,
// as AgOpenGPS's GPS_Out), and each RS-232 port's rate (auto, or pinned). Below, what is going
// out (each output's port, position source, sentences or RTCM messages, rate and drops) and what
// is coming in on each port and USB device: parsed for what the HAT reads, else the first 20
// characters of the last lines. Uses the firmware pages' stylesheet and layout. Settings from
// /api/serial, live state from /api/status.

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
        .two { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
        .nmea-boxes { display: flex; flex-wrap: wrap; gap: 14px; }
        .nmea-boxes label { display: flex; align-items: center; gap: 6px; font-weight: normal; }
        .nmea-boxes input, .check input { width: 20px; height: 20px; }
        .check { display: flex; align-items: center; gap: 8px; font-weight: normal; margin-top: 10px; }
        .port-live { font-size: 15px; padding: 10px; background: #f8f9fa; border-radius: 8px; line-height: 1.6; margin-bottom: 10px; }
        .port-live .head { font-weight: 600; }
        .port-live .head span { font-weight: normal; color: #7f8c8d; font-size: 13px; }
        .port-live pre { margin: 4px 0 0; font-family: Menlo, Consolas, monospace; font-size: 14px; white-space: pre; overflow-x: auto; }
        .quiet { color: #7f8c8d; }
        .port-live .head .found, .port-live .found { color: #27ae60; font-weight: 600; }
        .warn { color: #c0392b; }
    </style>
    <script>
        const RATES = [4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
        const FIX = { 0: 'no fix', 1: 'GPS', 2: 'DGPS', 3: 'PPS', 4: 'RTK fixed', 5: 'RTK float', 6: 'dead reckoning' };
        const KIND = { gnss: 'GNSS receiver', radio: 'RTK radio', imu: 'IMU' };
        let saved = {};       // the settings in force
        let usbNames = [];    // USB serial devices plugged in that can take an output

        function esc(t) { return String(t).replace(/[&<>"']/g, c => '&#' + c.charCodeAt(0) + ';'); }
        function deg(v) { return (v >= 0 ? '+' : '') + v.toFixed(1) + '&deg;'; }
        function rate(b) { return b >= 1000 ? (b / 1000).toFixed(1) + ' kB/s' : b + ' B/s'; }

        // An output select: Off, the two ports, and the USB devices (plus the chosen one if it
        // is not plugged in). Values: none, port1, port2, usb:<name>.
        function fillOut(id, where, usb) {
            const sel = document.getElementById(id);
            const keep = sel.options.length ? sel.value : (where === 'usb' ? 'usb:' + usb : where);
            const names = usbNames.slice();
            if (usb && !names.includes(usb)) names.push(usb);
            sel.innerHTML = '<option value="none">Off</option><option value="port1">Port 1</option><option value="port2">Port 2</option>' +
                names.map(n => '<option value="usb:' + esc(n) + '">USB ' + esc(n) + (usbNames.includes(n) ? '' : ' (not plugged in)') + '</option>').join('');
            sel.value = keep;
            if (sel.value !== keep) sel.value = 'none';
        }

        function outValue(id) {
            const v = document.getElementById(id).value;
            return v.startsWith('usb:') ? ['usb', v.slice(4)] : [v, ''];
        }

        function toPage(p) {
            fillOut('nmeaOut', p.nmeaOut, p.nmeaOutUsb);
            fillOut('rtcmOut', p.rtcmOut, p.rtcmOutUsb);
            for (let i = 0; i < 2; ++i) {
                const sel = document.getElementById('rate' + i);
                sel.innerHTML = '<option value="0">Auto</option>' + RATES.map(r => '<option value="' + r + '">' + r + '</option>').join('');
                sel.value = (p.rate || [0, 0])[i];
            }
            const mask = p.nmeaSentences !== undefined ? p.nmeaSentences : 3;
            [1, 2, 4, 8, 16].forEach(b => document.getElementById('nmea' + b).checked = (mask & b) !== 0);
            document.getElementById('nmeaRate').value = p.nmeaRateHz || 0;
            document.getElementById('nmeaCorrected').checked = p.nmeaCorrected !== false;
            changed();
        }

        function fromPage() {
            const nmea = outValue('nmeaOut'), rtcm = outValue('rtcmOut');
            let mask = 0;
            [1, 2, 4, 8, 16].forEach(b => { if (document.getElementById('nmea' + b).checked) mask |= b; });
            return {
                nmeaOut: nmea[0], nmeaOutUsb: nmea[1], rtcmOut: rtcm[0], rtcmOutUsb: rtcm[1],
                rate: [0, 1].map(i => parseInt(document.getElementById('rate' + i).value)),
                nmeaSentences: mask, nmeaRateHz: parseInt(document.getElementById('nmeaRate').value),
                nmeaCorrected: document.getElementById('nmeaCorrected').checked
            };
        }

        function changed() {
            document.getElementById('nmeaGroup').style.display = document.getElementById('nmeaOut').value === 'none' ? 'none' : '';
        }

        function saveSettings() {
            const msg = document.getElementById('status');
            msg.innerHTML = '<div class="status">Saving settings...</div>';
            fetch('/api/serial', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(fromPage()) })
                .then(response => response.json())
                .then(data => {
                    if (data.status === 'saved') {
                        saved = fromPage();
                        msg.innerHTML = '<div class="status success">Saved. In effect now.</div>';
                        setTimeout(() => { msg.innerHTML = ''; }, 5000);
                    } else {
                        msg.innerHTML = '<div class="status error">' + esc(data.message || 'Error saving settings') + '</div>';
                    }
                })
                .catch(error => { msg.innerHTML = '<div class="status error">Error: ' + esc(error) + '</div>'; });
        }

        function loadSettings() {
            fetch('/api/serial')
                .then(response => response.json())
                .then(data => { saved = data; toPage(data); })
                .catch(error => console.error('Error loading settings:', error));
        }

        function gnssView(data) {
            const g = data.gnss || {};
            return (FIX[g.fixQuality] || 'fix ' + g.fixQuality) + ', ' + g.satellites + ' satellites';
        }

        function imuView(imu) {
            if (!imu.valid) return '';
            return '<b>' + esc(imu.type) + '</b> &nbsp; Roll ' + deg(imu.roll) + ' &nbsp; Pitch ' + deg(imu.pitch) +
                ' &nbsp; Heading ' + imu.heading.toFixed(1) + '&deg; &nbsp; Turn ' + deg(imu.yawRate) + '/s';
        }

        function linesView(p, perSec) {
            if (p.msSinceIn === undefined || p.msSinceIn > 3000) return '<span class="quiet">Nothing coming in.</span>';
            if (p.rtcm && p.rtcm.length) return 'RTCM ' + p.rtcm.join(', ') + ' &nbsp; <span class="quiet">' + rate(perSec) + '</span>';
            return '<span class="quiet">' + rate(perSec) + '</span><pre>' + (p.lines || []).map(esc).join('\n') + '</pre>';
        }

        // Going Out Now: each output, where it goes and what it last wrote.
        function outView(ser, ports, perSec) {
            const where = key => {
                const p = ports.find(q => q[key] && q.open);
                return p ? (p.place === 'usb' ? 'USB <span>' + esc(p.name) + '</span>' : esc(p.name)) + ' &nbsp;<span>at ' + p.baud + '</span>' : '';
            };
            const blocks = [];
            const nmeaAt = where('nmeaOut');
            let nmea;
            if (saved.nmeaOut === undefined || saved.nmeaOut === 'none') nmea = '<span class="quiet">Off.</span>';
            else if (!nmeaAt) nmea = '<span class="quiet">Its port is not available.</span>';
            else {
                const src = ser.nmeaSource === 'corrected'
                    ? '<span class="found">Corrected position from AgOpenWeb</span>'
                    : 'Receiver position' + (saved.nmeaCorrected !== false
                        ? ' <span class="quiet">(no corrected position coming: tick Send corrected position in AgOpenWeb\'s Network IO)</span>' : '');
                const lines = ser.nmeaOutLast || [];
                nmea = src + '<br>' + perSec.nmea + ' sentences/s' +
                    (ser.nmeaOutDropped ? ', <span class="warn">' + ser.nmeaOutDropped + ' dropped: the port is too slow, lower the rate or raise the baud</span>' : '') +
                    (lines.length ? '<pre>' + lines.map(esc).join('\n') + '</pre>' : '<div class="quiet">Nothing sent lately.</div>');
            }
            blocks.push('<div class="port-live"><div class="head">NMEA Out' + (nmeaAt ? ' &nbsp;<span>&rarr;</span> ' + nmeaAt : '') + '</div>' + nmea + '</div>');
            const rtcmAt = where('rtcmOut');
            let rtcm;
            if (saved.rtcmOut === undefined || saved.rtcmOut === 'none') rtcm = '<span class="quiet">Off.</span>';
            else if (!rtcmAt) rtcm = '<span class="quiet">Its port is not available.</span>';
            else if (!(ser.rtcmOutTypes || []).length) rtcm = '<span class="quiet">No corrections going out.</span>';
            else rtcm = 'RTCM ' + ser.rtcmOutTypes.join(', ') + ' &nbsp; <span class="quiet">' + rate(perSec.rtcm) + '</span>';
            blocks.push('<div class="port-live"><div class="head">RTCM Out' + (rtcmAt ? ' &nbsp;<span>&rarr;</span> ' + rtcmAt : '') + '</div>' + rtcm + '</div>');
            return blocks.join('');
        }

        const lastIn = {};
        let lastPoll = 0;
        let lastOut = null;
        function pollStatus() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => {
                    const ser = data.serial || {};
                    const imu = data.imu || {};
                    const now = Date.now();
                    const secs = lastPoll ? (now - lastPoll) / 1000 : 1;
                    lastPoll = now;
                    const out = { nmea: ser.nmeaOutSent || 0, rtcm: ser.rtcmOutBytes || 0 };
                    const outPerSec = lastOut ? { nmea: Math.max(0, Math.round((out.nmea - lastOut.nmea) / secs)),
                                                  rtcm: Math.max(0, Math.round((out.rtcm - lastOut.rtcm) / secs)) } : { nmea: 0, rtcm: 0 };
                    lastOut = out;
                    document.getElementById('portOut').innerHTML = outView(ser, ser.ports || [], outPerSec);
                    const blocks = [];
                    const names = [];
                    (ser.ports || []).forEach(p => {
                        const perSec = lastIn[p.name] === undefined ? 0 : Math.max(0, Math.round((p.bytesIn - lastIn[p.name]) / secs));
                        lastIn[p.name] = p.bytesIn;
                        if (p.place === 'usb' && (p.kind === 'unknown' || p.nmeaOut || p.rtcmOut)) names.push(p.name);
                        if (p.open && (p.nmeaOut || p.rtcmOut)) return;  // an output: Going Out Now shows it
                        const head = p.place === 'usb' ? 'USB <span>' + esc(p.name) + '</span>' : esc(p.name);
                        let what, body;
                        if (!p.open) {
                            what = '';
                            body = '<span class="quiet">Not available.</span>';
                        } else if (p.kind === 'unknown') {
                            what = p.autoBaud ? 'listening, trying ' + p.baud : 'listening at ' + p.baud;
                            body = linesView(p, perSec);
                        } else {
                            what = '<span class="found">' + KIND[p.kind] + '</span> at ' + p.baud + (p.inUse ? ', in use' : '');
                            if (p.kind === 'gnss') body = p.inUse ? gnssView(data) : '<span class="quiet">Not in use: the onboard receiver comes first.</span>';
                            else if (p.kind === 'imu') body = (p.inUse && imuView(imu)) || linesView(p, perSec);
                            else body = linesView(p, perSec);
                        }
                        blocks.push('<div class="port-live"><div class="head">' + head + (what ? ' &nbsp;<span>' + what + '</span>' : '') +
                            '</div>' + body + '</div>');
                    });
                    document.getElementById('portLive').innerHTML = blocks.length ? blocks.join('') : '<span class="quiet">No serial ports.</span>';
                    if (names.join('|') !== usbNames.join('|')) {  // USB devices plugged in or out
                        usbNames = names;
                        fillOut('nmeaOut', saved.nmeaOut, saved.nmeaOutUsb);
                        fillOut('rtcmOut', saved.rtcmOut, saved.rtcmOutUsb);
                    }
                })
                .catch(() => {})
                .finally(() => setTimeout(pollStatus, 1000));
        }

        window.onload = function() {
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
                <div class="help-text" style="margin: 0 0 10px;">
                    Plug a GNSS receiver, RTK radio or IMU into either port or USB: the HAT works out what it is. Only outputs are set here.
                </div>
                <div class="form-group">
                    <label for="nmeaOut">NMEA Out:</label>
                    <select id="nmeaOut" onchange="changed()"></select>
                </div>
                <div id="nmeaGroup" class="form-group" style="margin-top: 10px;">
                    <div class="nmea-boxes">
                        <label><input type="checkbox" id="nmea1"> GGA</label>
                        <label><input type="checkbox" id="nmea2"> VTG</label>
                        <label><input type="checkbox" id="nmea4"> RMC</label>
                        <label><input type="checkbox" id="nmea8"> ZDA</label>
                        <label><input type="checkbox" id="nmea16"> HDT</label>
                    </div>
                    <select id="nmeaRate" style="margin-top: 10px;">
                        <option value="0">As the receiver sends them</option>
                        <option value="10">10 Hz</option>
                        <option value="5">5 Hz</option>
                        <option value="2">2 Hz</option>
                        <option value="1">1 Hz</option>
                    </select>
                    <label class="check"><input type="checkbox" id="nmeaCorrected"> Corrected position from AgOpenWeb</label>
                    <div class="help-text">The receiver's sentences, for a rate controller, sprayer or yield monitor. With the corrected position, the position and heading are AgOpenWeb's, with antenna offset and roll applied (tick "Send corrected position" in AgOpenWeb's Network IO); without it for a second, the receiver's own.</div>
                </div>
                <div class="form-group" style="margin-top: 15px;">
                    <label for="rtcmOut">RTCM Out:</label>
                    <select id="rtcmOut"></select>
                    <div class="help-text">The corrections the receiver gets, passed on, e.g. to a radio for a second receiver. Both outputs can share a port.</div>
                </div>
                <div class="two" style="margin-top: 15px;">
                    <div class="form-group">
                        <label for="rate0">Port 1 Rate:</label>
                        <select id="rate0"></select>
                    </div>
                    <div class="form-group">
                        <label for="rate1">Port 2 Rate:</label>
                        <select id="rate1"></select>
                    </div>
                </div>
                <div class="help-text">Auto tries rates until what comes in makes sense. Set one for a device the HAT cannot recognise, or for an output (on Auto an output runs at 115200).</div>
            </div>
        </form>

        <div class="card">
            <div class="form-group" style="margin-top: 0;">
                <label>Going Out Now:</label>
                <div id="portOut">-</div>
            </div>
        </div>

        <div class="card">
            <div class="form-group" style="margin-top: 0;">
                <label>Coming In Now:</label>
                <div id="portLive">-</div>
                <div class="help-text" style="margin-top: 5px;">
                    Mount an IMU with its arrow pointing to the front of the tractor. To check it: right side down (or tilt the IMU to the right) and roll goes positive; nose up and pitch goes positive; turn right and the heading goes up. If the IMU is turned 90 degrees, set "Use Y axis" in AgOpenWeb's steer settings.
                </div>
            </div>
        </div>
    </div>
</body>
</html>
)rawliteral";

#endif // SERIAL_PAGE_H
