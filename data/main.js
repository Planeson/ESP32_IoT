let currentCmd = 0;

// SSE: Listen for ret_cmd updates
const evtSource = new EventSource('/sse');
evtSource.onmessage = function(event) {
    try {
        const data = JSON.parse(event.data);
        currentCmd = data.ret_cmd;
        document.getElementById('retCmdVal').textContent = currentCmd;
        document.getElementById('retCmdBits').textContent =
            ' (' + (currentCmd & 0b100 ? '1' : '0') +
            (currentCmd & 0b010 ? '1' : '0') +
            (currentCmd & 0b001 ? '1' : '0') + ')';
    } catch (e) {
        document.getElementById('retCmdVal').textContent = '?';
    }
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
