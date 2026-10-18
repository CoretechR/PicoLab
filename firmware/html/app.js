// Network setup and live telemetry state
// Connects via SSE to stream real-time data from the Pico
let host = window.location.hostname || "192.168.7.1";
// Wrap IPv6 addresses in brackets for proper URL formatting
if (host.includes(':') && !host.startsWith('[')) {
    host = `[${host}]`;
}

const sseUrl = `http://${host}:8080/`;
let evtSource = null;
// Live telemetry gets updated by the SSE event listeners.
// Format: analog voltages, digital pin states, and power supply info.
const LiveTelemetry = { A: {}, D: {}, PSU: { i5: 0, i3: 0, ip: 0, vp: 0 } };
const TELEMETRY_HEX_FRAME_LEN = 36;
const ANALOG_CHANNEL_COUNT = 4;
const DIGITAL_CHANNEL_COUNT = 8;
const ADC_MAX = 4095;
let ADC_REF_V = 3.3;
let ADC_CHANNELS = 4;
let HAS_ADJ_RAIL = false;
let NUM_SAMPLES = 80000;

// Registry of app window titles
const APP_TITLES = Object.freeze({
    power: 'Power Supply',
    gpio: 'GPIO Control',
    terminal: 'UART Terminal',
    logic: 'Logic Analyzer',
    scope: 'Oscilloscope',
    logger: 'Data Logger',
    settings: 'System Settings',
    about: 'About',
    i2c: 'I2C Master',
    spi: 'SPI Master'
});

// Default dimensions and constraints for UI windows
const DEFAULT_WINDOW_LAYOUT = Object.freeze({
    width: '400px',
    height: '300px',
    minwidth: 250,
    minheight: 200,
    class: ['no-full']
});

const APP_WINDOW_LAYOUTS = Object.freeze({
    gpio: { width: '640px', height: '560px', minwidth: 550, minheight: 400, class: ['no-full'] },
    terminal: { width: '540px', height: '400px', minwidth: 540, minheight: 250, class: ['no-full'] },
    about: { width: '600px', height: '670px', minwidth: 600, minheight: 670, class: ['no-full', 'no-resize', 'no-max'] },
    logger: { width: '800px', height: '600px', minwidth: 600, minheight: 560, class: ['no-full'] },
    settings: { width: '400px', height: '370px', minwidth: 640, minheight: 360, class: ['no-full', 'no-resize'] },
    scope: { width: '768px', height: '550px', minwidth: 768, minheight: 500, class: ['no-full'] },
    logic: { width: '800px', height: '500px', minwidth: 760, minheight: 450, class: ['no-full'] },
    power: { width: '760px', height: '540px', minwidth: 680, minheight: 460, class: ['no-full'] },
    i2c: { width: '700px', height: '520px', minwidth: 640, minheight: 500, class: ['no-full'] },
    spi: { width: '700px', height: '520px', minwidth: 640, minheight: 500, class: ['no-full'] }
});

const UI_SETTINGS_STORAGE_KEY = 'picolab-ui-settings';
const DEFAULT_UI_SETTINGS = Object.freeze({
    wallpaperHue: 0,
    transparentWindows: true,
    csvDelimiter: ','
});

// Pull custom UI settings from local storage with fallbacks
function loadUiSettings() {
    let stored = {};
    try {
        stored = JSON.parse(localStorage.getItem(UI_SETTINGS_STORAGE_KEY) || '{}');
    } catch (err) {
        console.warn('Failed to read UI settings', err);
    }

    return {
        wallpaperHue: Number.isFinite(Number(stored.wallpaperHue)) ? Math.max(0, Math.min(360, Number(stored.wallpaperHue))) : DEFAULT_UI_SETTINGS.wallpaperHue,
        transparentWindows: typeof stored.transparentWindows === 'boolean' ? stored.transparentWindows : DEFAULT_UI_SETTINGS.transparentWindows,
        csvDelimiter: stored.csvDelimiter === ';' ? ';' : DEFAULT_UI_SETTINGS.csvDelimiter
    };
}

const UI_SETTINGS = loadUiSettings();

function saveUiSettings() {
    try {
        localStorage.setItem(UI_SETTINGS_STORAGE_KEY, JSON.stringify(UI_SETTINGS));
    } catch (err) {
        console.warn('Failed to save UI settings', err);
    }
}

// Injects CSS variables to theme the UI
function applyUiSettings() {
    document.documentElement.style.setProperty('--wallpaper-hue', `${UI_SETTINGS.wallpaperHue}deg`);
    document.body.classList.toggle('no-transparency', !UI_SETTINGS.transparentWindows);
}

function setUiSetting(key, value) {
    UI_SETTINGS[key] = value;
    saveUiSettings();
    applyUiSettings();
}

function getCsvDelimiter() {
    return UI_SETTINGS.csvDelimiter === ';' ? ';' : ',';
}

applyUiSettings();

// Connection status monitoring
// If we don't hear from the Pico in 1.5s, we show "Disconnected"
const statusDot = document.querySelector('#connection-status .status-dot');
const statusText = document.querySelector('#connection-status .status-text');
let connectionWatchdog = null;
let initialLoadComplete = false;
const WATCHDOG_TIMEOUT_MS = 1500;
const LOCAL_DEV_DISMISS_MS = 50;

function parseJsonSafe(text, contextLabel) {
    try {
        return JSON.parse(text);
    } catch (err) {
        console.error(`${contextLabel} parse error`, err);
        return null;
    }
}

function setCardDotState(card, isHigh) {
    const dot = card.querySelector('.status-dot');
    if (!dot) return;
    dot.style.backgroundColor = isHigh ? '#00ff00' : '#444444';
    dot.style.boxShadow = isHigh ? '0 0 5px #00ff00' : 'none';
}

function sendGet(path) {
    return fetch(path).catch(err => console.error('API Error:', err));
}

function dismissLoadingScreen() {
    if (!initialLoadComplete) {
        const loader = document.getElementById('loading-screen');
        if (loader) loader.classList.add('fade-out');
        initialLoadComplete = true;
    }
}

// Auto-dismiss loading screen for local dev environments
if (window.location.protocol === 'file:' || window.location.hostname === 'localhost') {
    setTimeout(dismissLoadingScreen, LOCAL_DEV_DISMISS_MS);
}

function updateConnectionStatus(isConnected) {
    statusDot.classList.toggle('green', isConnected);
    statusText.textContent = isConnected ? 'Connected' : 'Disconnected';
    statusText.style.color = isConnected ? '#ffffff' : '#a0a0ab';
}

// Resets the disconnect timer every time data arrives
function feedWatchdog() {
    updateConnectionStatus(true);
    clearTimeout(connectionWatchdog);
    connectionWatchdog = setTimeout(() => updateConnectionStatus(false), WATCHDOG_TIMEOUT_MS);
}

// API commands

function sendPinCommand(pinName, state) {
    const pinNum = parseInt(pinName.replace('D', '')) - 1;
    sendGet(`/toggle?pin=${pinNum}&state=${state}`);
}

function sendConfigCommand(pinName, param, value) {
    const pinNum = parseInt(pinName.replace('D', '')) - 1;
    sendGet(`/config?pin=${pinNum}&${param}=${value}`);
}

function getWindowLayout(appType) {
    return APP_WINDOW_LAYOUTS[appType] || DEFAULT_WINDOW_LAYOUT;
}

/* Parse hex telemetry frame (36 chars).
   Contains 4x analog, 4x PSU currents, 1x PSU voltage, 8x digital states. */
function parseMainTelemetryFrame(hexStr) {
    if (hexStr.length !== TELEMETRY_HEX_FRAME_LEN) return false;

    for (let i = 0; i < ANALOG_CHANNEL_COUNT; i++) {
        const rawA = parseInt(hexStr.substring(i * 4, (i * 4) + 4), 16);
        LiveTelemetry.A[`A${i+1}`] = i < ADC_CHANNELS ? (rawA / ADC_MAX) * ADC_REF_V : 0;
    }

    LiveTelemetry.PSU = {
        i5: parseInt(hexStr.substring(16, 20), 16),
        i3: parseInt(hexStr.substring(20, 24), 16),
        ip: parseInt(hexStr.substring(24, 28), 16),
        vp: parseInt(hexStr.substring(28, 32), 16)
    };

    const digitalState = parseInt(hexStr.substring(32, 36), 16);
    for (let i = 0; i < DIGITAL_CHANNEL_COUNT; i++) {
        const isHigh = (digitalState >> i) & 1;
        LiveTelemetry.D[`D${i+1}`] = isHigh;

        const card = document.getElementById(`card-D${i+1}`);
        if (!card) continue;

        const activeMode = card.querySelector('.mode-btn.active')?.getAttribute('data-mode');
        if (activeMode === 'pwm') continue;
        setCardDotState(card, isHigh);
    }

    return true;
}

// Initialize Server-Sent Events connection
// Listens for scope, logic, telemetry, UART, and system info events
function initTelemetry() {
    if (evtSource) return;
    evtSource = new EventSource(sseUrl);

    evtSource.onopen = function() {
        fetch('/system?cmd=about').catch(() => {});
    };
    
    evtSource.onmessage = function(event) {
        dismissLoadingScreen();
        feedWatchdog(); 
        
        if (!parseMainTelemetryFrame(event.data)) return;

        window.dispatchEvent(new CustomEvent('pico-telemetry-update', { detail: LiveTelemetry }));
    };

    evtSource.addEventListener("uart", function(event) {
        feedWatchdog();
        const data = parseJsonSafe(event.data, 'UART');
        if (!data) return;
        window.dispatchEvent(new CustomEvent('pico-uart-rx', { detail: data.text }));
    });

    // Buffer fragmented logic-analyzer chunks.
    // They come in pieces so we reassemble them before processing.
    let logicRxBuffer = "";

    evtSource.addEventListener("logic_start", function(event) {
        feedWatchdog();
        logicRxBuffer = "";
    });

    evtSource.addEventListener("logic_data", function(event) {
        feedWatchdog();
        logicRxBuffer += event.data; 
    });

    evtSource.addEventListener("logic_end", function(event) {
        feedWatchdog();
        const data = parseJsonSafe(event.data, 'Logic End');
        if (!data) return;

        window.dispatchEvent(new CustomEvent('pico-logic-rx', { 
            detail: { tb: data.tb, rate: data.rate, data: logicRxBuffer, generation: data.generation }
        }));

        logicRxBuffer = "";
    });

    evtSource.addEventListener("bus", function(event) {
        feedWatchdog();
        const data = parseJsonSafe(event.data, 'Bus Event');
        if (!data) return;
        window.dispatchEvent(new CustomEvent('pico-bus-rx', { detail: data }));
    });

    evtSource.onerror = function() {
        dismissLoadingScreen();
        updateConnectionStatus(false);
    };

    evtSource.addEventListener('sysinfo', (event) => {
        const data = parseJsonSafe(event.data, 'Sysinfo');
        if (!data) return;
        if (typeof data.adc_fs === 'number' && Number.isFinite(data.adc_fs)) ADC_REF_V = data.adc_fs;
        if (typeof data.adc_ch === 'number' && Number.isFinite(data.adc_ch)) ADC_CHANNELS = data.adc_ch;
        HAS_ADJ_RAIL = !!data.has_adj;

        // Hide and disable A4 in the oscilloscope and logger
        if (ADC_CHANNELS < 4) {
            document.querySelectorAll('.scope-ch-card[data-ch="A4"]').forEach(el => {
                el.classList.add('disabled');
                el.style.pointerEvents = 'none'; // Prevent any clicks inside the card
                
                const toggle = el.querySelector('.ch-toggle');
                if (toggle) {
                    toggle.classList.remove('active');
                    toggle.disabled = true;
                }
            });
            document.querySelectorAll('.trig-src option[value="A4"]').forEach(el => {
                el.disabled = true;
            });            
            document.querySelectorAll('.pin-select-btn[data-pin="A4"]').forEach(el => {
                el.classList.remove('active');
                el.disabled = true;
                el.style.opacity = '0.3';
                el.style.cursor = 'not-allowed';
            });
        }

        if (!HAS_ADJ_RAIL) {
            document.querySelectorAll('#template-logger .pin-select-btn[data-type="P"]').forEach(btn => {
                btn.classList.remove('active');
                btn.disabled = true;
                btn.style.opacity = '0.3';
                btn.style.cursor = 'not-allowed';
            });
        }

        // Deactivate the Power Supply app launcher if the hardware doesn't support it
        if (!HAS_ADJ_RAIL) {
            const psuLauncher = document.querySelector('.launcher[data-app="power"]');
            if (psuLauncher) {
                psuLauncher.style.opacity = '0.3';
                psuLauncher.style.cursor = 'not-allowed';
            }
        }

        document.querySelectorAll('.info-fw').forEach(el => el.textContent = data.fw);
        document.querySelectorAll('.info-chip').forEach(el => el.textContent = `${data.chip} (Rev ${data.rev})`);
        document.querySelectorAll('.info-serial').forEach(el => el.textContent = data.serial);
        document.querySelectorAll('.info-flash').forEach(el => el.textContent = `${data.flash} MB (${data.flash_used} MB used)`);
        document.querySelectorAll('.info-ram').forEach(el => el.textContent = `${data.ram} KB (${data.ram_used} KB used)`);
        document.querySelectorAll('.info-clock').forEach(el => el.textContent = `${data.clk} MHz`);
        document.querySelectorAll('.info-ip').forEach(el => el.textContent = data.ip);
        window.dispatchEvent(new CustomEvent('pico-sysinfo', { detail: data }));
    });
}

initTelemetry();

// Menu and window manager

const startBtn = document.getElementById('start-btn');
const startMenu = document.getElementById('start-menu');

startBtn.addEventListener('click', (e) => {
    e.stopPropagation(); 
    startMenu.classList.toggle('show');
});

// Close menus when clicking outside
document.addEventListener('click', (e) => {
    if (!startMenu.contains(e.target) && !startBtn.contains(e.target)) {
        startMenu.classList.remove('show');
    }
    document.querySelectorAll('.settings-menu.show').forEach(menu => {
        if (!menu.contains(e.target) && !e.target.classList.contains('menu-toggle')) {
            menu.classList.remove("show");
        }
    });
});

// Window manager and app lifecycle
// Handles opening/closing/minimizing windows and taskbar
const APP_INITIALIZERS = Object.freeze({
    gpio: initPinControl,
    terminal: initUartTerminal,
    about: initAboutWindow,
    logger: initLogger,
    settings: initSystemSettings,
    scope: initScope,
    logic: initLogicAnalyzer,
    power: initPowerSupply,
    i2c: initI2CMaster,
    spi: initSPIMaster
});

const launchers = document.querySelectorAll('.launcher');
const openWindows = Object.create(null);
const taskbarAppsContainer = document.getElementById('taskbar-apps');

// Brings an existing app window to the foreground
function focusAppWindow(appType, taskbarItem = null) {
    const win = openWindows[appType];
    if (!win) return false;

    const tbItem = taskbarItem || document.getElementById(`tb-${appType}`);
    win.show();
    win.restore();
    win.focus();
    if (tbItem) tbItem.classList.replace('minimized', 'active');
    return true;
}

