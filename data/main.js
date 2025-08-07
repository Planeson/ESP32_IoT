
let currentCmd = 0;
let pollInterval = 1000;
let pollTimer = null;

function updateRetCmdUI(val) {
    document.getElementById('retCmdVal').textContent = val;
    document.getElementById('retCmdBits').textContent =
        ' (' + (val & 0b100 ? '1' : '0') +
        (val & 0b010 ? '1' : '0') +
        (val & 0b001 ? '1' : '0') + ')';
}



// Store sensor data history (max 100 points)
const sensorHistory = Array(8).fill().map(() => []);
const maxHistory = 100;

function pollStatus() {
    fetch('/status')
        .then(response => response.json())
        .then(data => {
            currentCmd = data.ret_cmd;
            updateRetCmdUI(currentCmd);
            // Save sensor data to history
            if (Array.isArray(data.sensor_data)) {
                for (let i = 0; i < 8; i++) {
                    sensorHistory[i].push(data.sensor_data[i] || 0);
                    if (sensorHistory[i].length > maxHistory) sensorHistory[i].shift();
                }
            }
            drawGraphs();
        })
        .catch(() => {
            document.getElementById('retCmdVal').textContent = '?';
        });
}

function drawGraphs() {
    for (let i = 0; i < 8; i++) {
        const canvas = document.getElementById('graph' + i);
        if (!canvas) continue;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        // Draw axis
        ctx.strokeStyle = '#bbb';
        ctx.beginPath();
        ctx.moveTo(0, canvas.height - 20);
        ctx.lineTo(canvas.width, canvas.height - 20);
        ctx.stroke();
        // Draw sensor data (full height)
        ctx.strokeStyle = '#e67e22';
        ctx.lineWidth = 2;
        ctx.beginPath();
        const sensorHist = sensorHistory[i];
        // Find min/max for scaling
        let min = Math.min(...sensorHist, 0), max = Math.max(...sensorHist, 100);
        if (max === min) max = min + 1;
        for (let j = 0; j < sensorHist.length; j++) {
            const x = (j / maxHistory) * canvas.width;
            // Scale sensor value to full height
            const norm = (sensorHist[j] - min) / (max - min);
            const y = canvas.height - 20 - norm * (canvas.height - 30);
            if (j === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
        }
        ctx.stroke();
        // Label
        ctx.fillStyle = '#333';
        ctx.font = '14px Arial';
        ctx.fillText('Sensor ' + i, 8, 16);
        ctx.fillStyle = '#e67e22';
        ctx.font = '12px Arial';
        ctx.fillText('Value: ' + (sensorHist[sensorHist.length-1]||0).toFixed(2), 8, canvas.height - 8);
    }
}


function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(pollStatus, pollInterval);
    pollStatus(); // Immediate update
}

window.onload = function() {
    // Add polling rate dropdown
    const rates = [1000, 2000, 5000];
    const rateLabels = {1000: '1s', 2000: '2s', 5000: '5s'};
    const dropdown = document.createElement('select');
    dropdown.id = 'pollRateDropdown';
    rates.forEach(rate => {
        const opt = document.createElement('option');
        opt.value = rate;
        opt.textContent = rateLabels[rate];
        dropdown.appendChild(opt);
    });
    dropdown.value = pollInterval;
    dropdown.onchange = function() {
        setPollingRate(parseInt(dropdown.value));
    };
    const label = document.createElement('label');
    label.textContent = 'Polling rate: ';
    label.htmlFor = 'pollRateDropdown';
    document.body.insertBefore(label, document.getElementById('status'));
    document.body.insertBefore(dropdown, document.getElementById('status'));
    startPolling();
};

function toggleBit(bit) {
    // Toggle the bit in currentCmd
    let newCmd = currentCmd ^ (1 << bit);
    // Only keep bits 0-2
    newCmd = newCmd & 0b111;
    // Send POST to /set_cmd
    fetch('/set_cmd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ret_cmd=' + newCmd
    })
    .then(response => response.text())
    .then(text => {
        document.getElementById('status').textContent = 'Set ret_cmd to ' + newCmd + ': ' + text;
    })
    .catch(err => {
        document.getElementById('status').textContent = 'Error: ' + err;
    });
}
