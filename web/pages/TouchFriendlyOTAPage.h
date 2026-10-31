// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware_Teensy_AiO-New-Dawn is copyright 2025 by the AOG Group
// Firmware_Teensy_AiO-New-Dawn is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
// Firmware_Teensy_AiO-New-Dawn is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// You should have received a copy of the GNU General Public License along with Firmware_Teensy_AiO-New-Dawn. If not, see <https://www.gnu.org/licenses/>.
// Like most Arduino code, portions of this are based on other open source Arduino code with a compatiable license.

// AOW HAT: copied from Firmware_Teensy_AiO_26 v26.5.5 web_pages/TouchFriendlyOTAPage.h.
// HAT changes: the file is a signed update package (.aowpkg, deploy/package.sh), sent as bytes,
// not a Teensy .hex; after the upload the page follows /api/ota/status while a separate service
// verifies, installs and checks the new version (and restores the old one if it fails); a
// restart button when the boot files changed; wording. A package can carry AgOpenWeb too (the HAT
// software and the app as a matched set): the page shows the AgOpenWeb installed
// (/api/ota/versions), and says so when it is not the one the installed package carried; the wait
// for a result is 5 minutes, not 3. Layout and styles are the firmware's.

// TouchFriendlyOTAPage.h
// Touch-optimized system update page

#ifndef TOUCH_FRIENDLY_OTA_PAGE_H
#define TOUCH_FRIENDLY_OTA_PAGE_H

// #include <Arduino.h>  (AOW HAT: not needed; PROGMEM is defined by web/api.cpp)