// Creates a taskbar icon if one doesn't exist for the launched app
function getOrCreateTaskbarItem(appType) {
    let tbItem = document.getElementById(`tb-${appType}`);
    if (!tbItem) {
        tbItem = document.createElement('div');
        tbItem.id = `tb-${appType}`;
        tbItem.className = 'taskbar-item active';
        
        const launcherBtn = document.querySelector(`.launcher[data-app="${appType}"]`);
        const svgIcon = launcherBtn ? launcherBtn.querySelector('svg').outerHTML : '';
        tbItem.innerHTML = `${svgIcon} <span>${APP_TITLES[appType]}</span>`;
        taskbarAppsContainer.appendChild(tbItem);

        tbItem.addEventListener('click', () => {
            const win = openWindows[appType];
            if (!win) return;
            
            if (tbItem.classList.contains('minimized')) {
                win.show(); win.restore(); win.focus();
                tbItem.classList.remove('minimized');
                tbItem.classList.add('active');
            } else if (!tbItem.classList.contains('active')) {
                win.focus();
                tbItem.classList.add('active');
            } else {
                win.hide();
                tbItem.classList.remove('active');
                tbItem.classList.add('minimized');
            }
        });
    }
    return tbItem;
}

// Launch app handlers - tied to desktop icons
launchers.forEach(button => {
    button.addEventListener('click', () => {
        if (button.style.cursor === 'not-allowed') return;

        const appType = button.getAttribute('data-app');
        startMenu.classList.remove('show');

        if (focusAppWindow(appType)) return;

        const templateElement = document.getElementById('template-' + appType);
        if (!templateElement) return;

        const windowContent = templateElement.cloneNode(true);
        windowContent.style.display = "block";
        windowContent.removeAttribute('id');

        const initializer = APP_INITIALIZERS[appType];
        if (initializer) initializer(windowContent);

        const tbItem = getOrCreateTaskbarItem(appType);
        const layout = getWindowLayout(appType);

        openWindows[appType] = new WinBox({
            title: APP_TITLES[appType],
            mount: windowContent,
            width: layout.width,
            height: layout.height,
            minwidth: layout.minwidth,
            minheight: layout.minheight,
            x: "center", y: "center", top: 30,
            class: layout.class,
            background: "#333",
            
            onminimize: function() {
                this.hide();
                tbItem.classList.remove('active');
                tbItem.classList.add('minimized');
                return true; 
            },
            onfocus: function() { 
                tbItem.classList.remove('minimized');
                tbItem.classList.add('active'); 
            },
            onblur: function() { 
                tbItem.classList.remove('active'); 
            },
            onclose: function() {
                openWindows[appType] = null;
                tbItem.remove(); 
                return false; 
            }
        });
    });
});

// Keep windows visible when the viewport resizes
window.addEventListener('resize', () => {
    const ww = window.innerWidth, wh = window.innerHeight;
    for (const key in openWindows) {
        const win = openWindows[key];
        if (win && !win.max && !win.min) {
            let newX = win.x, newY = win.y;
            if (win.x + win.width > ww) newX = Math.max(0, ww - win.width);
            if (win.y + win.height > wh) newY = Math.max(30, wh - win.height);
            if (newX < 0) newX = 0;
            if (newY < 30) newY = 30;
            if (newX !== win.x || newY !== win.y) win.move(newX, newY);
        }
    }
});

// UART terminal app - read/write to the serial bridge
// Displays TX (sent) and RX (received) with formatting
function initUartTerminal(containerDOM) {
    const outputContent = containerDOM.querySelector(".output-content");
    const cmdInput = containerDOM.querySelector(".cmd-input");
    const clearBtn = containerDOM.querySelector(".clear-btn");
    const baudRateSelect = containerDOM.querySelector(".baud-rate");
    const lineEndingSelect = containerDOM.querySelector(".line-ending");
    let currentRxNode = null;

    function applyBaudRate() {
        const baud = baudRateSelect.value;
        fetch(`/uart?action=setup&baud=${baud}`)
            .catch(err => console.error("UART baud rate error:", err));
    }

    baudRateSelect.addEventListener("change", applyBaudRate);
    applyBaudRate();

    function appendToTerminal(text, type = 'rx') {
        if (type === 'tx') {
            const div = document.createElement('div');
            div.className = 'log-tx';
            div.textContent = `> ${text}`;
            outputContent.appendChild(div);
            currentRxNode = null; 
        } else {
            // Group received lines into the same div if they come in rapid succession.
            const parts = text.replace(/\r/g, '').split('\n');
            for (let i = 0; i < parts.length; i++) {
                if (!currentRxNode) {
                    currentRxNode = document.createElement('div');
                    currentRxNode.className = 'log-rx';
                    currentRxNode.style.whiteSpace = 'pre-wrap'; 
                    outputContent.appendChild(currentRxNode);
                }
                currentRxNode.textContent += parts[i];
                if (i < parts.length - 1) currentRxNode = null;
            }
        }
        outputContent.scrollTop = outputContent.scrollHeight;
    }

    window.addEventListener('pico-uart-rx', (e) => {
        if (document.body.contains(containerDOM)) {
            appendToTerminal(e.detail, 'rx');
        }
    });

    cmdInput.addEventListener("keypress", (e) => {
        if (e.key === "Enter" && cmdInput.value) {
            const rawText = cmdInput.value;
            appendToTerminal(rawText, 'tx');
            cmdInput.value = '';

            let payload = rawText;
            const ending = lineEndingSelect.value;
            if (ending === 'nl') payload += '\n';
            else if (ending === 'cr') payload += '\r';
            else if (ending === 'both') payload += '\r\n';

            fetch('/uart_tx', { method: 'POST', body: payload }).catch(err => console.error("UART Tx Error:", err));
        }
    });

    clearBtn.addEventListener("click", () => {
        outputContent.innerHTML = '';
        currentRxNode = null;
    });
}

// GPIO control - the main pin controller
// Build interactive cards for each pin with mode/state/PWM controls
let draggedCard = null;

function initPinControl(containerDOM) {
    const pinSelectBtns = containerDOM.querySelectorAll(".pin-select-btn");
    const pinGrid = containerDOM.querySelector(".pin-grid");
    const template = document.getElementById("pin-card-template");
    const activePins = new Map(); 

    pinSelectBtns.forEach(btn => {
        btn.addEventListener("click", () => {
            const pinName = btn.getAttribute("data-pin");
            if (activePins.has(pinName)) {
                activePins.get(pinName).remove();
                activePins.delete(pinName);
                btn.classList.remove("active");
            } else {
                const newCard = buildPinCard(pinName, template, containerDOM);
                pinGrid.appendChild(newCard);
                activePins.set(pinName, newCard);
                btn.classList.add("active");
            }
        });
    });

    // enable D1 and D2 by default
    const d1Btn = containerDOM.querySelector('.pin-select-btn[data-pin="D1"]');
    const d2Btn = containerDOM.querySelector('.pin-select-btn[data-pin="D2"]');
    if (d1Btn) d1Btn.click();
    if (d2Btn) d2Btn.click();
}

function buildPinCard(pinName, cardTemplate, containerDOM) {
    const clone = cardTemplate.content.cloneNode(true);
    const cardElement = clone.querySelector(".card");
    
    cardElement.id = `card-${pinName}`;
    cardElement.setAttribute("data-pin", pinName);
    cardElement.querySelector(".pin-name").textContent = pinName;
    cardElement.querySelector(".card-title").value = `Digital Pin ${pinName}`;

    // Settings menu
    cardElement.querySelector(".menu-toggle").addEventListener("click", (e) => {
        e.stopPropagation();
        cardElement.querySelector(".settings-menu").classList.toggle("show");
    });

    // Mode buttons - switch pin between input, output, PWM
    const modeButtons = cardElement.querySelectorAll(".mode-btn");
    const dynamicRows = cardElement.querySelectorAll(".dynamic-row");
    
    modeButtons.forEach(button => {
        button.addEventListener("click", (e) => {
            const targetMode = e.currentTarget.getAttribute("data-mode");
            modeButtons.forEach(btn => btn.classList.remove("active"));
            e.currentTarget.classList.add("active");

            // Show/hide mode-specific controls
            dynamicRows.forEach(row => row.classList.add("hidden"));
            const targetRow = cardElement.querySelector(`.row-${targetMode}`);
            if (targetRow) targetRow.classList.remove("hidden");

            sendConfigCommand(pinName, "mode", targetMode);

            if (targetMode === "out") {
                const lowBtn = Array.from(cardElement.querySelectorAll('.row-out .sub-btn')).find(b => b.textContent === 'LOW');
                if (lowBtn) lowBtn.click(); 
            } else if (targetMode === "in") {
                const hizBtn = Array.from(cardElement.querySelectorAll('.row-in .sub-btn')).find(b => b.textContent === 'High-Z');
                if (hizBtn) hizBtn.click();
            } else if (targetMode === "pwm") {
                const pwmSlider = cardElement.querySelector(".pwm-slider");
                const pwmValueText = cardElement.querySelector(".pwm-text");
                if (pwmSlider && pwmValueText) {
                    pwmSlider.value = 0;
                    pwmValueText.textContent = "0%";
                    sendConfigCommand(pinName, "pwm_duty", 0);
                }                
                const dot = cardElement.querySelector('.status-dot');
                if (dot) {
                    dot.style.backgroundColor = '#444444';
                    dot.style.boxShadow = 'none';
                }
            }
        });
    });

    // Default state
    const initialHizBtn = Array.from(cardElement.querySelectorAll('.row-in .sub-btn')).find(b => b.textContent === 'High-Z');
    if (initialHizBtn) initialHizBtn.click();

    // Drive and PWM settings
    const driveSelect = cardElement.querySelector(".settings-menu select");
    const freqInput = cardElement.querySelector(".settings-menu input[type='number']");
    if (driveSelect) driveSelect.addEventListener("change", (e) => sendConfigCommand(pinName, "drive", parseInt(e.target.value)));
    if (freqInput) freqInput.addEventListener("change", (e) => sendConfigCommand(pinName, "pwm_freq", e.target.value));

    // Sub-buttons (HIGH/LOW/PULL)
    cardElement.querySelectorAll(".sub-btn").forEach(button => {
        button.addEventListener("click", (e) => {
            const parentControl = e.currentTarget.closest(".segmented-control");
            parentControl.querySelectorAll(".sub-btn").forEach(btn => btn.classList.remove("active"));
            e.currentTarget.classList.add("active");

            const currentMode = cardElement.querySelector(".mode-btn.active").getAttribute("data-mode");
            const btnText = e.currentTarget.textContent.trim();
            
            if (currentMode === "out") {
                sendPinCommand(pinName, btnText === "HIGH" ? 1 : 0);
            } else if (currentMode === "in") {
                let pullState = "hi-z";
                if (btnText === "Pull-Up") pullState = "up";
                sendConfigCommand(pinName, "pull", pullState);
            }
        });
    });

    // PWM slider
    const pwmSlider = cardElement.querySelector(".pwm-slider");
    const pwmValueText = cardElement.querySelector(".pwm-text");
    let pwmThrottleTimer = null;
    let pendingPwmValue = null;

    pwmSlider.addEventListener("input", (e) => {
        const val = e.currentTarget.value;
        pwmValueText.textContent = `${val}%`;
        pendingPwmValue = val;

        const intensity = val / 100;
        const dot = cardElement.querySelector('.status-dot');
        const r = Math.round(68 * (1 - intensity));                  
        const g = Math.round(68 * (1 - intensity) + 255 * intensity);
        const b = Math.round(68 * (1 - intensity));                  
        
        dot.style.backgroundColor = `rgb(${r}, ${g}, ${b})`;
        dot.style.boxShadow = intensity > 0 ? `0 0 ${8 * intensity}px rgba(0, 255, 0, ${intensity})` : 'none';
        
        // Rate-limit PWM command burst to prevent flooding the server
        if (!pwmThrottleTimer) {
            sendConfigCommand(pinName, "pwm_duty", val);
            pendingPwmValue = null;
            pwmThrottleTimer = setTimeout(() => {
                pwmThrottleTimer = null;
                if (pendingPwmValue !== null) {
                    sendConfigCommand(pinName, "pwm_duty", pendingPwmValue);
                    pendingPwmValue = null;
                }
            }, 100); 
        }
    });

    pwmSlider.addEventListener("change", (e) => {
        if (pwmThrottleTimer) {
            clearTimeout(pwmThrottleTimer);
            pwmThrottleTimer = null;
        }
        sendConfigCommand(pinName, "pwm_duty", e.currentTarget.value);
        pendingPwmValue = null;
    });

    bindDragAndDrop(cardElement, containerDOM);
    return cardElement;
}

function bindDragAndDrop(card, containerDOM) {
    // Drag and drop support for reordering cards
    // You can grab the pin name and drag it around the grid
    const dragHandle = card.querySelector('.drag-handle');
    
    dragHandle.addEventListener('mousedown', (e) => {
        if (!e.target.closest('.menu-toggle')) card.setAttribute('draggable', 'true');
    });
    
    dragHandle.addEventListener('mouseup', () => card.removeAttribute('draggable'));

    card.addEventListener("dragstart", function(e) {
        draggedCard = this;
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", this.id);
        setTimeout(() => { 
            this.style.opacity = "0.4"; 
            document.body.classList.add("is-dragging"); 
        }, 0); 
    });

    card.addEventListener("dragend", function() {
        draggedCard = null;
        this.style.opacity = "1";
        document.body.classList.remove("is-dragging"); 
        containerDOM.querySelectorAll('.card').forEach(c => c.classList.remove('drag-over'));
        this.removeAttribute('draggable'); 
    });

    card.addEventListener("dragover", e => { 
        e.preventDefault(); 
        e.dataTransfer.dropEffect = "move"; 
        return false; 
    });

    card.addEventListener("dragenter", function(e) { 
        e.preventDefault(); 
        if (this !== draggedCard) this.classList.add("drag-over"); 
    });

    card.addEventListener("dragleave", function() { 
        this.classList.remove("drag-over"); 
    });

    card.addEventListener("drop", function(e) {
        e.stopPropagation(); 
        if (draggedCard !== this) {
            const siblingNode = draggedCard.nextSibling === this ? draggedCard : draggedCard.nextSibling;
            this.parentNode.insertBefore(draggedCard, this);
            this.parentNode.insertBefore(this, siblingNode);
        }
        this.classList.remove("drag-over");
        return false;
    });
}

// System commands and About window
// Bootsel mode and reboot triggers
// Reusable function for attaching confirm dialogs to system reboot commands
function bindSystemCommand(buttonId, confirmMessage, command, onSuccess) {
    const button = document.getElementById(buttonId);
    if (!button) return;

    button.addEventListener('click', () => {
        if (!confirm(confirmMessage)) return;
        fetch(`/system?cmd=${command}`)
            .then(onSuccess)
            .catch((err) => console.error(err));
    });
}

bindSystemCommand(
    'btn-bootsel-mode',
    'Reboot Pico into BOOTSEL mode? You will lose the network connection.',
    'bootsel',
    () => {
        document.body.innerHTML = "<h2 style='color:white; text-align:center; margin-top:50px;'>Pico is now in BOOTSEL Mode.<br>Ready for new firmware.</h2>";
    }
);

bindSystemCommand(
    'btn-reboot',
    'Reboot the Pico? You will temporarily lose the network connection.',
    'reboot',
    () => {
        document.body.innerHTML = "<h2 style='color:white; text-align:center; margin-top:50px;'>Pico is rebooting...<br>Please wait.</h2>";
        setTimeout(() => location.reload(), 3000);
    }
);

