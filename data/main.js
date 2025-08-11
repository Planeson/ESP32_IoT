let currentCmd = 0;
let pollInterval = 1000;
let pollTimer = null;
let cmd = [false, false, false];
let buttonDOM = [null, null, null];

function initButtonDom() {
    buttonDOM = [
        document.getElementById("bDoor"),
        document.getElementById("bFan"),
        document.getElementById("bLight")
    ];
}

function updateRetCmdUI() {
    cmd[0] = currentCmd & 0b100;
    cmd[1] = currentCmd & 0b010;
    cmd[2] = currentCmd & 0b001;
    for (let i = 0; i < 3; ++i){
        if (cmd[i]) {
            buttonDOM[i].style.backgroundColor = '#2ecc71'; // Green for ON
        }
        else {
            buttonDOM[i].style.backgroundColor = '#e74c3c'; // Red for OFF
        }
    }
}



// Store sensor data history (max 100 points)
const sensorHistory = Array(8).fill().map(() => []);
const sensorTimestamps = Array(8).fill().map(() => []);
const maxHistory = 100;

function pollStatus() {
    fetch('/status')
        .then(response => response.json())
        .then(data => {
            // Save button states
            currentCmd = data.ret_cmd;
            updateRetCmdUI();
            // Save sensor data to history with timestamps
            if (Array.isArray(data.sensor_data)) {
                const now = Date.now();
                for (let i = 0; i < 8; i++) {
                    sensorHistory[i].push(data.sensor_data[i] || 0);
                    sensorTimestamps[i].push(now);
                    if (sensorHistory[i].length > maxHistory) sensorHistory[i].shift();
                    if (sensorTimestamps[i].length > maxHistory) sensorTimestamps[i].shift();
                }
            }
            drawGraphs();
        })
        .catch(() => {
            console.error('Error fetching status');
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
        ctx.moveTo(0, canvas.height - 40);
        ctx.lineTo(canvas.width, canvas.height - 40);
        ctx.stroke();

        // Draw time axis ticks (every 20 points)
        const timestamps = sensorTimestamps[i];
        if (timestamps.length > 1) {
            ctx.fillStyle = '#666';
            ctx.font = '28px Arial';
            for (let j = 0; j < timestamps.length; j += 20) {
                const x = (j / maxHistory) * canvas.width;
                const t = new Date(timestamps[j]);
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
}


/*function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(pollStatus, pollInterval);
    pollStatus(); // Immediate update
}*/

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
}

// initialization
window.onload = function() {
    initButtonDom();
    setInterval(pollStatus, 1000);
};