const char TOUCH_FRIENDLY_OTA_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <title>System Update - AgOpenWeb HAT</title>
    <link rel="stylesheet" href="/touch.css">
    <style>
        /* Additional styles specific to OTA update */
        .board-info {
            background: #e3f2fd;
            padding: 15px;
            border-radius: 8px;
            text-align: center;
            margin-bottom: 20px;
        }
        
        .board-info div {
            font-size: 18px;
            font-weight: 500;
            color: #1976d2;
            margin: 5px 0;
        }
        
        .warning-box {
            background: #ffebee;
            border: 2px solid #ef5350;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            text-align: center;
            color: #c62828;
        }
        
        .warning-box strong {
            color: #c62828;
            font-size: 18px;
            display: block;
            margin-bottom: 5px;
        }
        
        
        .file-info {
            margin-top: 15px;
            padding: 15px;
            background: #e8f5e9;
            border-radius: 8px;
            font-family: monospace;
            word-break: break-all;
            display: none;
        }
        
        #feedback {
            font-family: monospace;
            background: #f5f5f5;
            padding: 15px;
            border-radius: 8px;
            margin: 20px 0;
            min-height: 120px;
            white-space: pre-wrap;
            font-size: 14px;
            border: 2px solid #bdc3c7;
        }
        
        .progress-track {
            height: 28px;
            background: #e0e0e0;
            border-radius: 14px;
            overflow: hidden;
            margin-top: 20px;
            display: none;
        }
        
        .progress-fill {
            height: 100%;
            width: 0%;
            background: #4caf50;
            transition: width 0.2s linear;
        }
        
        .progress-fill.busy {
            background: repeating-linear-gradient(45deg, #4caf50 0 14px, #66bb6a 14px 28px);
            background-size: 40px 28px;
            animation: progress-stripes 1s linear infinite;
        }
        
        .progress-fill.failed {
            background: #ef5350;
        }
        
        @keyframes progress-stripes {
            from { background-position: 0 0; }
            to { background-position: 40px 0; }
        }
        
        .progress-label {
            text-align: center;
            font-size: 18px;
            font-weight: 600;
            margin-top: 8px;
            display: none;
        }
        
        .upload-button {
            background: #4caf50;
        }
        
        .button-grid .upload-button {
            margin-top: 0;
        }
        
        .upload-button:active {
            background: #388e3c;
        }
        
        .upload-button:disabled {
            background: #95a5a6;
            opacity: 0.6;
        }
        
        .nav-buttons {
            display: grid;
            grid-template-columns: 1fr;
            gap: 15px;
            margin-bottom: 20px;
        }
        
        .button-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-bottom: 20px;
        }
        
        .button-grid .touch-button,
        .button-grid label.touch-button {
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            height: 60px;
            min-height: 60px;
            box-sizing: border-box;
            padding: 20px 25px;
            margin: 0;
            line-height: 1;
        }
        
        @media (max-width: 600px) {
            #feedback {
                font-size: 12px;
            }
        }
    </style>
    <script>
        let startVersion = '';
        
        function loadVersion() {
            fetch('/api/ota/versions')
            .then(response => response.json())
            .then(data => {
                if (data.version) {
                    if (!startVersion) startVersion = data.version;
                    document.getElementById('currentVersion').textContent = data.version;
                } else {
                    document.getElementById('currentVersion').textContent = 'Unknown';
                }
                let app = data.agopenweb || 'Not installed';
                if (data.agopenwebPackaged && data.agopenweb && data.agopenweb !== data.agopenwebPackaged)
                    app += ' (not the one in the installed package, ' + data.agopenwebPackaged + ')';
                document.getElementById('appVersion').textContent = app;
            })
            .catch(error => {
                console.error('Error loading version:', error);
                document.getElementById('currentVersion').textContent = 'Error';
            });
        }
        
        window.onload = loadVersion;
        
        function displayFileName() {
            const fileInput = document.getElementById('file');
            const fileInfo = document.getElementById('fileInfo');
            const file = fileInput.files[0];
            
            if (file) {
                fileInfo.innerHTML = '<strong>Selected file:</strong> ' + file.name;
                fileInfo.style.display = 'block';
            } else {
                fileInfo.style.display = 'none';
            }
        }
        
        function setProgress(percent, label, state) {
            const track = document.getElementById('progressTrack');
            const fill = document.getElementById('progressFill');
            const text = document.getElementById('progressLabel');
            track.style.display = 'block';
            text.style.display = 'block';
            fill.style.width = percent + '%';
            fill.className = 'progress-fill' + (state ? ' ' + state : '');
            text.textContent = label;
        }
        
        function log(line) {
            document.getElementById('feedback').textContent += line + '\n';
        }
        
        function formatKB(bytes) {
            return Math.round(bytes / 1024).toLocaleString() + ' KB';
        }
        
        const stepText = {
            received: 'Package received',
            verifying: 'Checking the signature',
            installing: 'Installing',
            checking: 'Checking the new version runs'
        };
        
        // Follow the updater until it finishes. The daemon restarts part-way through, so a
        // few failed requests are expected.
        function waitForUpdate() {
            const started = Date.now();
            let lastMessage = '';
            
            function finish() {
                document.getElementById('uploadBtn').disabled = false;
                loadVersion();
            }
            
            function poll() {
                const elapsed = Math.round((Date.now() - started) / 1000);
                if (elapsed > 300) {
                    setProgress(100, 'No result after 5 minutes', 'failed');
                    log('The unit did not report a result. Reload this page to see its state.');
                    finish();
                    return;
                }
                
                const controller = new AbortController();
                const timer = setTimeout(() => controller.abort(), 2000);
                fetch('/api/ota/status?t=' + Date.now(), { signal: controller.signal, cache: 'no-store' })
                .then(response => response.json())
                .then(data => {
                    clearTimeout(timer);
                    if (data.message && data.message !== lastMessage) {
                        lastMessage = data.message;
                        log(data.message);
                    }
                    if (data.state === 'done') {
                        setProgress(100, 'Update complete: version ' + data.version, '');
                        if (data.rebootNeeded) document.getElementById('rebootBtn').style.display = 'flex';
                        finish();
                    } else if (data.state === 'rolled_back') {
                        setProgress(100, 'Update failed: the previous version was put back', 'failed');
                        finish();
                    } else if (data.state === 'failed') {
                        setProgress(100, 'Update failed', 'failed');
                        finish();
                    } else {
                        setProgress(100, 'Step 3 of 3: ' + (stepText[data.state] || 'Working') + '... ' + elapsed + ' s', 'busy');
                        setTimeout(poll, 1000);
                    }
                })
                .catch(() => {
                    clearTimeout(timer);
                    setProgress(100, 'Step 3 of 3: Restarting... ' + elapsed + ' s', 'busy');
                    setTimeout(poll, 1000);
                });
            }
            
            setTimeout(poll, 500);
        }
        
        function sendPackage(name, content) {
            const uploadBtn = document.getElementById('uploadBtn');
            const total = content.byteLength;
            let sentAll = false;
            let lastPercent = 0;
            
            log('File loaded: ' + name + ' (' + formatKB(total) + ')');
            setProgress(0, 'Step 1 of 3: Uploading... 0%', '');
            
            const xhr = new XMLHttpRequest();
            
            xhr.upload.addEventListener('progress', function(e) {
                if (!e.lengthComputable || sentAll) return;
                lastPercent = Math.floor((e.loaded / e.total) * 100);
                setProgress(lastPercent, 'Step 1 of 3: Uploading... ' + lastPercent + '% (' +
                            formatKB(e.loaded) + ' of ' + formatKB(e.total) + ')', '');
            });
            
            xhr.upload.addEventListener('load', function() {
                sentAll = true;
                log('Upload sent: ' + formatKB(total));
                setProgress(100, 'Step 2 of 3: Finishing upload...', 'busy');
            });
            
            xhr.addEventListener('load', function() {
                if (xhr.status === 200) {
                    log('Package accepted by the unit.');
                    waitForUpdate();
                } else {
                    setProgress(lastPercent, 'Update not started', 'failed');
                    log('Update not started: ' + xhr.responseText);
                    uploadBtn.disabled = false;
                }
            });
            
            xhr.addEventListener('error', function() {
                setProgress(lastPercent, 'Upload interrupted at ' + lastPercent + '%', 'failed');
                log('Connection lost during upload. Check the connection and try again.');
                uploadBtn.disabled = false;
            });
            
            xhr.open('POST', '/api/ota/upload');
            xhr.setRequestHeader('Content-Type', 'application/octet-stream');
            xhr.send(content);
        }
        
        function uploadFile() {
            const fileInput = document.getElementById('file');
            const file = fileInput.files[0];
            
            if (!file) {
                alert('Please select an update file');
                return false;
            }
            
            if (!file.name.endsWith('.aowpkg')) {
                alert('Please select an .aowpkg update file');
                return false;
            }
        
            document.getElementById('feedback').textContent = '';
            document.getElementById('rebootBtn').style.display = 'none';
            document.getElementById('uploadBtn').disabled = true;
            
            const reader = new FileReader();
            reader.onload = function(e) {
                sendPackage(file.name, e.target.result);
            };
            reader.readAsArrayBuffer(file);
            return false;
        }
        
        function rebootUnit() {
            if (!confirm('Restart the unit now? It will be away for about a minute.')) return;
            fetch('/api/ota/reboot', { method: 'POST' })
            .then(response => response.text().then(text => {
                log(response.ok ? 'Restarting the unit. Reload this page in a minute.' : 'Not restarted: ' + text);
            }))
            .catch(() => log('Restarting the unit. Reload this page in a minute.'));
        }
    </script>