function initAboutWindow(containerDOM) {
    containerDOM.querySelector('.info-ip').textContent = host.replace(/\[|\]/g, '');
    fetch('/system?cmd=about').catch(err => console.error("Failed to request sysinfo:", err));

    // Determine the correct pinout image based on hardware and load it once
    const pinoutImg = containerDOM.querySelector('#about-pinout-img');
    
    const sysInfoHandler = (e) => {
        const info = e.detail || {};
        
        // Match the custom board name defined in board_config.h
        if (info.board && info.board.includes('PicoLab')) {
            pinoutImg.src = 'pinout_picolab.webp';
        } else {
            pinoutImg.src = 'pinout_pico2.webp';
        }
        
        pinoutImg.style.display = 'block';
        
        // Remove the listener so it doesn't trigger on subsequent sysinfo updates
        window.removeEventListener('pico-sysinfo', sysInfoHandler);
    };
    
    window.addEventListener('pico-sysinfo', sysInfoHandler);

    // no easter egg here
    const fwElement = containerDOM.querySelector('.info-fw');
    let clickCount = 0;

    fwElement.style.userSelect = 'none';
    fwElement.style.cursor = 'default';

    fwElement.addEventListener('click', () => {
        clickCount++;
        
        if (clickCount === 7) {
            alert("You are now a developer!");
            clickCount = 0;

            const style = document.createElement('style');
            style.innerHTML = `
                * { color: #00ff00 !important; border-color: #00ff00 !important; font-family: "Courier New", Courier, monospace !important; }
                body { background: #000000 !important; }
                body::after { content: ""; position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; background: repeating-linear-gradient(0deg, rgba(0,0,0,0.15), rgba(0,0,0,0.15) 1px, transparent 1px, transparent 2px); pointer-events: none; z-index: 999999; }
            `;
            document.head.appendChild(style);
        }
    });
}

function initSystemSettings(containerDOM) {
    const hueSlider = containerDOM.querySelector('.settings-theme-hue');
    const transparencySelect = containerDOM.querySelector('.settings-transparency-select');
    const csvSelect = containerDOM.querySelector('.settings-csv-delimiter');

    function syncControls() {
        if (hueSlider) hueSlider.value = String(UI_SETTINGS.wallpaperHue);
        if (transparencySelect) transparencySelect.value = UI_SETTINGS.transparentWindows ? 'transparent' : 'solid';
        if (csvSelect) csvSelect.value = getCsvDelimiter();
    }

    if (hueSlider) {
        hueSlider.addEventListener('input', (e) => {
            const hue = Math.max(0, Math.min(360, Number(e.target.value) || 0));
            setUiSetting('wallpaperHue', hue);
        });
    }

    if (transparencySelect) {
        transparencySelect.addEventListener('change', (e) => {
            setUiSetting('transparentWindows', e.target.value === 'transparent');
        });
    }

    if (csvSelect) {
        csvSelect.addEventListener('change', (e) => {
            setUiSetting('csvDelimiter', e.target.value === ';' ? ';' : ',');
        });
    }

    syncControls();
}

