let currentCmd = 0;
let pollInterval = 1000;
let pollTimer = null;
let butDOM = null;
let fanDOM = null;
let litDOM = null;


function initSwitchesDom() {
    butDOM = document.getElementById("bDoor");
    fanDOM = document.getElementById("fanSlider");
    litDOM = document.getElementById("lightSlider");
}

// Store sensor data history (max 100 points)
const sensorHistory = Array(8).fill().map(() => []);
const sensorTimestamp = [];
const maxHistory = 100;

function pollStatus() {
    fetch('/status')
        .then(response => response.json())
        .then(data => {
            // Save button states
            updateStatusUI(data);
        })
        .catch((error) => {
            console.error('Error fetching status', error);
        });
}

function pollSensors() {
    fetch('/sensor')
        .then(response => response.json())
        .then(data => {
            // Save sensor data to history with sensorTimestamps
            if (Array.isArray(data.sensor_data)) {
                const now = Date.now();
                sensorTimestamp.push(now);
                if (sensorTimestamp.length > maxHistory) sensorTimestamp.shift();
                for (let i = 0; i < 8; i++) {
                    sensorHistory[i].push(data.sensor_data[i] || 0);
                    if (sensorHistory[i].length > maxHistory) sensorHistory[i].shift();
                }
            }
            drawGraphs();
        })
        .catch(() => {
            console.error('Error fetching status');
        });
}

// --- Frontend logic for new controls ---
let doorState = 0;
let fanLevel = 0;
let lightLevel = 0;
let fanLevelServer = 0;
let lightLevelServer = 0;

function updateStatusUI(data) {
    // set door button
    butDOM.style.backgroundColor = data.door_state ? '#2ecc71' : '#e74c3c';
    
    // set sliders
    fanLevelServer = data.fan_level;
    lightLevelServer = data.light_level;
    setSliderServer(fanDOM, fanLevelServer);
    setSliderServer(litDOM, lightLevelServer);
}

function setCmd() {
    fetch('/set_cmd', {
        method: 'POST',
        body: `door=${doorState}&fan=${fanLevel}&light=${lightLevel}`,
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
    }).then(() => pollStatus());
}

function toggleDoor() {
    doorState = doorState ? 0 : 1;
    setCmd();
}

document.addEventListener('DOMContentLoaded', function () {
    initSwitchesDom();
    fanDOM.addEventListener('input', function () {
        fanLevel = parseInt(this.value, 10);
        setSliderServer(this, fanLevelServer); // Show user drag
    });
    
    fanDOM.addEventListener('change', function () {
        setCmd();
    });
    
    litDOM.addEventListener('input', function () {
        lightLevel = parseInt(this.value, 10);
        setSliderServer(this, lightLevelServer); // Show user drag
    });
    
    litDOM.addEventListener('change', function () {
        setCmd();
    });
    
    // Initial poll and periodic update
    pollSensors();
    setInterval(pollSensors, 1000);
});


// first is user val, second is returned val
let fanSliderVal = [0, 0];
let lightSliderVal = [0, 0];

function setSliderServer(slider, serverLevel) {
    const sliderPercent = slider.value / 255 * 100;
    const serverPercent = serverLevel / 255 * 100;
    if (sliderPercent > serverPercent) {
        slider.style.setProperty('--pc1', sliderPercent + '%');
        slider.style.setProperty('--pc2', serverPercent + '%');
    }
    else {
        slider.style.setProperty('--pc1', serverPercent + '%');
        slider.style.setProperty('--pc2', sliderPercent + '%');
    }
};

function drawGraphs() {
    for (let i = 0; i < 8; i++) {
        const canvas = document.getElementById('graph' + i);
        if (!canvas) continue;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        // Draw axis
        ctx.strokeStyle = '#bbb';
        ctx.beginPath();
        ctx.moveTo(0, canvas.height - 40);
        ctx.lineTo(canvas.width, canvas.height - 40);
        ctx.stroke();

        // Draw time axis ticks (every 20 points)
        if (sensorTimestamps.length > 1) {
            ctx.fillStyle = '#666';
            ctx.font = '28px Arial';
            for (let j = 0; j < sensorTimestamp.length; j += 20) {
                const x = (j / maxHistory) * canvas.width;
                const t = new Date(sensorTimestamps[j]);
                const label = t.toLocaleTimeString();
                ctx.fillText(label, x, canvas.height - 10);
            }
        }

        // Draw sensor data (full height)
        ctx.strokeStyle = '#e67e22';
        ctx.lineWidth = 2;
        ctx.beginPath();
        const sensorHist = sensorHistory[i];
        // Find min/max for scaling
        let min = Math.min(...sensorHist), max = Math.max(...sensorHist);
        if (max === min) max = min + 1;
        for (let j = 0; j < sensorHist.length; j++) {
            const x = (j / maxHistory) * canvas.width;
            // Scale sensor value to full height
            const norm = (sensorHist[j] - min) / (max - min);
            const y = canvas.height - 80 - norm * (canvas.height - 140);
            if (j === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }
        ctx.stroke();
        // Label
        ctx.fillStyle = '#333';
        ctx.font = '40px Arial';
        ctx.fillText('Sensor ' + i, 8, 44);
        ctx.fillStyle = '#e67e22';
        ctx.font = '38px Arial';
        ctx.fillText('Value: ' + (sensorHist[sensorHist.length - 1] || 0).toFixed(2), 8, canvas.height - 46);
    }
};

// initialization
window.onload = function () {
};