</head>
<body>
    <div class="container">
        <h1>System Update</h1>
        
        <div class="nav-buttons">
            <button type="button" class="touch-button" style="background: #7f8c8d;" 
                    onclick="window.location.href='/'">
                Back to Home
            </button>
        </div>
        
        <div class="card">
            <div class="button-grid">
                <label for="file" class="touch-button">
                    Choose Update File
                </label>
                <button type="button" id="uploadBtn" class="touch-button upload-button" onclick="uploadFile()">
                    Install Update
                </button>
            </div>
            
            <input type="file" id="file" name="package" accept=".aowpkg" onchange="displayFileName()" style="display: none;">
            
            <div id="fileInfo" class="file-info"></div>
            
            <div id="progressTrack" class="progress-track"><div id="progressFill" class="progress-fill"></div></div>
            <div id="progressLabel" class="progress-label"></div>
            
            <div id="feedback"></div>
            
            <button type="button" id="rebootBtn" class="touch-button" style="display: none; background: #e67e22; width: 100%;" onclick="rebootUnit()">
                Restart Unit to Finish
            </button>
        </div>
        
        <div class="board-info">
            <div>Board Type: CM4 + AgOpenWeb HAT</div>
            <div>Current Version: <span id="currentVersion">Loading...</span></div>
            <div>AgOpenWeb: <span id="appVersion">Loading...</span></div>
        </div>
        
        <div class="warning-box">
            <strong>Before you update</strong>
            Disengage steering first: the HAT software restarts during the update, and AgOpenWeb
            too when the package has a new one. Only signed AgOpenWeb HAT update files (.aowpkg)
            are accepted. If the new version does not start, the previous one is put back.
        </div>
        
    </div>
</body>
</html>
)rawliteral";

#endif // TOUCH_FRIENDLY_OTA_PAGE_H