// Power supply monitoring and control
// Displays current draw on each rail and lets you configure the adjustable rail
function initPowerSupply(containerDOM) {
    const adjVoltageMin = 0.5;
    let adjVoltageMax = 5.0;

    const controls = {
        five: containerDOM.querySelector('.power-control-card[data-output="5v"] .power-toggle'),
        three: containerDOM.querySelector('.power-control-card[data-output="3v3"] .power-toggle'),
        adj: containerDOM.querySelector('.power-control-card[data-output="adj"] .power-toggle')
    };

    const voltageInput = containerDOM.querySelector('.power-voltage-input');
    const currentLimitInput = containerDOM.querySelector('.power-current-limit-input');
    const currentCanvas = containerDOM.querySelector('.power-current-canvas');
    const voltageCanvas = containerDOM.querySelector('.power-voltage-canvas');
    const currentCtx = currentCanvas.getContext('2d');
    const voltageCtx = voltageCanvas.getContext('2d');
    const readout5V = containerDOM.querySelector('.val-5v');
    const readout3V = containerDOM.querySelector('.val-3v3');

    const State = {
        outputs: {
            five: { enabled: true, current: 0 },
            three: { enabled: true, current: 0 },
            adj: { enabled: true, current: 0, voltage: 0, setVoltage: 1.1, setLimit: 500 }
        },
        history: [],
        startTime: Date.now()
    };

    let psuHoverX = null;
    let psuHoverCanvas = null;
    [currentCanvas, voltageCanvas].forEach(c => {
        c.addEventListener('mousemove', (e) => {
            psuHoverX = e.clientX - c.getBoundingClientRect().left;
            psuHoverCanvas = c;
            requestAnimationFrame(render);
        });
        c.addEventListener('mouseleave', () => {
            psuHoverX = null;
            psuHoverCanvas = null;
            requestAnimationFrame(render);
        });
    });

    function clamp(value, min, max) {
        return Math.min(max, Math.max(min, value));
    }

    function updateControlState() {
        const controlsEnabled = HAS_ADJ_RAIL;

        Object.entries(controls).forEach(([key, button]) => {
            if (!button) return;
            const isOn = State.outputs[key].enabled;
            
            button.classList.toggle('active', isOn);
            button.classList.toggle('inactive', !isOn);
            button.textContent = isOn ? 'ON' : 'OFF';
            
            if (key === 'five' || key === 'three') {
                button.disabled = true;
            } else {
                button.disabled = !controlsEnabled;
            }
        });

        if (voltageInput) voltageInput.disabled = !controlsEnabled;
        if (currentLimitInput) currentLimitInput.disabled = !controlsEnabled;
    }

    function recordSample() {
        State.history.push({
            t: Date.now() - State.startTime,
            five: State.outputs.five.current,
            three: State.outputs.three.current,
            adjCurrent: State.outputs.adj.current,
            adjVoltage: State.outputs.adj.voltage
        });

        if (State.history.length > 2000) {
            State.history.shift();
        }
    }

    // Renders the power telemetry onto an HTML canvas
    function drawChart(ctx, canvas, config) {
        const width = canvas.width;
        const height = canvas.height;
        ctx.clearRect(0, 0, width, height);

        /* Skip drawing if canvas hasn't been sized yet (common on initial load) */
        if (width <= 0 || height <= 0) return;

        const margin = { left: 44, right: 16, top: 16, bottom: 24 };
        const graphW = width - margin.left - margin.right;
        const graphH = height - margin.top - margin.bottom;
        const plotLeft = margin.left;
        const plotRight = margin.left + graphW;
        const plotBottom = margin.top + graphH;

        // Draw main axes
        ctx.strokeStyle = '#3c3c4a';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(plotLeft, margin.top);
        ctx.lineTo(plotLeft, plotBottom);
        ctx.stroke();

        ctx.beginPath();
        ctx.moveTo(plotLeft, plotBottom);
        ctx.lineTo(plotRight, plotBottom);
        ctx.stroke();

        // Default to a 10s window if we have no telemetry history yet
        const maxTime = State.history.length > 0 ? Math.max(...State.history.map(sample => sample.t)) : 10000;
        const minTime = maxTime - 10000; 
        const timeScale = graphW / 10000;

        ctx.save();
        ctx.beginPath();
        ctx.rect(plotLeft, margin.top, graphW, graphH);
        ctx.clip();

        // Draw background grid lines
        ctx.strokeStyle = '#3c3c4a';
        for (let i = 0; i <= 5; i++) {
            const y = margin.top + (graphH / 5) * i;
            ctx.beginPath();
            ctx.moveTo(plotLeft, y);
            ctx.lineTo(plotRight, y);
            ctx.stroke();
        }

        /* Only draw the data series lines if we actually have telemetry */
        if (State.history.length >= 2) {
            const drawSeries = (series, color, active) => {
                if (!active) return;
                ctx.beginPath();
                ctx.strokeStyle = color;
                ctx.lineWidth = 1.5;
                State.history.forEach((sample, index) => {
                    const x = plotLeft + (sample.t - minTime) * timeScale;
                    const normalized = clamp((series(sample) / config.maxValue), 0, 1);
                    const y = margin.top + graphH - normalized * graphH;
                    if (index === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                });
                ctx.stroke();
            };

            config.series.forEach(entry => drawSeries(entry.fn, entry.color, entry.active));
        }
        ctx.restore();

        // Y-axis labels
        ctx.fillStyle = '#a0a0ab';
        ctx.font = '10px sans-serif';
        ctx.textAlign = 'right';
        for (let i = 0; i <= 5; i++) {
            const y = margin.top + (graphH / 5) * i;
            const value = config.maxValue - ((config.maxValue / 5) * i);
            ctx.fillText(`${value.toFixed(config.precision)}${config.unit}`, plotLeft - 6, y + 3);
        }

        // Tooltip Overlay - shows values when you hover
        if (psuHoverCanvas === canvas && psuHoverX !== null && psuHoverX >= plotLeft && psuHoverX <= plotRight && State.history.length > 0) {
            const mouseTime = minTime + ((psuHoverX - plotLeft) / timeScale);
            
            const closest = State.history.reduce((prev, curr) => 
                Math.abs(curr.t - mouseTime) < Math.abs(prev.t - mouseTime) ? curr : prev
            );

            ctx.strokeStyle = 'rgba(255, 255, 255, 0.4)';
            ctx.beginPath(); 
            ctx.moveTo(psuHoverX, margin.top); 
            ctx.lineTo(psuHoverX, plotBottom); 
            ctx.stroke();

            const activeSeries = config.series.filter(s => s.active);
            const boxW = 80;
            const boxH = 10 + (activeSeries.length * 15);
            const boxX = (psuHoverX + 15 + boxW > plotRight) ? psuHoverX - boxW - 10 : psuHoverX + 10;
            
            ctx.fillStyle = 'rgba(34, 34, 38, 0.9)';
            ctx.strokeStyle = '#555';
            ctx.fillRect(boxX, margin.top + 5, boxW, boxH);
            ctx.strokeRect(boxX, margin.top + 5, boxW, boxH);
            
            ctx.fillStyle = '#fff';
            ctx.font = '11px sans-serif';
            ctx.textAlign = 'left';
            ctx.textBaseline = 'top';
            
            let yOff = margin.top + 10;
            activeSeries.forEach(s => {
                ctx.fillStyle = s.color;
                const displayPrecision = s.precision !== undefined ? s.precision : config.precision;
                ctx.fillText(`${s.fn(closest).toFixed(displayPrecision)} ${config.unit}`, boxX + 8, yOff);
                yOff += 15;
            });
        }
    }

    function render() {
        currentCtx.clearRect(0, 0, currentCanvas.width, currentCanvas.height);
        voltageCtx.clearRect(0, 0, voltageCanvas.width, voltageCanvas.height);

        drawChart(currentCtx, currentCanvas, {
            placeholder: 'Waiting for telemetry',
            maxValue: 500,
            precision: 0,
            unit: 'mA',
            series: [
                { fn: sample => sample.five, color: '#c68e58', active: true },       
                { fn: sample => sample.three, color: '#c65a58', active: true },     
                { fn: sample => sample.adjCurrent, color: '#b3b3b3', active: true }   
            ]
        });

        drawChart(voltageCtx, voltageCanvas, {
            placeholder: 'Waiting for ADJ voltage',
            maxValue: adjVoltageMax,
            precision: 1,
            unit: 'V',
            series: [
                { fn: sample => sample.adjVoltage, color: '#b3b3b3', active: true, precision: 3 }   
            ]
        });
    }

    let lastDigitUpdateMs = 0;

    // Apply telemetry data to the power supply displays.
    // Rate-limits digit updates to avoid flicker.
    function applyTelemetry() {
        const psu = LiveTelemetry.PSU || {};
        
        State.outputs.five.current = clamp(psu.i5 || 0, 0, 500);
        State.outputs.three.current = clamp(psu.i3 || 0, 0, 500);
        State.outputs.adj.current = clamp(psu.ip || 0, 0, 500);
        State.outputs.adj.voltage = clamp((psu.vp || 0) / 1000, 0, adjVoltageMax);

        const now = Date.now();
        if (now - lastDigitUpdateMs > 300) {
            lastDigitUpdateMs = now;
            
            if (readout5V) readout5V.textContent = `${State.outputs.five.current.toFixed(0)} mA`;
            if (readout3V) readout3V.textContent = `${State.outputs.three.current.toFixed(0)} mA`;
        }

        recordSample();
        render();
    }

    Object.values(controls).forEach(toggle => {
        if (!toggle) return;
        toggle.addEventListener('click', () => {
            const output = toggle.dataset.output || toggle.closest('.power-control-card').dataset.output;
            const key = output === '3v3' ? 'three' : output === 'adj' ? 'adj' : 'five';
            State.outputs[key].enabled = !State.outputs[key].enabled;
            
            updateControlState();
            render();
            fetch(`/power?output=${output}&state=${State.outputs[key].enabled ? 1 : 0}`).catch(() => {});
        });
    });

    function commitAdjValue(input, key, formatter) {
        if (!input) return;
        const parsed = parseFloat(input.value);
        if (!Number.isFinite(parsed)) return;
        
        if (key === 'setVoltage') {
            State.outputs.adj[key] = clamp(parsed, adjVoltageMin, adjVoltageMax);
            input.value = formatter(State.outputs.adj[key]);
            fetch(`/power?output=adj&voltage=${input.value}`).catch(() => {});
        } else {
            const quantized = Math.round(parsed / 50) * 50;
            State.outputs.adj[key] = clamp(quantized, 50, 500);
            input.value = formatter(State.outputs.adj[key]);
            fetch(`/power?output=adj&limit=${input.value}`).catch(() => {});
        }
        render();
    }

    if (voltageInput) {
        voltageInput.min = String(adjVoltageMin);
        voltageInput.max = String(adjVoltageMax);
        voltageInput.addEventListener('change', () => {
            commitAdjValue(voltageInput, 'setVoltage', value => value.toFixed(2));
        });
    }

    if (currentLimitInput) {
        currentLimitInput.addEventListener('change', () => {
            commitAdjValue(currentLimitInput, 'setLimit', value => value.toFixed(0));
        });
    }

    if (currentCanvas && currentCanvas.parentElement) {
        new ResizeObserver(() => {
            currentCanvas.width = currentCanvas.parentElement.clientWidth;
            currentCanvas.height = currentCanvas.parentElement.clientHeight;
            render();
        }).observe(currentCanvas.parentElement);
    }

    if (voltageCanvas && voltageCanvas.parentElement) {
        new ResizeObserver(() => {
            voltageCanvas.width = voltageCanvas.parentElement.clientWidth;
            voltageCanvas.height = voltageCanvas.parentElement.clientHeight;
            render();
        }).observe(voltageCanvas.parentElement);
    }

    updateControlState();
    render();
    applyTelemetry();

    window.addEventListener('pico-telemetry-update', () => {
        applyTelemetry();
    });

    window.addEventListener('pico-sysinfo', (e) => {
        const info = e.detail || {};
        const fs = Number(info.adc_fs);
        if (Number.isFinite(fs) && fs > 0) {
            adjVoltageMax = fs;
            if (voltageInput) {
                voltageInput.min = String(adjVoltageMin);
                voltageInput.max = String(adjVoltageMax);
                const currentValue = parseFloat(voltageInput.value);
                if (currentValue < adjVoltageMin) {
                    voltageInput.value = adjVoltageMin.toFixed(2);
                } else if (currentValue > adjVoltageMax) {
                    voltageInput.value = adjVoltageMax.toFixed(2);
                }
            }
        }
        updateControlState();
        render();
    });
}

// Data logger - record and export analog/digital waveforms
// Samples at configurable rates and lets you export as CSV
function initLogger(containerDOM) {
    const canvas = containerDOM.querySelector('.logger-canvas');
    const ctx = canvas.getContext('2d');
    
    const Config = {
        colors: {
            A1: '#ffcc00', A2: '#34c759', A3: '#007aff', A4: '#ff3b30',
            I5: '#c68e58', I3: '#c65a58', IA: '#b3b3b3', VA: '#66d9ef',
            D1: '#ff9500', D2: '#ff2a6d', D3: '#af52de', D4: '#05d9e8', 
            D5: '#cba6f7', D6: '#5856d6', D7: '#d1f534', D8: '#ff8a65'
        },
        margin: { left: 50, right: 48, top: 20, bottom: 20 }
    };

    const State = {
        dataStore: [], 
        isRecording: false, 
        intervalId: null, 
        startTime: Date.now(), 
        pauseTime: null,
        /* Tracking pan/zoom state for the chart view */
        view: { minTime: 0, timeScale: 0.1, voltScale: 1 },
        interaction: { isDragging: false, lastMouseX: 0, hoverData: null }
    };

    const elements = {
        btnToggle: containerDOM.querySelector('.btn-toggle-record'),
        btnClear: containerDOM.querySelector('.btn-clear'),
        btnExport: containerDOM.querySelector('.export-btn'),
        sampleRate: containerDOM.querySelector('.sample-rate'),
        pinBtns: containerDOM.querySelectorAll('.pin-select-btn')
    };

    new ResizeObserver(() => {
        canvas.width = canvas.parentElement.clientWidth;
        canvas.height = canvas.parentElement.clientHeight;
        requestAnimationFrame(render);
    }).observe(canvas.parentElement);

    const iconPlay = '<svg viewBox="0 0 24 24" width="16" height="16"><path fill="currentColor" d="M8 5v14l11-7z"/></svg> <span>Run</span>';
    const iconStop = '<svg viewBox="0 0 24 24" width="16" height="16"><path fill="currentColor" d="M6 6h12v12H6z"/></svg> <span>Stop</span>';

    elements.btnToggle.addEventListener('click', () => {
        if (!State.isRecording) {
            State.isRecording = true;
            elements.btnToggle.classList.add('recording');
            elements.btnToggle.innerHTML = iconStop;
            elements.btnToggle.title = "Stop Recording";
            
            if (State.dataStore.length === 0) { 
                State.startTime = Date.now(); 
                State.pauseTime = null; 
            } else if (State.pauseTime) { 
                State.startTime += (Date.now() - State.pauseTime); 
                State.pauseTime = null; 
            }
            startSampling();
        } else {
            State.isRecording = false;
            elements.btnToggle.classList.remove('recording');
            elements.btnToggle.innerHTML = iconPlay;
            elements.btnToggle.title = "Start Recording";
            
            clearInterval(State.intervalId);
            State.pauseTime = Date.now();
        }
    });

    elements.btnClear.addEventListener('click', () => {
        State.dataStore = []; 
        State.startTime = Date.now(); 
        State.pauseTime = null; 
        State.view.minTime = 0;
        render();
    });

    elements.sampleRate.addEventListener('change', () => { if (State.isRecording) startSampling(); });
    elements.pinBtns.forEach(btn => btn.addEventListener('click', e => { e.target.classList.toggle('active'); render(); }));

    // Begins appending telemetry snapshots into the local data store at a set interval
    function startSampling() {
        clearInterval(State.intervalId);
        const rate = parseInt(elements.sampleRate.value, 10);
        
        const takeSample = () => {
            const timestamp = Date.now() - State.startTime;
            const entry = {
                time: timestamp,
                A: { ...LiveTelemetry.A },
                P: HAS_ADJ_RAIL ? {
                    I5: LiveTelemetry.PSU.i5,
                    I3: LiveTelemetry.PSU.i3,
                    IA: LiveTelemetry.PSU.ip,
                    VA: LiveTelemetry.PSU.vp / 1000
                } : {},
                D: { ...LiveTelemetry.D }
            };
            State.dataStore.push(entry);
            
            // Auto-scroll the view to follow new samples
            const graphW = canvas.width - Config.margin.left - Config.margin.right;
            if (graphW > 0) {
                const maxVisibleTime = State.view.minTime + (graphW / State.view.timeScale);
                if (timestamp > maxVisibleTime - (graphW * 0.1)) {
                    State.view.minTime = timestamp - graphW + (graphW * 0.1);
                }
            }
            render();
        };

        takeSample();
        State.intervalId = setInterval(takeSample, rate);
    }

    function getActiveChannels() {
        const active = { A: [], P: [], D: [] };
        containerDOM.querySelectorAll('.pin-select-btn.active').forEach(btn => {
            active[btn.dataset.type].push(btn.dataset.pin);
        });
        return active;
    }

    function render() {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        const active = getActiveChannels();
        const graphW = canvas.width - Config.margin.left - Config.margin.right;
        const graphH = canvas.height - Config.margin.top - Config.margin.bottom;
        
        if(graphW <= 0 || graphH <= 0) return;

        const hasAnalogChannels = active.A.length + active.P.length > 0;
        const currentChannels = active.P.filter(pin => pin !== 'VA');
        const voltageMax = Math.max(3, ADC_REF_V, active.P.includes('VA') ? 5 : 0);
        const currentMax = 500;
        const digitalAreaH = active.D.length > 0 ? (hasAnalogChannels ? graphH * 0.40 : graphH) : 0;
        const analogAreaH = graphH - digitalAreaH;
        const analogStartY = Config.margin.top + digitalAreaH;

        ctx.strokeStyle = '#555'; 
        ctx.fillStyle = '#a0a0ab'; 
        ctx.font = "10px sans-serif";
        ctx.textAlign = "center"; 
        ctx.textBaseline = "top";
        
        const yAxisLine = canvas.height - Config.margin.bottom;
        ctx.beginPath(); 
        ctx.moveTo(Config.margin.left, yAxisLine); 
        ctx.lineTo(Config.margin.left + graphW, yAxisLine); 
        ctx.stroke();

        const visibleMs = graphW / State.view.timeScale;
        let tickInterval = 100;
        
        if (visibleMs > 1200000) tickInterval = 200000;
        else if (visibleMs > 600000) tickInterval = 100000;
        else if (visibleMs > 300000) tickInterval = 60000;
        else if (visibleMs > 120000) tickInterval = 20000;
        else if (visibleMs > 60000) tickInterval = 10000;
        else if (visibleMs > 30000) tickInterval = 5000;
        else if (visibleMs > 15000) tickInterval = 2000;
        else if (visibleMs > 5000) tickInterval = 1000;
        else if (visibleMs > 2000) tickInterval = 500;
        else if (visibleMs > 1000) tickInterval = 200;

        const firstTick = Math.ceil(State.view.minTime / tickInterval) * tickInterval;
        
        ctx.strokeStyle = '#3c3c4a'; 
        for (let t = firstTick; t < State.view.minTime + visibleMs; t += tickInterval) {
            if (t < 0) continue;
            const x = Config.margin.left + (t - State.view.minTime) * State.view.timeScale;
            ctx.beginPath(); 
            ctx.moveTo(x, Config.margin.top); 
            ctx.lineTo(x, yAxisLine); 
            ctx.stroke();
            ctx.fillText(`${(t/1000).toFixed(1)}s`, x, yAxisLine + 6); 
        }
        
        if (analogAreaH > 0) {
            ctx.beginPath(); 
            ctx.moveTo(Config.margin.left, analogStartY); 
            ctx.lineTo(Config.margin.left, analogStartY + analogAreaH); 
            ctx.stroke();
            
            ctx.textAlign = "right"; 
            ctx.textBaseline = "middle"; 
            ctx.strokeStyle = '#3c3c4a';
            
            for (let tick = 0; tick <= 4; tick++) {
                const value = voltageMax * tick / 4;
                const y = analogStartY + analogAreaH - (value / voltageMax) * analogAreaH * State.view.voltScale;
                ctx.beginPath();
                ctx.moveTo(Config.margin.left, y);
                ctx.lineTo(Config.margin.left + graphW, y);
                ctx.stroke();
                ctx.fillText(`${value.toFixed(voltageMax <= 5 ? 1 : 0)}V`, Config.margin.left - 8, y);
            }

            if (currentChannels.length > 0) {
                const rightAxisX = Config.margin.left + graphW;
                ctx.beginPath();
                ctx.moveTo(rightAxisX, analogStartY);
                ctx.lineTo(rightAxisX, analogStartY + analogAreaH);
                ctx.stroke();
                ctx.textAlign = 'left';

                for (let tick = 0; tick <= 4; tick++) {
                    const value = currentMax * tick / 4;
                    const y = analogStartY + analogAreaH - (value / currentMax) * analogAreaH * State.view.voltScale;
                    ctx.beginPath();
                    ctx.moveTo(rightAxisX, y);
                    ctx.lineTo(rightAxisX + 4, y);
                    ctx.stroke();
                    ctx.fillText(`${value}mA`, rightAxisX + 7, y);
                }
            }
        }

        if (State.dataStore.length > 0) {
            ctx.save(); 
            ctx.beginPath(); 
            ctx.rect(Config.margin.left, 0, graphW, canvas.height); 
            ctx.clip();
            
            if (active.D.length > 0) {
                const laneH = digitalAreaH / active.D.length;
                active.D.forEach((pin, index) => {
                    const gap = laneH * 0.15; 
                    const baseY = Config.margin.top + (index + 1) * laneH - gap;
                    const highY = Config.margin.top + index * laneH + gap;
                    
                    ctx.beginPath(); 
                    ctx.strokeStyle = Config.colors[pin]; 
                    ctx.lineWidth = 1.5;
                    
                    State.dataStore.forEach((d, i) => {
                        const x = Config.margin.left + (d.time - State.view.minTime) * State.view.timeScale;
                        const y = d.D[pin] === 1 ? highY : baseY;
                        if (i === 0) {
                            ctx.moveTo(x, y);
                        } else { 
                            ctx.lineTo(x, State.dataStore[i-1].D[pin] === 1 ? highY : baseY); 
                            ctx.lineTo(x, y); 
                        }
                    });
                    ctx.stroke();
                });
            }

            const analogChannels = [...active.A, ...active.P];
            if (analogChannels.length > 0) {
                analogChannels.forEach(pin => {
                    ctx.beginPath(); 
                    ctx.strokeStyle = Config.colors[pin]; 
                    ctx.lineWidth = 1.5;
                    
                    State.dataStore.forEach((d, i) => {
                        const x = Config.margin.left + (d.time - State.view.minTime) * State.view.timeScale;
                        const isPowerChannel = active.P.includes(pin);
                        const isCurrentChannel = isPowerChannel && pin !== 'VA';
                        const val = isPowerChannel ? d.P[pin] : d.A[pin];
                        const maxValue = isCurrentChannel ? currentMax : voltageMax;
                        const clamped = Math.max(0, Math.min(maxValue, val || 0));
                        const y = analogStartY + analogAreaH - (clamped / maxValue) * analogAreaH * State.view.voltScale;
                        
                        if (i === 0) ctx.moveTo(x, y); 
                        else ctx.lineTo(x, y);
                    });
                    ctx.stroke();
                });
            }
            ctx.restore();

            if (active.D.length > 0) {
                const laneH = digitalAreaH / active.D.length;
                active.D.forEach((pin, index) => {
                    ctx.fillStyle = Config.colors[pin]; 
                    ctx.textAlign = "right"; 
                    ctx.textBaseline = "middle";
                    ctx.fillText(pin, Config.margin.left - 8, Config.margin.top + (index + 0.5) * laneH);
                });
            }
        }

        drawTooltip(active);
    }

    function drawTooltip(active) {
        if (!State.interaction.hoverData || !State.interaction.hoverData.point) return;
        
        const pt = State.interaction.hoverData.point;
        const px = Config.margin.left + (pt.time - State.view.minTime) * State.view.timeScale;
        
        if (px < Config.margin.left || px > canvas.width - Config.margin.right) return;

        ctx.strokeStyle = 'rgba(255, 255, 255, 0.4)';
        ctx.beginPath(); 
        ctx.moveTo(px, Config.margin.top); 
        ctx.lineTo(px, canvas.height - Config.margin.bottom); 
        ctx.stroke();

        const boxW = 100;
        const boxH = 20 + (active.A.length + active.P.length + active.D.length) * 14;
        const boxX = px + 15 + boxW > canvas.width - Config.margin.right ? px - boxW - 15 : px + 15;

        ctx.fillStyle = 'rgba(34, 34, 38, 0.9)';
        ctx.strokeStyle = '#555';
        ctx.fillRect(boxX, Config.margin.top, boxW, boxH);
        ctx.strokeRect(boxX, Config.margin.top, boxW, boxH);

        ctx.fillStyle = '#fff';
        ctx.font = "11px sans-serif";
        ctx.textAlign = "left";
        ctx.textBaseline = "top";
        ctx.fillText(`${(pt.time/1000).toFixed(2)}s`, boxX + 8, Config.margin.top + 6);
        
        let yOffset = Config.margin.top + 22;
        active.A.forEach(pin => {
            ctx.fillStyle = Config.colors[pin];
            const val = pt.A[pin] !== undefined ? pt.A[pin] : 0;
            ctx.fillText(`${pin}: ${val.toFixed(2)}V`, boxX + 8, yOffset);
            yOffset += 14;
        });
        active.P.forEach(pin => {
            ctx.fillStyle = Config.colors[pin];
            const val = pt.P[pin] !== undefined ? pt.P[pin] : 0;
            const unit = pin === 'VA' ? 'V' : 'mA';
            const precision = pin === 'VA' ? 3 : 0;
            ctx.fillText(`${pin}: ${val.toFixed(precision)}${unit}`, boxX + 8, yOffset);
            yOffset += 14;
        });
        active.D.forEach(pin => {
            ctx.fillStyle = Config.colors[pin];
            const val = pt.D[pin] !== undefined ? pt.D[pin] : 0;
            ctx.fillText(`${pin}: ${val}`, boxX + 8, yOffset);
            yOffset += 14;
        });
    }

    canvas.addEventListener('mousedown', e => { 
        State.interaction.isDragging = true; 
        State.interaction.lastMouseX = e.clientX; 
        canvas.style.cursor = 'grabbing'; 
    });
    
    window.addEventListener('mouseup', () => { 
        State.interaction.isDragging = false; 
        canvas.style.cursor = 'crosshair'; 
    });
    
    canvas.addEventListener('mousemove', e => {
        const rect = canvas.getBoundingClientRect();
        const mouseX = e.clientX - rect.left;

        if (State.interaction.isDragging) {
            // Pan the view by dragging horizontally
            State.view.minTime -= (e.clientX - State.interaction.lastMouseX) / State.view.timeScale;
            if (State.view.minTime < 0) State.view.minTime = 0; 
            State.interaction.lastMouseX = e.clientX;
            State.interaction.hoverData = null; 
        } else if (mouseX > Config.margin.left && mouseX < canvas.width - Config.margin.right && State.dataStore.length > 0) {
            const mouseTime = State.view.minTime + (mouseX - Config.margin.left) / State.view.timeScale;
            const closest = State.dataStore.reduce((prev, curr) => 
                Math.abs(curr.time - mouseTime) < Math.abs(prev.time - mouseTime) ? curr : prev
            );
            
            if (Math.abs(closest.time - mouseTime) * State.view.timeScale < 20) {
                State.interaction.hoverData = { point: closest };
            } else {
                State.interaction.hoverData = null;
            }
        } else {
            State.interaction.hoverData = null;
        }
        render();
    });

    canvas.addEventListener('mouseleave', () => {
        State.interaction.hoverData = null;
        State.interaction.isDragging = false;
        render();
    });

    // Generates and downloads a CSV file of the recorded telemetry
    elements.btnExport.addEventListener('click', () => {
        if (State.dataStore.length === 0) return;
        const delimiter = getCsvDelimiter();
        // Build CSV with all recorded channels
        const headers = ["Time(s)","A1(V)","A2(V)","A3(V)","A4(V)"];
        if (HAS_ADJ_RAIL) headers.push("I5V0(mA)","I3V3(mA)","IADJ(mA)","VADJ(V)");
        headers.push("D1","D2","D3","D4","D5","D6","D7","D8");
        let csv = headers.join(delimiter) + "\n";
        State.dataStore.forEach(d => {
            const row = [(d.time/1000).toFixed(3), (d.A.A1||0).toFixed(3), (d.A.A2||0).toFixed(3), (d.A.A3||0).toFixed(3), (d.A.A4||0).toFixed(3)];
            if (HAS_ADJ_RAIL) row.push(d.P.I5||0, d.P.I3||0, d.P.IA||0, (d.P.VA||0).toFixed(3));
            row.push(d.D.D1||0, d.D.D2||0, d.D.D3||0, d.D.D4||0, d.D.D5||0, d.D.D6||0, d.D.D7||0, d.D.D8||0);
            csv += row.join(delimiter) + "\n";
        });
        const blob = new Blob([csv], { type: 'text/csv' });
        const a = document.createElement('a'); 
        a.href = URL.createObjectURL(blob); 
        a.download = `datalog_${Date.now()}.csv`;
        a.click();
    });
}

// Oscilloscope app - triggered capture with configurable timebase
// Real-time waveform display with trigger level and edge selection
function initScope(containerDOM) {
    // Standard oscilloscope channel color mapping
    const scopeColors = { A1: '#ffcc00', A2: '#34c759', A3: '#007aff', A4: '#ff3b30' };
    const canvas = containerDOM.querySelector('.scope-canvas');
    const ctx = canvas.getContext('2d');
    
    const btnRunStop = containerDOM.querySelector('.btn-run-stop');
    const chToggles = containerDOM.querySelectorAll('.ch-toggle');
    const modeBtns = containerDOM.querySelectorAll('.mode-selector .sub-btn');
    const trigEdgeBtn = containerDOM.querySelector('.trig-edge');

    let isRunning = false;
    let trigEdgeRising = true;
    let visualTimebaseMs = parseFloat(containerDOM.querySelector('.timebase').value);
    let frameTimebaseMs = visualTimebaseMs;
    // Offset view to center the initial frame
    let xOffsetMs = -(visualTimebaseMs * 5);
    
    let lastFrameSamples = {};
    let lastFrameActive = [];

    // Debounce config updates to avoid hammering the API
    let configTimeout = null;

    // Sync UI styling with initial active channel states
    chToggles.forEach(btn => {
        const card = btn.closest('.scope-ch-card');
        if (btn.classList.contains('active')) {
            card.classList.remove('disabled');
        } else {
            card.classList.add('disabled');
        }
    });

    // Auto-resize canvas to match container layout changes
    new ResizeObserver(() => {
        canvas.width = canvas.parentElement.clientWidth;
        canvas.height = canvas.parentElement.clientHeight;
        forceRedraw();
    }).observe(canvas.parentElement);

    // Packages current UI settings and sends them to the Pico backend
    function sendScopeConfig(runState) {
        if (configTimeout) clearTimeout(configTimeout);

        configTimeout = setTimeout(() => {
            const ch = parseInt(containerDOM.querySelector('.trig-src').value.replace('A', '')) - 1;
            const level = containerDOM.querySelector('.trig-level').value;
            const tb = containerDOM.querySelector('.timebase').value;
            const edge = trigEdgeRising ? 1 : 0;
            
            const modeStr = containerDOM.querySelector('.mode-selector .active').getAttribute('data-mode');
            let modeNum = 0;
            if (modeStr === 'single') modeNum = 1;
            else if (modeStr === 'auto') modeNum = 2;
            
            // Build bitmask for enabled channels
            let mask = 0;
            if (!containerDOM.querySelector('.scope-ch-card[data-ch="A1"]').classList.contains('disabled')) mask |= 1;
            if (!containerDOM.querySelector('.scope-ch-card[data-ch="A2"]').classList.contains('disabled')) mask |= 2;
            if (!containerDOM.querySelector('.scope-ch-card[data-ch="A3"]').classList.contains('disabled')) mask |= 4;
            if (!containerDOM.querySelector('.scope-ch-card[data-ch="A4"]').classList.contains('disabled')) mask |= 8;
            if (mask === 0) mask = 1; 
            
            fetch(`/scope?run=${runState}&trig_ch=${ch}&trig_level=${level}&trig_edge=${edge}&tb=${tb}&mask=${mask}&mode=${modeNum}&x_off=0`)
                .catch(err => console.error("Scope Config Error:", err));
                
            configTimeout = null;
        }, 100);
    }
    
    // Toggle acquisition state
    btnRunStop.addEventListener('click', () => {
        isRunning = !isRunning;
        if (isRunning) {
            btnRunStop.classList.replace('stopped', 'running');
            btnRunStop.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" style="vertical-align: middle; margin-right: 4px;"><path fill="currentColor" d="M6 6h12v12H6z"/></svg> Stop';
            sendScopeConfig(1);
        } else {
            btnRunStop.classList.replace('running', 'stopped');
            btnRunStop.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" style="vertical-align: middle; margin-right: 4px;"><path fill="currentColor" d="M8 5v14l11-7z"/></svg> Run';
            sendScopeConfig(0);
        }
    });

    // Decode base64 scope data back into 16-bit ADC samples
    function base64ToUint16Array(base64) {
        const binaryString = atob(base64);
        const bytes = new Uint8Array(binaryString.length);
        for (let i = 0; i < binaryString.length; i++) bytes[i] = binaryString.charCodeAt(i);
        return new Uint16Array(bytes.buffer);
    }

    // Double-click to reset zoom/pan to default view
    canvas.addEventListener('dblclick', () => {
        visualTimebaseMs = frameTimebaseMs;
        xOffsetMs = -(visualTimebaseMs * 5);
        forceRedraw();
        sendScopeConfig(isRunning ? 1 : 0);
    });
            
    window.addEventListener("pico-telemetry-update", () => {
        // This is a passive hook to ensure components remain active
    });

    if (evtSource) {
        // Handle incoming SSE payload containing new waveform data
        evtSource.addEventListener("scope", function(event) {
            if (!document.body.contains(containerDOM)) return;
            
            const data = JSON.parse(event.data);
            const rawSamples = base64ToUint16Array(data.data);
            const mode = containerDOM.querySelector('.mode-selector .active').getAttribute('data-mode');
            const captureTimebaseMs = Number(data.tb);
            frameTimebaseMs = Number.isFinite(captureTimebaseMs) && captureTimebaseMs > 0
                ? captureTimebaseMs
                : parseFloat(containerDOM.querySelector('.timebase').value);
            visualTimebaseMs = frameTimebaseMs;
            xOffsetMs = -(visualTimebaseMs * 5);
            
            const activeChannels = [];
            // Extract which channels are enabled from the mask bitmask
            if (data.mask & 1) activeChannels.push(0);
            if (data.mask & 2) activeChannels.push(1);
            if (data.mask & 4) activeChannels.push(2);
            if (data.mask & 8) activeChannels.push(3);

            const ptsPerCh = rawSamples.length / activeChannels.length;
            const chSamples = {};
            
            // Initialize typed arrays for deinterleaved data
            activeChannels.forEach(ch => chSamples[ch] = new Uint16Array(ptsPerCh));

            // Unpack interleaved data: [A1, A2, A3, A4, A1, A2, ...]
            for(let i = 0; i < ptsPerCh; i++) {
                activeChannels.forEach((ch, idx) => {
                    chSamples[ch][i] = rawSamples[i * activeChannels.length + idx];
                });
            }

            lastFrameSamples = chSamples;
            lastFrameActive = activeChannels;

            forceRedraw();

            // Automatically stop if single-shot mode fired
            if (isRunning && mode === 'single') {
                isRunning = false;
                btnRunStop.classList.replace('running', 'stopped');
                btnRunStop.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" style="vertical-align: middle; margin-right: 4px;"><path fill="currentColor" d="M8 5v14l11-7z"/></svg> Run';
            }
        });
    }

    let isDragging = false;
    let lastMouseX = 0;
    let hoverPos = null;

    canvas.addEventListener('mousedown', (e) => {
        isDragging = true;
        lastMouseX = e.clientX;
        canvas.style.cursor = 'grabbing';
    });

    window.addEventListener('mouseup', () => {
        if (isDragging) {
            isDragging = false;
            canvas.style.cursor = 'crosshair';
            sendScopeConfig(isRunning ? 1 : 0);
        }
    });

    // Handle mouse wheel zoom
    canvas.addEventListener('wheel', (e) => {
        e.preventDefault(); 
        if (lastFrameActive.length === 0) return;

        const hwTimebaseMs = frameTimebaseMs;
        
        // Calculate the center point before zooming
        const currentTotalVisibleMs = visualTimebaseMs * 10;
        const centerTimeMs = xOffsetMs + (currentTotalVisibleMs / 2);

        // Apply zoom factor
        if (e.deltaY < 0) {
            visualTimebaseMs /= 1.2; // Zoom in
        } else {
            visualTimebaseMs *= 1.2; // Zoom out
        }

        // Clamp zoom bounds (from 1000x zoom up to the full hardware capture width)
        const maxTb = hwTimebaseMs; 
        const minTb = hwTimebaseMs / 1000; 
        if (visualTimebaseMs > maxTb) visualTimebaseMs = maxTb;
        if (visualTimebaseMs < minTb) visualTimebaseMs = minTb;

        // Recalculate offset to keep the center of the screen stable
        const newTotalVisibleMs = visualTimebaseMs * 10;
        xOffsetMs = centerTimeMs - (newTotalVisibleMs / 2);
        
        // Clamp panning bounds
        const maxOffsetMs = hwTimebaseMs * 5;
        if (xOffsetMs < -maxOffsetMs) xOffsetMs = -maxOffsetMs;
        if (xOffsetMs > maxOffsetMs - newTotalVisibleMs) {
             xOffsetMs = maxOffsetMs - newTotalVisibleMs;
        }

        requestRedraw();
    });

    // Handle drag panning and hover tooltips
    canvas.addEventListener('mousemove', (e) => {
        const rect = canvas.getBoundingClientRect();
        const clientX = e.clientX - rect.left;

        if (isDragging) {
            const dx = e.clientX - lastMouseX;
            lastMouseX = e.clientX;
            
            const hwTimebaseMs = frameTimebaseMs;
            const totalVisibleMs = visualTimebaseMs * 10; 
            const graphW = canvas.width - 70; 
            
            // Pan by converting mouse delta to time delta
            const msPerPixel = totalVisibleMs / graphW; 
            
            xOffsetMs -= dx * msPerPixel;
            
            const maxOffsetMs = hwTimebaseMs * 5;
            if (xOffsetMs < -maxOffsetMs) xOffsetMs = -maxOffsetMs;
            if (xOffsetMs > maxOffsetMs - totalVisibleMs) xOffsetMs = maxOffsetMs - totalVisibleMs;
            
            hoverPos = null; // Hide hover while dragging
            requestRedraw();
        } else {
            // Track hover when not dragging
            hoverPos = { x: clientX };
            requestRedraw();
        }
    });

    canvas.addEventListener('mouseleave', () => {
        hoverPos = null;
        requestRedraw();
    });

    let redrawPending = false;
    function requestRedraw() {
        if (!redrawPending) {
            redrawPending = true;
            requestAnimationFrame(() => {
                forceRedraw();
                redrawPending = false;
            });
        }
    }

    // Main render loop for the oscilloscope canvas
    function forceRedraw() {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        
        const margin = { left: 50, right: 20, top: 20, bottom: 20 };
        const graphW = canvas.width - margin.left - margin.right;
        const graphH = canvas.height - margin.top - margin.bottom;
        
        if (graphW <= 0 || graphH <= 0) return;

        const hwTimebaseMs = frameTimebaseMs;
        const totalHardwareMs = hwTimebaseMs * 10;

        drawGraticule(margin, graphW, graphH, visualTimebaseMs);

        // Render active waveform traces and tooltip
        if (lastFrameActive.length > 0) {
            ctx.save();
            ctx.beginPath();
            ctx.rect(margin.left, margin.top, graphW, graphH);
            ctx.clip(); // Restrict drawing to graph area

            lastFrameActive.forEach(ch => {
                renderScopeFrame(lastFrameSamples[ch], ch, margin, graphW, graphH, totalHardwareMs);
            });

            ctx.restore();

            // Show tooltip on hover - displays time and voltage at cursor
            if (hoverPos && hoverPos.x > margin.left && hoverPos.x < canvas.width - margin.right) {
                const totalVisibleMs = visualTimebaseMs * 10;
                const msPerSample = totalHardwareMs / (lastFrameSamples[lastFrameActive[0]]?.length || 1);
                
                // Map mouse X to time
                const mouseTimeMs = xOffsetMs + ((hoverPos.x - margin.left) / graphW) * totalVisibleMs;
                const captureStartMs = -(totalHardwareMs / 2);
                
                // Find the nearest sample index in the buffer
                let sampleIdx = Math.round((mouseTimeMs - captureStartMs) / msPerSample);
                
                if (sampleIdx >= 0 && sampleIdx < lastFrameSamples[lastFrameActive[0]].length) {
                    // Draw vertical crosshair
                    ctx.strokeStyle = 'rgba(255, 255, 255, 0.4)';
                    ctx.beginPath(); 
                    ctx.moveTo(hoverPos.x, margin.top); 
                    ctx.lineTo(hoverPos.x, margin.top + graphH); 
                    ctx.stroke();

                    // Setup tooltip box
                    const boxW = 85;
                    const boxH = 20 + (lastFrameActive.length * 15);
                    const boxX = (hoverPos.x + 15 + boxW > canvas.width - margin.right) ? hoverPos.x - boxW - 10 : hoverPos.x + 10;
                    
                    ctx.fillStyle = 'rgba(34, 34, 38, 0.9)';
                    ctx.strokeStyle = '#555';
                    ctx.fillRect(boxX, margin.top + 10, boxW, boxH);
                    ctx.strokeRect(boxX, margin.top + 10, boxW, boxH);
                    
                    ctx.fillStyle = '#fff';
                    ctx.font = "11px sans-serif";
                    ctx.textAlign = "left";
                    ctx.textBaseline = "top";
                    
                    // Format time cleanly
                    const timeLabel = Math.abs(mouseTimeMs) < 1 ? `${(mouseTimeMs * 1000).toFixed(0)} µs` : `${mouseTimeMs.toFixed(2)} ms`;
                    ctx.fillText(`T: ${timeLabel}`, boxX + 8, margin.top + 15);
                    
                    let yOffset = margin.top + 31;
                    lastFrameActive.forEach(ch => {
                        const raw = lastFrameSamples[ch][sampleIdx];
                        const voltage = (raw / 4095) * ADC_REF_V;
                        ctx.fillStyle = scopeColors[`A${ch + 1}`];
                        ctx.fillText(`A${ch + 1}: ${voltage.toFixed(2)} V`, boxX + 8, yOffset);
                        yOffset += 15;
                    });
                }
            }
        }
    }

    // Bind real-time config updates to form controls
    const controlsToWatch = containerDOM.querySelectorAll('.ch-scale, .ch-offset, .timebase, .trig-level, .trig-src');
    controlsToWatch.forEach(ctrl => {
        ctrl.addEventListener('input', () => {
            if (ctrl.classList.contains('timebase')) {
                if (lastFrameActive.length === 0) {
                    visualTimebaseMs = parseFloat(ctrl.value);
                    frameTimebaseMs = visualTimebaseMs;
                    xOffsetMs = -(visualTimebaseMs * 5);
                }
            }
            requestRedraw();
        }); 
        ctrl.addEventListener('change', () => { sendScopeConfig(isRunning ? 1 : 0); });
    });

    // Draws a single channel's waveform trace
    function renderScopeFrame(samples, channelIndex, margin, graphW, graphH, totalHardwareMs) {
        const chName = `A${channelIndex+1}`;
        const card = containerDOM.querySelector(`.scope-ch-card[data-ch="${chName}"]`);
        if (card.classList.contains('disabled')) return;

        // Get per-channel voltage scaling and offset
        const scale = parseFloat(card.querySelector('.ch-scale').value); 
        const offset = parseFloat(card.querySelector('.ch-offset').value); 

        ctx.beginPath();
        ctx.strokeStyle = scopeColors[chName];
        ctx.lineWidth = 1.5;

        const bottomY = margin.top + graphH;
        const totalVoltageVisible = scale * 8; 

        const totalVisibleMs = visualTimebaseMs * 10;
        
        // Offset relative to the trigger point (which is in the middle of the hardware capture)
        const captureStartMs = -(totalHardwareMs / 2);
        
        // Calculate the start and end sample indices based on the current view
        const msPerSample = totalHardwareMs / samples.length;
        const startViewMs = xOffsetMs;
        const endViewMs = xOffsetMs + totalVisibleMs;

        const startSampleIdx = Math.max(0, Math.floor((startViewMs - captureStartMs) / msPerSample));
        const endSampleIdx = Math.min(samples.length, Math.ceil((endViewMs - captureStartMs) / msPerSample));

        for (let i = startSampleIdx; i < endSampleIdx; i++) {
            const voltage = (samples[i] / 4095) * ADC_REF_V;
            const sampleTimeMs = captureStartMs + (i * msPerSample);
            
            // Map the sample's time to the screen pixel
            const x = margin.left + ((sampleTimeMs - xOffsetMs) / totalVisibleMs) * graphW;
            
            const vEffective = voltage + offset; 
            const yOffsetPx = (vEffective / totalVoltageVisible) * graphH;
            const y = bottomY - yOffsetPx; 

            if (i === startSampleIdx) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }
        ctx.stroke();
    }

    // Renders the background grid, axis labels, and trigger markers
    function drawGraticule(margin, graphW, graphH, visualTbMs) {
        const hDivs = 10;
        const vDivs = 8;
        const dx = graphW / hDivs;
        const dy = graphH / vDivs;

        // Draw the outer box and grid lines
        ctx.strokeStyle = '#555';
        ctx.lineWidth = 1;
        ctx.strokeRect(margin.left, margin.top, graphW, graphH);

        ctx.strokeStyle = '#3c3c4a'; 
        ctx.beginPath();
        for (let i = 1; i < hDivs; i++) {
            let x = Math.round(margin.left + i * dx) + 0.5;
            ctx.moveTo(x, margin.top); ctx.lineTo(x, margin.top + graphH);
        }
        for (let i = 1; i < vDivs; i++) {
            let y = Math.round(margin.top + i * dy) + 0.5;
            ctx.moveTo(margin.left, y); ctx.lineTo(margin.left + graphW, y);
        }
        ctx.stroke();

        // Draw Y-axis labels (voltage)
        ctx.fillStyle = '#a0a0ab';
        ctx.font = "10px sans-serif";
        ctx.textAlign = "right"; 
        ctx.textBaseline = "middle";
        
        const trigCh = containerDOM.querySelector('.trig-src').value;
        const trigCard = containerDOM.querySelector(`.scope-ch-card[data-ch="${trigCh}"]`);
        let trigScale = 1.0, trigOffset = 0;
        if (trigCard && !trigCard.classList.contains('disabled')) {
            trigScale = parseFloat(trigCard.querySelector('.ch-scale').value);
            trigOffset = parseFloat(trigCard.querySelector('.ch-offset').value);
        }

        // Y-axis labels based on trigger channel scale
        for (let i = 0; i <= vDivs; i++) {
            let y = margin.top + Math.round(i * dy);
            let divsFromBottom = vDivs - i;
            let volts = (divsFromBottom * trigScale) - trigOffset; 
            if (Math.abs(volts) < 0.001) volts = 0; 
            ctx.fillText(volts.toFixed(1) + "V", margin.left - 18, y); 
        }

        // X-axis labels (time, relative to trigger)
        ctx.textAlign = "center"; 
        ctx.textBaseline = "top";
        
        for (let i = 0; i <= hDivs; i++) {
            let x = margin.left + Math.round(i * dx);
            let timeMs = xOffsetMs + (i * visualTbMs);
            
            let label = "";
            if (Math.abs(timeMs) < 0.001) {
                label = '0';
            } else {
                let absTime = Math.abs(timeMs);
                let sign = timeMs < 0 ? '-' : '+';
                label = absTime < 1 ? sign + Math.round(absTime * 1000) + 'µs' : sign + absTime.toFixed(1) + 'ms';
            }
            ctx.fillText(label, x, margin.top + graphH + 6);
        }

        // Draw Trigger Arrow - shows where the trigger point is on the timeline
        const totalVisibleMs = visualTbMs * 10;
        const trigXPx = margin.left + ((0 - xOffsetMs) / totalVisibleMs) * graphW;

        if (trigXPx >= margin.left && trigXPx <= margin.left + graphW) {
            if (scopeColors[trigCh]) {
                ctx.fillStyle = scopeColors[trigCh];
                ctx.beginPath();
                ctx.moveTo(trigXPx - 5, margin.top - 6);
                ctx.lineTo(trigXPx + 5, margin.top - 6);
                ctx.lineTo(trigXPx, margin.top + 2);
                ctx.fill();
            }
        }

        // Draw Channel Indicators and Trigger Level Line
        for (let chIdx = 0; chIdx < 4; chIdx++) {
            const chName = `A${chIdx+1}`;
            const card = containerDOM.querySelector(`.scope-ch-card[data-ch="${chName}"]`);
            if (!card || card.classList.contains('disabled')) continue;
            
            const scale = parseFloat(card.querySelector('.ch-scale').value);
            const offset = parseFloat(card.querySelector('.ch-offset').value);
            const totalVisible = scale * 8;
            
            const vEffective = 0 + offset;
            const yOffsetPx = (vEffective / totalVisible) * graphH;
            const y = (margin.top + graphH) - yOffsetPx;

            if (y >= margin.top && y <= margin.top + graphH) {
                ctx.fillStyle = scopeColors[chName];
                ctx.beginPath();
                ctx.moveTo(margin.left - 1, y);
                ctx.lineTo(margin.left - 6, y - 4);
                ctx.lineTo(margin.left - 14, y - 4);
                ctx.lineTo(margin.left - 14, y + 4);
                ctx.lineTo(margin.left - 6, y + 4);
                ctx.fill();
            }
        }

        // Draw Trigger Level Line - dashed line where the trigger fires
        const trigLevel = parseFloat(containerDOM.querySelector('.trig-level').value);
        const trigEffective = trigLevel + trigOffset;
        const totalVoltageVisible = trigScale * 8;
        const trigYPx = (margin.top + graphH) - (trigEffective / totalVoltageVisible) * graphH;

        if (trigYPx >= margin.top && trigYPx <= margin.top + graphH) {
            ctx.beginPath();
            ctx.strokeStyle = scopeColors[trigCh];
            ctx.setLineDash([4, 4]); 
            ctx.moveTo(margin.left, trigYPx);
            ctx.lineTo(margin.left + graphW, trigYPx);
            ctx.stroke();
            ctx.setLineDash([]); 
            
            // Draw trigger level indicator arrow on the left
            ctx.fillStyle = scopeColors[trigCh];
            ctx.beginPath();
            ctx.moveTo(margin.left - 1, trigYPx);
            ctx.lineTo(margin.left - 6, trigYPx - 5);
            ctx.lineTo(margin.left - 14, trigYPx - 5);
            ctx.lineTo(margin.left - 14, trigYPx + 5);
            ctx.lineTo(margin.left - 6, trigYPx + 5);
            ctx.fill();

            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 1;
            ctx.stroke();
        }
    }

    // Bind UI clicks to toggle individual channel cards
    chToggles.forEach(btn => {
        btn.addEventListener('click', (e) => {
            const card = e.target.closest('.scope-ch-card');
            const isActive = e.target.classList.toggle('active');
            
            const selects = card.querySelectorAll('select');
            const inputs = card.querySelectorAll('input');
            
            if (isActive) {
                card.classList.remove('disabled');
                selects.forEach(s => s.disabled = false);
                inputs.forEach(i => i.disabled = false);
            } else {
                card.classList.add('disabled');
                selects.forEach(s => s.disabled = true);
                inputs.forEach(i => i.disabled = true);
            }
            forceRedraw();
            sendScopeConfig(isRunning ? 1 : 0);
        });
    });

    // Toggle trigger edge (rising/falling)
    trigEdgeBtn.addEventListener('click', () => {
        trigEdgeRising = !trigEdgeRising;
        if (trigEdgeRising) {
            trigEdgeBtn.innerHTML = '<svg viewBox="0 0 24 24" width="18" height="18" stroke="currentColor" fill="none" stroke-width="2"><path d="M4 16h6l4-8h6" stroke-linejoin="round"/></svg>';
        } else {
            trigEdgeBtn.innerHTML = '<svg viewBox="0 0 24 24" width="18" height="18" stroke="currentColor" fill="none" stroke-width="2"><path d="M4 8h6l4 8h6" stroke-linejoin="round"/></svg>';
        }
        sendScopeConfig(isRunning ? 1 : 0);
    });

    // Handle trigger mode switching (Auto / Normal / Single)
    modeBtns.forEach(btn => {
        btn.addEventListener('click', (e) => {
            modeBtns.forEach(b => b.classList.remove('active'));
            e.target.classList.add('active');
            sendScopeConfig(isRunning ? 1 : 0);
        });
    });
}

// Logic Analyzer app - captures GPIO state transitions at high speed
// Displays digital waveforms with RLE decompression, pan/zoom and decoding
function initLogicAnalyzer(containerDOM) {
    const canvas = containerDOM.querySelector('#logic-canvas');
    const ctx = canvas.getContext('2d');
    
    const btnRunStop = containerDOM.querySelector('.btn-run-stop');
    const pinBtns = containerDOM.querySelectorAll('.pin-select-btn');
    const trigEdgeBtn = containerDOM.querySelector('.trig-edge');
    const trigSrcSelect = containerDOM.querySelector('.trig-src');
    const sampleRateSelect = containerDOM.querySelector('.sample-rate');
    let displayedSampleRate = parseFloat(sampleRateSelect.value);
    const decoderToggle = containerDOM.querySelector('.decoder-toggle');
    const decoderMenu = containerDOM.querySelector('.decoder-menu');
    const decoderApply = containerDOM.querySelector('.decoder-apply');

    let trigEdgeRising = true;
    let xOffsetSamples = 0;
    let activeChannels = [0, 1]; 
    let rawLogicData = null;
    let decodedFrames = [];

    const decoderConfig = {
        protocol: 'off',
        sck: 0, mosi: 1, miso: 2, cs: 0, spiMode: 0,
        scl: 1, sda: 0, rx: 0, baud: 115200
    };

    function decoderSelect(name) {
        return decoderMenu.querySelector(`.decoder-${name}`);
    }

    function syncDecoderMenu() {
        decoderMenu.classList.remove('spi', 'i2c', 'uart');
        const protocol = decoderSelect('protocol').value;
        if (protocol !== 'off') decoderMenu.classList.add(protocol);
    }

    decoderToggle.addEventListener('click', (event) => {
        event.stopPropagation();
        decoderMenu.classList.toggle('show');
        syncDecoderMenu();
    });

    decoderMenu.addEventListener('click', event => event.stopPropagation());
    document.addEventListener('click', () => decoderMenu.classList.remove('show'));
    decoderSelect('protocol').addEventListener('change', syncDecoderMenu);

    function readDecoderConfig() {
        ['protocol', 'sck', 'mosi', 'miso', 'cs', 'spiMode', 'scl', 'sda', 'rx', 'baud'].forEach(name => {
            const select = decoderSelect(name);
            if (select) decoderConfig[name] = name === 'protocol' ? select.value : Number(select.value);
        });
    }

    function byteLabel(value) {
        const ascii = value >= 32 && value <= 126 ? ` '${String.fromCharCode(value)}'` : '';
        return `0x${value.toString(16).padStart(2, '0').toUpperCase()}${ascii}`;
    }

    function decodeSpi(samples, rate) {
        const { sck, mosi, miso, cs, spiMode } = decoderConfig;
        const cpol = Math.floor(spiMode / 2) === 1;
        const cpha = spiMode % 2;
        const sampleRising = cpha === 0 ? !cpol : cpol;
        const frames = [];
        // deal with CS being asserted in the first stored sample
        let active = cs < 0 || ((samples[0] >> cs) & 1) === 0;
        let bitsMosi = [], bitsMiso = [], byteStart = 0;

        const emit = end => {
            if (bitsMosi.length !== 8) return;
            const makeByte = bits => bits.reduce((value, bit) => (value << 1) | bit, 0);
            const mosiByte = makeByte(bitsMosi);
            const misoByte = makeByte(bitsMiso);
            const label = miso === mosi ? `MOSI ${byteLabel(mosiByte)}` : `MOSI ${byteLabel(mosiByte)}  MISO ${byteLabel(misoByte)}`;
            frames.push({ start: byteStart, end, label, color: '#ffcc00' });
            bitsMosi = [];
            bitsMiso = [];
        };

        for (let i = 1; i < samples.length; i++) {
            if (cs >= 0) {
                if (!active && samples[i - 1] >> cs & 1 && !(samples[i] >> cs & 1)) {
                    active = true;
                    bitsMosi = [];
                    bitsMiso = [];
                } else if (active && !(samples[i - 1] >> cs & 1) && samples[i] >> cs & 1) {
                    emit(i);
                    active = false;
                }
            }
            if (!active) continue;

            const previousClock = (samples[i - 1] >> sck) & 1;
            const currentClock = (samples[i] >> sck) & 1;
            const rising = !previousClock && currentClock;
            const falling = previousClock && !currentClock;
            if ((sampleRising && rising) || (!sampleRising && falling)) {
                if (bitsMosi.length === 0) byteStart = i;
                bitsMosi.push((samples[i] >> mosi) & 1);
                bitsMiso.push((samples[i] >> miso) & 1);
                if (bitsMosi.length === 8) emit(i);
            }
        }
        emit(samples.length - 1);
        return frames;
    }

    function decodeI2c(samples) {
        const { scl, sda } = decoderConfig;
        const frames = [];
        // deal with start edge falling just before sample zero
        let active = ((samples[0] >> sda) & 1) === 0;
        let bitCount = 0;
        let value = 0;
        let byteStart = 0;
        let pendingFrame = null;
        let expectingAddress = active;

        for (let i = 1; i < samples.length; i++) {
            const previousScl = (samples[i - 1] >> scl) & 1;
            const currentScl = (samples[i] >> scl) & 1;
            const previousSda = (samples[i - 1] >> sda) & 1;
            const currentSda = (samples[i] >> sda) & 1;

            if (previousScl && currentScl && previousSda && !currentSda) {
                active = true;
                bitCount = 0;
                value = 0;
                byteStart = i;
                pendingFrame = null;
                expectingAddress = true;
            } else if (active && previousScl && currentScl && !previousSda && currentSda) {
                active = false;
            }

            if (active && !previousScl && currentScl) {
                if (bitCount < 8) {
                    if (bitCount === 0) byteStart = i;
                    value = (value << 1) | currentSda;
                    bitCount++;
                    if (bitCount === 8) {
                        const label = expectingAddress
                            ? `${value & 1 ? 'Read' : 'Write'} 0x${(value >> 1).toString(16).padStart(2, '0').toUpperCase()}`
                            : byteLabel(value);
                        pendingFrame = { start: byteStart, end: i, label, color: '#05d9e8' };
                        frames.push(pendingFrame);
                        expectingAddress = false;
                    }
                } else {
                    if (pendingFrame) {
                        pendingFrame.end = i;
                        pendingFrame.label += currentSda ? ' + NAK' : ' + ACK';
                    }
                    pendingFrame = null;
                    bitCount = 0;
                    value = 0;
                }
            }
        }
        return frames;
    }

    function decodeUart(samples, rate) {
        const { rx, baud } = decoderConfig;
        const samplesPerBit = rate / baud;
        const frames = [];
        if (!Number.isFinite(samplesPerBit) || samplesPerBit < 2) return frames;

        // Treat the trigger boundary as an implicit start edge.
        const firstSampleIsStart = !trigEdgeRising && ((samples[0] >> rx) & 1) === 0;

        for (let start = 0; start < samples.length - samplesPerBit * 10; start++) {
            const isBoundaryStart = start === 0 && firstSampleIsStart;
            if (!isBoundaryStart) {
                const previous = (samples[start - 1] >> rx) & 1;
                const current = (samples[start] >> rx) & 1;
                if (previous !== 1 || current !== 0) continue;

                const idleSample = Math.round(start - samplesPerBit * 0.75);
                if (((samples[idleSample] >> rx) & 1) !== 1) continue;
            }

            const sampleBit = bit => Math.min(samples.length - 1, Math.round(start + (bit + 0.5) * samplesPerBit));
            if (((samples[sampleBit(0)] >> rx) & 1) !== 0) continue;
            let value = 0;
            for (let bit = 0; bit < 8; bit++) value |= ((samples[sampleBit(bit + 1)] >> rx) & 1) << bit;
            const stopIndex = sampleBit(9);
            if (((samples[stopIndex] >> rx) & 1) !== 1) continue;

            frames.push({ start, end: Math.min(samples.length - 1, stopIndex), label: byteLabel(value), color: '#ff9500' });
            start = stopIndex;
        }
        return frames;
    }

    function decodeCapture() {
        decodedFrames = [];
        if (!rawLogicData || decoderConfig.protocol === 'off') return;
        const rate = displayedSampleRate;
        if (decoderConfig.protocol === 'spi') decodedFrames = decodeSpi(rawLogicData, rate);
        if (decoderConfig.protocol === 'i2c') decodedFrames = decodeI2c(rawLogicData);
        if (decoderConfig.protocol === 'uart') decodedFrames = decodeUart(rawLogicData, rate);
    }

    decoderApply.addEventListener('click', () => {
        readDecoderConfig();
        syncDecoderMenu();
        decodeCapture();
        decoderMenu.classList.remove('show');
        forceRedraw();
    });

    // Compute duration per horizontal division (10 divisions across screen)
    function getFullScaleTimebaseUs(rate = displayedSampleRate) {
        const totalUs = (NUM_SAMPLES / rate) * 1000000;
        return totalUs / 10.0;
    }

    let visualTimebaseUs = getFullScaleTimebaseUs();

    // Dynamically format option labels: e.g. "10 MS/s (8 ms)"
    function updateSampleRateLabels() {
        Array.from(sampleRateSelect.options).forEach(opt => {
            const rate = parseFloat(opt.value);
            const totalSec = NUM_SAMPLES / rate;

            let rateStr = rate >= 1000000 ? `${rate / 1000000} MS/s` : `${rate / 1000} kS/s`;
            let durStr = totalSec < 0.001 
                ? `${Math.round(totalSec * 1000000)} µs` 
                : totalSec < 1 
                    ? `${(totalSec * 1000).toFixed(totalSec * 1000 >= 10 ? 0 : 1)} ms` 
                    : `${totalSec.toFixed(2)} s`;

            opt.text = `${rateStr} (${durStr})`;
        });
    }

    updateSampleRateLabels();

    window.addEventListener('pico-sysinfo', (e) => {
        const info = e.detail || {};
        NUM_SAMPLES = (info.chip && info.chip.includes('RP2040')) ? 40000 : 80000;
        updateSampleRateLabels();
        if (!rawLogicData) visualTimebaseUs = getFullScaleTimebaseUs();
        forceRedraw();
    });
    
    // Digital channel color palette
    const colors = [
        '#ff9500', '#ff2a6d', '#af52de', '#05d9e8', 
        '#cba6f7', '#5856d6', '#d1f534', '#ff8a65'
    ];

    let configTimeout = null;
    let configQueue = Promise.resolve();
    let logicGeneration = 0;

    // Push configuration to the backend before arming
    function sendLogicConfig(runState) {
        if (configTimeout) clearTimeout(configTimeout);
        const generation = ++logicGeneration;
        configTimeout = setTimeout(() => {
            const trigPin = trigSrcSelect.value;
            const edge = trigEdgeRising ? 1 : 0;
            const rate = sampleRateSelect.value;
            const url = `/logic?run=${runState}&trig_pin=${trigPin}&trig_edge=${edge}&rate=${rate}&gen=${generation}`;

            configQueue = configQueue
                .then(() => fetch(url))
                .catch(err => console.error("Logic Config Error:", err));
            configTimeout = null;
        }, 100);
    }

    // Start or cancel a single-shot logic-analyzer capture
    btnRunStop.addEventListener('click', () => {
        const isRunning = btnRunStop.classList.contains('running');
        if (isRunning) {
            btnRunStop.classList.replace('running', 'stopped');
            btnRunStop.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" style="vertical-align: middle; margin-right: 4px;"><path fill="currentColor" d="M8 5v14l11-7z"/></svg> Run';
            sendLogicConfig(0);
        } else {
            btnRunStop.classList.replace('stopped', 'running');
            btnRunStop.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" style="vertical-align: middle; margin-right: 4px;"><path fill="currentColor" d="M6 6h12v12H6z"/></svg> Stop';
            sendLogicConfig(1);
        }
    });

    // Auto-scale to display the complete recorded buffer
    sampleRateSelect.addEventListener('change', () => {
        if (!rawLogicData) {
            displayedSampleRate = parseFloat(sampleRateSelect.value);
            visualTimebaseUs = getFullScaleTimebaseUs(displayedSampleRate);
            xOffsetSamples = 0;
            forceRedraw();
        }
        sendLogicConfig(btnRunStop.classList.contains('running') ? 1 : 0);
    });

    trigSrcSelect.addEventListener('change', () => {
        sendLogicConfig(btnRunStop.classList.contains('running') ? 1 : 0);
    });

    // Process incoming logic analyzer capture chunks
    window.addEventListener('pico-logic-rx', (e) => {
        if (!document.body.contains(containerDOM)) return;
        
        const dataPayload = e.detail;
        if (dataPayload.generation !== undefined && dataPayload.generation !== logicGeneration) return;

        const captureRate = Number(dataPayload.rate);
        displayedSampleRate = Number.isFinite(captureRate) && captureRate > 0
            ? captureRate
            : parseFloat(sampleRateSelect.value);
        
        // 1. Base64 decode to get the compressed bytes
        const binaryString = atob(dataPayload.data);
        const compressedBytes = new Uint8Array(binaryString.length);
        for (let i = 0; i < binaryString.length; i++) {
            compressedBytes[i] = binaryString.charCodeAt(i);
        }
        
        // 2. RLE Decompression - expand [value, count] pairs back to full sample buffer
        const decompressed = new Uint8Array(NUM_SAMPLES);
        let writeIndex = 0;
        
        for (let i = 0; i < compressedBytes.length; i += 2) {
            const val = compressedBytes[i];
            const count = compressedBytes[i + 1];
            
            // Expand the compressed block
            for (let j = 0; j < count; j++) {
                if (writeIndex < NUM_SAMPLES) {
                    decompressed[writeIndex++] = val;
                }
            }
        }
        
        rawLogicData = decompressed;
        decodeCapture();
        btnRunStop.classList.replace('running', 'stopped');
        btnRunStop.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" style="vertical-align: middle; margin-right: 4px;"><path fill="currentColor" d="M8 5v14l11-7z"/></svg> Run';

        // Auto-scale display across the 10 screen divisions
        visualTimebaseUs = getFullScaleTimebaseUs(displayedSampleRate);
        xOffsetSamples = 0; 
        forceRedraw();
    });

    // Toggle visibility of specific digital pins
    pinBtns.forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.target.classList.toggle('active');
            updateActiveChannels();
            forceRedraw();
        });
    });

    // Keep active channel list synced with UI buttons
    function updateActiveChannels() {
        activeChannels = [];
        pinBtns.forEach(btn => {
            if (btn.classList.contains('active')) {
                activeChannels.push(parseInt(btn.dataset.pin.replace('D', '')) - 1);
            }
        });
    }

    // Toggle logic trigger edge (rising/falling)
    trigEdgeBtn.addEventListener('click', () => {
        trigEdgeRising = !trigEdgeRising;
        trigEdgeBtn.innerHTML = trigEdgeRising 
            ? '<svg viewBox="0 0 24 24" width="18" height="18" stroke="currentColor" fill="none" stroke-width="2"><path d="M4 16h6l4-8h6" stroke-linejoin="round"/></svg>'
            : '<svg viewBox="0 0 24 24" width="18" height="18" stroke="currentColor" fill="none" stroke-width="2"><path d="M4 8h6l4 8h6" stroke-linejoin="round"/></svg>';
        sendLogicConfig(btnRunStop.classList.contains('running') ? 1 : 0);
    });

    // Pan / Zoom handlers
    let isDragging = false;
    let lastMouseX = 0;

    // Handle scroll wheel zooming on the digital waveform
    canvas.addEventListener('wheel', (e) => {
        e.preventDefault(); 
        if (!rawLogicData || rawLogicData.length === 0) return;

        const rate = displayedSampleRate;
        const hwUsPerSample = 1000000 / rate;
        const hwTimebaseUs = (NUM_SAMPLES * hwUsPerSample) / 10;
        
        // Calculate mouse position relative to the graph area
        const rect = canvas.getBoundingClientRect();
        const margin = { left: 50, right: 20 };
        const graphW = canvas.width - margin.left - margin.right;
        
        let mouseX = e.clientX - rect.left - margin.left;
        
        // Clamp mouseX to graph area so zooming outside the grid bounds doesn't throw the view
        if (mouseX < 0) mouseX = 0;
        if (mouseX > graphW) mouseX = graphW;
        
        // Find the exact sample under the mouse pointer
        const mouseFraction = mouseX / graphW;
        const currentVisibleSamples = (visualTimebaseUs * 10) / hwUsPerSample;
        const sampleUnderMouse = xOffsetSamples + (mouseFraction * currentVisibleSamples);

        // Apply zoom factor
        if (e.deltaY < 0) {
            visualTimebaseUs /= 1.2; // Zoom in
        } else {
            visualTimebaseUs *= 1.2; // Zoom out
        }

        // Clamp zoom (from 1000x zoom up to the full hardware capture width)
        const maxTb = hwTimebaseUs; 
        const minTb = hwTimebaseUs / 1000; 
        if (visualTimebaseUs > maxTb) visualTimebaseUs = maxTb;
        if (visualTimebaseUs < minTb) visualTimebaseUs = minTb;

        // Recalculate offset to keep the hovered sample locked in place
        const newVisibleSamples = (visualTimebaseUs * 10) / hwUsPerSample;
        xOffsetSamples = sampleUnderMouse - (mouseFraction * newVisibleSamples);
        
        // Clamp panning bounds
        if (xOffsetSamples < 0) xOffsetSamples = 0;
        if (xOffsetSamples > NUM_SAMPLES - newVisibleSamples) {
            xOffsetSamples = NUM_SAMPLES - newVisibleSamples;
        }

        forceRedraw();
    });

    canvas.addEventListener('mousedown', (e) => {
        isDragging = true;
        lastMouseX = e.clientX;
        canvas.style.cursor = 'grabbing';
    });

    window.addEventListener('mouseup', () => {
        isDragging = false;
        canvas.style.cursor = 'crosshair';
    });

    // Support horizontal drag-to-pan functionality
    canvas.addEventListener('mousemove', (e) => {
        if (isDragging) {
            const dx = e.clientX - lastMouseX;
            lastMouseX = e.clientX;
            
            const rate = displayedSampleRate;
            const hwUsPerSample = 1000000 / rate;
            const visibleSamples = (visualTimebaseUs * 10) / hwUsPerSample;
            
            const graphW = canvas.width - 70; 
            // Pan by converting mouse delta to sample delta
            const samplesPerPixel = visibleSamples / graphW;
            
            xOffsetSamples -= dx * samplesPerPixel;
            
            if (xOffsetSamples < 0) xOffsetSamples = 0;
            if (xOffsetSamples > NUM_SAMPLES - visibleSamples) {
                xOffsetSamples = NUM_SAMPLES - visibleSamples;
            }
            
            forceRedraw();
        }
    });

    // Auto-resize logic analyzer canvas
    new ResizeObserver(() => {
        canvas.width = canvas.parentElement.clientWidth;
        canvas.height = canvas.parentElement.clientHeight;
        forceRedraw();
    }).observe(canvas.parentElement);

    // Primary render loop for digital signal visualization
    function drawDecodedFrames(margin, graphW, graphH, visibleSamples) {
        if (decodedFrames.length === 0) return;

        const visibleStart = xOffsetSamples;
        const visibleEnd = xOffsetSamples + visibleSamples;
        const labelHeight = 18;
        const labelY = 2;
        ctx.font = '10px sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';

        decodedFrames.forEach(frame => {
            if (frame.end < visibleStart || frame.start > visibleEnd) return;
            const startX = margin.left + ((frame.start - xOffsetSamples) / visibleSamples) * graphW;
            const endX = margin.left + ((frame.end - xOffsetSamples) / visibleSamples) * graphW;
            const left = Math.max(margin.left, startX);
            const right = Math.min(margin.left + graphW, Math.max(startX + 18, endX));
            if (right <= left) return;

            ctx.fillStyle = '#ffeb3b';
            ctx.fillRect(left, labelY, right - left, labelHeight);
            const availableTextWidth = right - left - 8;
            let visibleLabel = '';
            for (const character of frame.label) {
                const candidate = visibleLabel + character;
                if (ctx.measureText(candidate).width > availableTextWidth) break;
                visibleLabel = candidate;
            }
            if (visibleLabel) {
                ctx.fillStyle = '#000000';
                ctx.fillText(visibleLabel, (left + right) / 2, labelY + labelHeight / 2);
            }
        });
    }

    function forceRedraw() {
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        
        const margin = { left: 50, right: 20, top: 20, bottom: 30 };
        const graphW = canvas.width - margin.left - margin.right;
        const graphH = canvas.height - margin.top - margin.bottom;
        
        if (graphW <= 0 || graphH <= 0 || activeChannels.length === 0) return;

        const rate = displayedSampleRate;
        const hwUsPerSample = 1000000 / rate;
        const hwTimebaseUs = (NUM_SAMPLES * hwUsPerSample) / 10;
        const visibleSamples = (visualTimebaseUs * 10) / hwUsPerSample;

        drawGraticule(margin, graphW, graphH, visualTimebaseUs, hwTimebaseUs, hwUsPerSample, visibleSamples);

        if (rawLogicData && rawLogicData.length > 0) {
            const laneH = graphH / activeChannels.length;

            ctx.save();
            ctx.beginPath();
            ctx.rect(margin.left, margin.top, graphW, graphH);
            ctx.clip();

            // Render each active digital lane
            activeChannels.forEach((chIndex, laneIndex) => {
                const gap = laneH * 0.2; 
                const baseY = margin.top + (laneIndex + 1) * laneH - gap; 
                const highY = margin.top + laneIndex * laneH + gap;       
                
                ctx.beginPath();
                ctx.strokeStyle = colors[chIndex];
                ctx.lineWidth = 1.5;
                
                // Calculate which slice of the 80,000 array is currently on screen
                const startSample = Math.max(0, Math.floor(xOffsetSamples));
                const endSample = Math.min(rawLogicData.length, Math.ceil(xOffsetSamples + visibleSamples));

                for (let i = startSample; i < endSample; i++) {
                    const byteVal = rawLogicData[i];
                    const val = (byteVal >> chIndex) & 1;

                    const x = margin.left + ((i - xOffsetSamples) / visibleSamples) * graphW;
                    const y = val === 1 ? highY : baseY;

                    if (i === startSample) {
                        ctx.moveTo(x, y);
                    } else {
                        const prevVal = (rawLogicData[i - 1] >> chIndex) & 1;
                        if (val !== prevVal) {
                            // Draw a sharp vertical edge when state changes
                            ctx.lineTo(x, prevVal === 1 ? highY : baseY);
                        }
                        ctx.lineTo(x, y);
                    }
                }
                ctx.stroke();

                // Draw channel labels on the left side
                ctx.restore(); 
                ctx.fillStyle = colors[chIndex];
                ctx.font = "11px sans-serif";
                ctx.textAlign = "right";
                ctx.textBaseline = "middle";
                ctx.fillText(`D${chIndex + 1}`, margin.left - 8, margin.top + (laneIndex + 0.5) * laneH);
                ctx.save(); 
                ctx.beginPath();
                ctx.rect(margin.left, margin.top, graphW, graphH);
                ctx.clip();
            });
            ctx.restore();

            drawDecodedFrames(margin, graphW, graphH, visibleSamples);
        }
    }

    // Draws the timing grid and labels
    function drawGraticule(margin, graphW, graphH, visualTbUs, hwTbUs, hwUsPerSample, visibleSamples) {
        const hDivs = 10;
        const dx = graphW / hDivs;

        // Draw vertical gridlines
        ctx.strokeStyle = '#3c3c4a'; 
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (let i = 0; i <= hDivs; i++) {
            let x = Math.round(margin.left + i * dx) + 0.5;
            ctx.moveTo(x, margin.top); 
            ctx.lineTo(x, margin.top + graphH);
        }
        ctx.stroke();

        ctx.fillStyle = '#a0a0ab';
        ctx.font = "10px sans-serif";
        ctx.textAlign = "center"; 
        ctx.textBaseline = "top";
        
        // X-axis labels (time relative to trigger)
        for (let i = 0; i <= hDivs; i++) {
            let x = margin.left + Math.round(i * dx);
            
            // Calculate absolute time based on hardware sample offset + visual zoom
            let timeValUs = (xOffsetSamples * hwUsPerSample) + (i * visualTbUs); 
            
            let label = timeValUs === 0 ? '0' : 
                        timeValUs >= 1000 ? `+${(timeValUs / 1000).toFixed(2)} ms` : 
                        `+${Math.round(timeValUs)} µs`;
                        
            ctx.fillText(label, x, margin.top + graphH + 6);
        }
    }
}

