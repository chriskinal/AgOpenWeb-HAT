// AgOpenWeb HAT. GPL-3.0-or-later.
//
// AOW HAT: a new page, not from the firmware (the Teensy had no ISOBUS). It uses the firmware
// pages' stylesheet and layout. Settings and live state come from /api/isobus.

#ifndef ISOBUS_PAGE_H
#define ISOBUS_PAGE_H

const char ISOBUS_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <title>ISOBUS - AgOpenWeb HAT</title>
    <link rel="stylesheet" href="/touch.css">
    <style>
        .toggle-container { display: flex; align-items: center; justify-content: space-between; padding: 12px 0; border-bottom: 1px solid #ecf0f1; }
        .toggle-label { font-size: 18px; font-weight: 500; }
        .toggle-info { flex: 1; margin-right: 15px; }
        .help-text { font-size: 14px; color: #7f8c8d; margin-top: 5px; line-height: 1.3; }
        select { width: 100%; padding: 15px; font-size: 18px; border: 2px solid #bdc3c7; border-radius: 8px; background: white; }
        .nav-buttons { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-bottom: 20px; }
        .row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #ecf0f1; font-size: 17px; }
        .row:last-child { border-bottom: none; }
        .row span:last-child { font-weight: 600; text-align: right; }
        .good { color: #27ae60; } .warn { color: #e67e22; } .bad { color: #e74c3c; } .idle { color: #7f8c8d; }
        .sections { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 10px; }
        .section { width: 30px; height: 38px; border-radius: 4px; font-size: 12px; line-height: 38px; text-align: center; color: white; background: #bdc3c7; }
        .section.on { background: #27ae60; }
        .section.wanted { background: #e67e22; }
        .section.unwanted { background: #e74c3c; }
        .legend { font-size: 13px; color: #7f8c8d; margin-top: 8px; }
    </style>
    <script>
        const STATE = {
            'not built': ['This build has no ISOBUS stack', 'bad'],
            'disabled': ['Off', 'idle'],
            'starting': ['Claiming an address...', 'warn'],
            'claimed': ['On the bus', 'good'],
            'failed': ['Failed: see the log', 'bad']
        };
        const TECU = {
            'off': ['Off', 'idle'],
            'waiting': ['Checking the bus for a tractor ECU...', 'warn'],
            'active': ['Standing in: sending speed', 'good'],
            'tractor has one': ["The tractor's own is used", 'good'],
            'failed': ['Failed: address 240 not free', 'bad']
        };

        function set(id, text, cls) {
            const e = document.getElementById(id);
            e.textContent = text;
            e.className = cls || '';
        }

        function showStatus(s) {
            const st = STATE[s.state] || [s.state, ''];
            set('state', st[0], st[1]);
            set('bus', s.state === 'disabled' ? '-' : 'CAN' + s.channel + ' (' + s.interface + ')');
            set('address', s.address !== undefined ? s.address : '-');
            if (s.state === 'claimed')
                set('busActive', s.busActive ? 'Yes' : 'No: nothing else is answering', s.busActive ? 'good' : 'warn');
            else set('busActive', '-');
            const te = TECU[s.tractorEcu.state] || [s.tractorEcu.state, ''];
            set('tecu', s.state === 'disabled' ? '-' : te[0], te[1]);
            set('speed', s.tractorEcu.state === 'active' ? s.tractorEcu.speed.toFixed(1) + ' km/h' : '-');

            const tc = s.taskController;
            const have = tc.sections > 0;
            set('implement', have ? (tc.implement || 'unnamed') : (s.state === 'claimed' ? 'None connected' : '-'), have ? 'good' : 'idle');
            set('sectionCount', have ? tc.sections : '-');
            set('width', have && tc.width > 0 ? tc.width.toFixed(2) + ' m' : '-');
            set('sectionControl', have ? (tc.sectionControl ? 'On' : 'Off: no app') : '-', tc.sectionControl ? 'good' : 'idle');
            const box = document.getElementById('sections');
            if (box.childElementCount !== tc.sections) {
                box.innerHTML = '';
                for (let i = 0; i < tc.sections; i++) {
                    const d = document.createElement('div');
                    d.textContent = i + 1;
                    box.appendChild(d);
                }
            }
            for (let i = 0; i < tc.sections; i++) {
                const want = tc.setpoint[i] === '1', is = tc.actual[i] === '1';
                box.children[i].className = 'section' + (want && is ? ' on' : want ? ' wanted' : is ? ' unwanted' : '');
            }
            document.getElementById('legend').style.display = have ? 'block' : 'none';
        }

        function poll() {
            fetch('/api/isobus').then(r => r.json()).then(d => showStatus(d.status)).catch(() => set('state', 'No answer from the daemon', 'bad'));
        }

        function loadSettings() {
            fetch('/api/isobus').then(r => r.json()).then(d => {
                document.getElementById('enabled').checked = d.enabled;
                document.getElementById('channel').value = d.channel;
                document.getElementById('tractorEcu').checked = d.tractorEcu;
                showStatus(d.status);
            });
        }

        function saveSettings() {
            const body = {
                enabled: document.getElementById('enabled').checked,
                channel: parseInt(document.getElementById('channel').value),
                tractorEcu: document.getElementById('tractorEcu').checked
            };
            const msg = document.getElementById('status');
            msg.innerHTML = '<div class="status">Saving...</div>';
            fetch('/api/isobus', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
                .then(r => r.json())
                .then(d => {
                    if (d.status !== 'saved') throw new Error(d.message || 'not saved');
                    if (!d.restartRequired) {
                        msg.innerHTML = '<div class="status success">Saved. Nothing changed.</div>';
                        setTimeout(() => { msg.innerHTML = ''; }, 4000);
                        return;
                    }
                    if (confirm('Saved. ISOBUS settings take effect when the HAT software restarts. Steering stops for a few seconds. Restart now?')) {
                        msg.innerHTML = '<div class="status">Restarting...</div>';
                        fetch('/api/restart', { method: 'POST' }).finally(() => setTimeout(() => location.reload(), 4000));
                    } else {
                        msg.innerHTML = '<div class="status">Saved. Takes effect at the next restart.</div>';
                    }
                })
                .catch(e => { msg.innerHTML = '<div class="status error">Error: ' + e.message + '</div>'; });
        }

        window.onload = function() {
            loadSettings();
            setInterval(poll, 1000);
        };
    </script>
</head>
<body>
    <div class="container">
        <h1>ISOBUS</h1>

        <div class="nav-buttons">
            <button type="button" class="touch-button" style="background: #7f8c8d;" onclick="window.location.href='/'">Back to Home</button>
            <button type="submit" class="touch-button" form="settingsForm">Apply Changes</button>
        </div>

        <div id="status"></div>

        <form id="settingsForm" onsubmit="saveSettings(); return false;">
            <div class="card">
                <div class="toggle-container">
                    <div class="toggle-info">
                        <label for="enabled" class="toggle-label">ISOBUS Section Control</label>
                        <div class="help-text">
                            Act as the task controller for an ISOBUS implement: the sections AgOpenWeb switches are sent to the implement.
                        </div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="enabled">
                        <span class="toggle-slider"></span>
                    </label>
                </div>

                <div class="form-group" style="margin-top: 15px;">
                    <label for="channel">CAN Channel:</label>
                    <select id="channel">
                        <option value="1">CAN1</option>
                        <option value="2">CAN2 (Default)</option>
                        <option value="3">CAN3</option>
                    </select>
                    <div class="help-text">
                        The channel wired to the implement bus. It runs at 250 kbit/s whatever the CAN Steering page says. On a tractor's ISOBUS leave this channel's 120 ohm jumper open: the bus is terminated already.
                    </div>
                </div>

                <div class="toggle-container" style="margin-top: 15px; border-bottom: none;">
                    <div class="toggle-info">
                        <label for="tractorEcu" class="toggle-label">Send Speed When the Tractor Does Not</label>
                        <div class="help-text">
                            If no tractor ECU is on the bus, stand in for one and send ground speed, which most implements need. It is never used beside a tractor's own.
                        </div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="tractorEcu">
                        <span class="toggle-slider"></span>
                    </label>
                </div>
            </div>
        </form>

        <h2>Bus</h2>
        <div class="card">
            <div class="row"><span>State</span><span id="state">-</span></div>
            <div class="row"><span>Channel</span><span id="bus">-</span></div>
            <div class="row"><span>Our address</span><span id="address">-</span></div>
            <div class="row"><span>Other devices answering</span><span id="busActive">-</span></div>
            <div class="row"><span>Tractor ECU</span><span id="tecu">-</span></div>
            <div class="row"><span>Speed sent</span><span id="speed">-</span></div>
        </div>

        <h2>Implement</h2>
        <div class="card">
            <div class="row"><span>Implement</span><span id="implement">-</span></div>
            <div class="row"><span>Sections</span><span id="sectionCount">-</span></div>
            <div class="row"><span>Working width</span><span id="width">-</span></div>
            <div class="row"><span>Section control</span><span id="sectionControl">-</span></div>
            <div class="sections" id="sections"></div>
            <div class="legend" id="legend" style="display: none;">
                Green: on. Orange: asked for, not yet reported on. Red: reported on, not asked for. Grey: off.
            </div>
        </div>
    </div>
</body>
</html>
)rawliteral";

#endif // ISOBUS_PAGE_H
