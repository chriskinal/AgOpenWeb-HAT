// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// AOW HAT: the pages' own alert and confirm, served as /dialog.js. AgOpenWeb shows these pages in
// a web card, an iframe sandboxed without allow-modals, where the browser's alert() does nothing
// and confirm() returns false at once (AgOpenWeb Docs/ADD_ON_CARDS.md, "What the frame allows"):
// every Restart and Reboot button would do nothing. These work in a card and in a browser tab
// alike. Both return a Promise: await uiConfirm('...') is true for OK. Esc or a tap outside is
// Cancel. Text only (newlines kept), never HTML. Fits the smallest card, 420 x 350 CSS px.

#ifndef AOW_DIALOG_H
#define AOW_DIALOG_H

const char AOW_DIALOG_JS[] = R"rawliteral(
(function () {
    function show(message, ok, cancel) {
        return new Promise(function (resolve) {
            const back = document.createElement('div');
            back.style.cssText = 'position:fixed;inset:0;z-index:10000;background:rgba(0,0,0,.55);' +
                'display:flex;align-items:center;justify-content:center;padding:12px;';
            const box = document.createElement('div');
            box.setAttribute('role', 'alertdialog');
            box.setAttribute('aria-modal', 'true');
            box.style.cssText = 'background:#fff;color:#2c3e50;border-radius:12px;padding:20px;width:100%;' +
                'max-width:440px;max-height:100%;overflow:auto;box-shadow:0 8px 30px rgba(0,0,0,.4);' +
                'font-family:inherit;font-size:18px;line-height:1.4;';
            const text = document.createElement('div');
            text.style.cssText = 'white-space:pre-line;overflow-wrap:anywhere;margin-bottom:18px;';
            text.textContent = message;
            const row = document.createElement('div');
            row.style.cssText = 'display:flex;gap:12px;justify-content:flex-end;flex-wrap:wrap;';
            function button(label, primary, value) {
                const b = document.createElement('button');
                b.type = 'button';
                b.textContent = label;
                b.style.cssText = 'min-height:48px;min-width:110px;padding:10px 20px;border:none;border-radius:8px;' +
                    'font-size:18px;font-weight:600;cursor:pointer;color:#fff;background:' + (primary ? '#3498db' : '#7f8c8d') + ';';
                b.onclick = function () { done(value); };
                row.appendChild(b);
                return b;
            }
            function done(value) {
                document.removeEventListener('keydown', key, true);
                back.remove();
                resolve(value);
            }
            function key(e) {
                if (e.key === 'Escape') { e.preventDefault(); done(cancel ? false : undefined); }
                else if (e.key === 'Enter') { e.preventDefault(); done(cancel ? true : undefined); }
            }
            if (cancel) button(cancel, false, false);
            const okButton = button(ok, true, cancel ? true : undefined);
            back.onclick = function (e) { if (e.target === back) done(cancel ? false : undefined); };
            box.appendChild(text);
            box.appendChild(row);
            back.appendChild(box);
            document.body.appendChild(back);
            document.addEventListener('keydown', key, true);
            okButton.focus();
        });
    }
    window.uiAlert = function (message) { return show(String(message), 'OK', null); };
    window.uiConfirm = function (message, ok) { return show(String(message), ok || 'OK', 'Cancel'); };
})();
)rawliteral";

#endif  // AOW_DIALOG_H