// Bus utilities - shared code for I2C and SPI master interfaces
// Handles request/response logging, payload formatting, and event synchronization

// Remove all non-hex characters and convert to uppercase
function cleanHexInput(str) {
    return (str || '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
}

// Ensure non-negative integer for input parameters
function parseNonNegativeInt(value, fallback = 1) {
    const parsed = parseInt(value, 10);
    if (!Number.isFinite(parsed)) return fallback;
    return Math.max(0, parsed);
}

// Format timestamp string for bus transaction logging
function nowTimeString() {
    return new Date().toLocaleTimeString();
}

// Builds a transaction log table for I2C/SPI operations
function createBusTableLogger(containerDOM, tbodySelector) {
    const tbody = containerDOM.querySelector(tbodySelector);
    const scrollHost = tbody;

    function addRow({ direction, operation, request, response, status }) {
        const tr = document.createElement('tr');
        const statusText = String(status || '-');
        const isOk = statusText.toLowerCase() === 'ok';
        tr.className = isOk ? 'bus-row-ok' : 'bus-row-error';
        tr.innerHTML = `
            <td>${nowTimeString()}</td>
            <td>${operation}</td>
            <td>${request || '-'}</td>
            <td>${response || '-'}</td>
            <td>${statusText}</td>
        `;
        tbody.appendChild(tr);
        if (scrollHost) scrollHost.scrollTop = scrollHost.scrollHeight;
    }

    function clear() {
        tbody.innerHTML = '';
    }

    return { addRow, clear };
}

// Convert raw JSON bus payload into clean display strings
function formatBusPayload(payload) {
    if (Array.isArray(payload.found)) {
        return payload.found.length > 0
            ? payload.found.map(v => `0x${Number(v).toString(16).toUpperCase()}`).join(' ')
            : 'none';
    }
    if (typeof payload.rx === 'string') return payload.rx;
    if (typeof payload.msg === 'string') return payload.msg;
    if (typeof payload.bytes === 'number') return `bytes=${payload.bytes}`;
    return JSON.stringify(payload);
}

// Wait for asynchronous SSE bus response event with a timeout
function waitForBusEvent(busType, containerDOM, timeoutMs = 1500) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            window.removeEventListener('pico-bus-rx', onBusEvent);
            reject(new Error('Bus response timeout'));
        }, timeoutMs);

        function onBusEvent(e) {
            if (!document.body.contains(containerDOM)) return;
            if (!e.detail || e.detail.bus !== busType) return;
            clearTimeout(timer);
            window.removeEventListener('pico-bus-rx', onBusEvent);
            resolve(e.detail);
        }

        window.addEventListener('pico-bus-rx', onBusEvent);
    });
}

// Perform GET request to the Pico bus API and log the outcome
async function requestBusWithEvent({ busType, url, operation, requestText, logger, containerDOM }) {
    try {
        const waitPromise = waitForBusEvent(busType, containerDOM);
        const response = await fetch(url);
        const bodyText = await response.text();
        if (!response.ok) {
            const errText = bodyText ? bodyText : `HTTP ${response.status}`;
            logger.addRow({
                direction: 'TX',
                operation,
                request: requestText,
                response: errText,
                status: 'error'
            });
            throw new Error(errText);
        }

        const payload = await waitPromise;
        logger.addRow({
            direction: 'TX/RX',
            operation,
            request: requestText,
            response: formatBusPayload(payload),
            status: payload.err === undefined ? 'ok' : `err ${payload.err}`
        });
        return payload;
    } catch (err) {
        logger.addRow({
            direction: 'TX/RX',
            operation,
            request: requestText,
            response: String(err),
            status: 'error'
        });
        throw err;
    }
}

// I2C Master app - configure baud rate, scan addresses, write/read device registers
function initI2CMaster(containerDOM) {
    const logger = createBusTableLogger(containerDOM, '#i2c-log-body');
    const baudSelect = containerDOM.querySelector('#i2c-baud');
    const rxLenInput = containerDOM.querySelector('#i2c-rx-len');
    let configuredBaud = null;
    let setupPromise = null;

    if (rxLenInput) {
        rxLenInput.addEventListener('input', () => {
            if (rxLenInput.value === '') return;
            rxLenInput.value = String(parseNonNegativeInt(rxLenInput.value, 0));
        });

        rxLenInput.addEventListener('change', () => {
            rxLenInput.value = String(parseNonNegativeInt(rxLenInput.value, 1));
        });
    }

    // Apply baud rate settings before transaction if needed
    async function ensureSetup(force = false) {
        const baud = baudSelect.value;
        if (!force && configuredBaud === baud) return;
        if (setupPromise) return setupPromise;

        setupPromise = requestBusWithEvent({
            busType: 'i2c',
            url: `/i2c?action=setup&baud=${baud}`,
            operation: 'setup',
            requestText: `baud=${baud}`,
            logger,
            containerDOM
        }).then(() => {
            configuredBaud = baud;
        }).finally(() => {
            setupPromise = null;
        });

        return setupPromise;
    }

    // Helper to gather configuration from the UI inputs
    function getParams() {
        return {
            addr: parseInt(cleanHexInput(containerDOM.querySelector('#i2c-addr').value), 16),
            tx: cleanHexInput(containerDOM.querySelector('#i2c-tx').value),
            rlen: parseNonNegativeInt(containerDOM.querySelector('#i2c-rx-len').value, 1)
        };
    }

    baudSelect.addEventListener('change', async () => {
        configuredBaud = null;
        await ensureSetup(true);
    });

    containerDOM.querySelector('#btn-i2c-clear-log').addEventListener('click', () => logger.clear());

    // Probe bus for active I2C addresses
    containerDOM.querySelector('#btn-i2c-scan').addEventListener('click', async () => {
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'i2c',
            url: '/i2c?action=scan',
            operation: 'scan',
            requestText: 'addr=0x08..0x77',
            logger,
            containerDOM
        });
    });

    // Execute standard I2C Write operation
    containerDOM.querySelector('#btn-i2c-write').addEventListener('click', async () => {
        const p = getParams();
        if (!Number.isInteger(p.addr)) {
            logger.addRow({ direction: 'TX', operation: 'write', request: '-', response: 'Invalid address', status: 'error' });
            return;
        }
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'i2c',
            url: `/i2c?action=write&addr=${p.addr}&data=${p.tx}`,
            operation: 'write',
            requestText: `addr=0x${p.addr.toString(16).toUpperCase()}, data=${p.tx || '(empty)'}`,
            logger,
            containerDOM
        });
    });

    // Execute standard I2C Read operation
    containerDOM.querySelector('#btn-i2c-read').addEventListener('click', async () => {
        const p = getParams();
        if (!Number.isInteger(p.addr)) {
            logger.addRow({ direction: 'TX', operation: 'read', request: '-', response: 'Invalid address', status: 'error' });
            return;
        }
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'i2c',
            url: `/i2c?action=read&addr=${p.addr}&rlen=${p.rlen}`,
            operation: 'read',
            requestText: `addr=0x${p.addr.toString(16).toUpperCase()}, rlen=${p.rlen}`,
            logger,
            containerDOM
        });
    });

    // Execute combined I2C Write-Read operation (common for reading specific registers)
    containerDOM.querySelector('#btn-i2c-wr').addEventListener('click', async () => {
        const p = getParams();
        if (!Number.isInteger(p.addr)) {
            logger.addRow({ direction: 'TX', operation: 'writeread', request: '-', response: 'Invalid address', status: 'error' });
            return;
        }
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'i2c',
            url: `/i2c?action=writeread&addr=${p.addr}&data=${p.tx}&rlen=${p.rlen}`,
            operation: 'writeread',
            requestText: `addr=0x${p.addr.toString(16).toUpperCase()}, tx=${p.tx || '(empty)'}, rlen=${p.rlen}`,
            logger,
            containerDOM
        });
    });

    ensureSetup(true).catch(() => {});
}

// SPI Master app - configure SPI bus speed and perform full-duplex transfers
function initSPIMaster(containerDOM) {
    const logger = createBusTableLogger(containerDOM, '#spi-log-body');
    const baudSelect = containerDOM.querySelector('#spi-baud');
    const rxLenInput = containerDOM.querySelector('#spi-rx-len');
    let configuredBaud = null;
    let setupPromise = null;

    if (rxLenInput) {
        rxLenInput.addEventListener('input', () => {
            if (rxLenInput.value === '') return;
            rxLenInput.value = String(parseNonNegativeInt(rxLenInput.value, 0));
        });

        rxLenInput.addEventListener('change', () => {
            rxLenInput.value = String(parseNonNegativeInt(rxLenInput.value, 1));
        });
    }

    // Apply baud rate settings before transaction if needed
    async function ensureSetup(force = false) {
        const baud = baudSelect.value;
        if (!force && configuredBaud === baud) return;
        if (setupPromise) return setupPromise;

        setupPromise = requestBusWithEvent({
            busType: 'spi',
            url: `/spi?action=setup&baud=${baud}`,
            operation: 'setup',
            requestText: `baud=${baud}`,
            logger,
            containerDOM
        }).then(() => {
            configuredBaud = baud;
        }).finally(() => {
            setupPromise = null;
        });

        return setupPromise;
    }

    baudSelect.addEventListener('change', async () => {
        configuredBaud = null;
        await ensureSetup(true);
    });

    containerDOM.querySelector('#btn-spi-clear-log').addEventListener('click', () => logger.clear());

    // Execute a standard SPI Write operation
    containerDOM.querySelector('#btn-spi-write').addEventListener('click', async () => {
        const tx = cleanHexInput(containerDOM.querySelector('#spi-tx').value);
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'spi',
            url: `/spi?action=write&data=${tx}`,
            operation: 'write',
            requestText: `tx=${tx || '(empty)'}`,
            logger,
            containerDOM
        });
    });

    // Execute a standard SPI Read operation
    containerDOM.querySelector('#btn-spi-read').addEventListener('click', async () => {
        const rlen = parseNonNegativeInt(containerDOM.querySelector('#spi-rx-len').value, 1);
        const pad = parseInt(cleanHexInput(containerDOM.querySelector('#spi-pad').value) || '0', 16);
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'spi',
            url: `/spi?action=read&rlen=${rlen}&send=${pad}`,
            operation: 'read',
            requestText: `rlen=${rlen}, pad=0x${pad.toString(16).toUpperCase()}`,
            logger,
            containerDOM
        });
    });

    // Execute a combined SPI Write-Read operation
    containerDOM.querySelector('#btn-spi-wr').addEventListener('click', async () => {
        const tx = cleanHexInput(containerDOM.querySelector('#spi-tx').value);
        const rlen = parseNonNegativeInt(containerDOM.querySelector('#spi-rx-len').value, 1);
        const pad = parseInt(cleanHexInput(containerDOM.querySelector('#spi-pad').value) || '0', 16);
        await ensureSetup();
        await requestBusWithEvent({
            busType: 'spi',
            url: `/spi?action=writeread&data=${tx}&rlen=${rlen}&send=${pad}`,
            operation: 'writeread',
            requestText: `tx=${tx || '(empty)'}, rlen=${rlen}, pad=0x${pad.toString(16).toUpperCase()}`,
            logger,
            containerDOM
        });
    });

    ensureSetup(true).catch(() => {});
}