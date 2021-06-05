/**
 * Debug info slider
 */

tasmotaUrl = "";
const acc = document.getElementsByClassName("accordion");
let i;

for (i = 0; i < acc.length; i++) {
    acc[i].addEventListener("click", function () {
        const panel = this.nextElementSibling;
        if (panel.style.display === "block") {
            panel.style.display = "none";
            this.innerHTML = "Show Device Info";
        } else {
            panel.style.display = "block";
            document.getElementById("debug-panel").scrollIntoView();
            this.innerHTML = "Hide Device Info";
        }
    });
}

/**
 * Populate page data
 */

window.onload = populateDeviceInfo();

function populateDeviceInfo() {
    fetch("info")
        .then((response) => response.json())
        .then((jsonResponse) => {
            jsonResponse.forEach((entry) => {
                const element = document.getElementById(entry.id);
                const priorValue = element.innerHTML;
                element.innerHTML = entry.innerHTML;
                if (
                    !["", entry.innerHTML].includes(priorValue) &&
                    entry.id != "debug-log"
                ) {
                    element.setAttribute("class", "changed");
                }

                if (entry.id == "chip") {
                    setChip(element.innerHTML);
                    /* Set Tasmota URL for initial page load. */
                    document.getElementById("fw_url").value = tasmotaUrl;
                }
            });
        })
        .catch(console.error);
}

function setChip(chipName) {
    if (chipName == "ESP32") {
        tasmotaUrl = "http://ota.tasmota.com/tasmota32/release/tasmota32.bin";
    } else if (chipName == "ESP8266") {
        tasmotaUrl = "http://ota.tasmota.com/tasmota/release/tasmota.bin";
    } else {
        tasmotaUrl = "";
    }
}

/**
 * Download Backup
 */
document.getElementById("backup").onclick = function () {
    window.location.href = "/backup";
};

/**
 * Revert
 */
document.getElementById("undoBtn").onclick = function () {
    window.location.href = "/undo";
};

/**
 * Flasher
 */

const flashStatuses = {
    FLASH_STATE_INACTIVE: "0",
    FLASH_STATE_RUNNING: "1",
    FLASH_STATE_SUCCESS: "2",
    FLASH_STATE_FAIL: "3",
    FLASH_STATE_RETRY: "4",
    FLASH_STATE_WARN: "5",
    FLASH_STATE_DONE: "6",
};

const ansiUp = new AnsiUp();
ansiUp.use_classes = true;
document.getElementById("flashBtn").onclick = function () {
    const encodedUrl = document.getElementById("fw_url").value;
    const extension = encodedUrl.split(/[#?]/)[0].split(".").pop().trim();
    buildFlashTaskTable();
    if (!encodedUrl) {
        alert("You must enter a firmware URL before flashing.");
    } else if (extension !== "bin") {
        alert("File must be an uncompressed .bin.");
    } else {
        document.getElementById("flashBtn").disabled = true;
        document.getElementById("flash-status-hr").hidden = false;
        document.getElementById("flash-status-container").hidden = false;
        document.getElementById("flash-status-container").scrollIntoView();
        fetch("flash?url=" + encodedUrl)
            .then(processChunkedResponse)
            .then(onChunkedResponseComplete)
            .catch(onChunkedResponseError);
    }
};

function onChunkedResponseComplete(result) {
    console.log("Response closed!", result);
    // If anything fails, this will get us updated device state info
    // I.e.: partition content, etc.
    populateDeviceInfo();
}

function onChunkedResponseError(err) {
    console.error("Error! " + err);
}

function appendFlashStatus(jsonObj) {
    const currentStepNode = document.getElementById("step-" + jsonObj.step);
    const currentStepUpdateNode = document.getElementById(
        "step-" + jsonObj.step + "-update"
    );
    switch (jsonObj.state) {
        case flashStatuses.FLASH_STATE_RUNNING:
            currentStepNode.setAttribute("state", "running");
            break;
        case flashStatuses.FLASH_STATE_SUCCESS:
            currentStepNode.setAttribute("state", "success");
            break;
        case flashStatuses.FLASH_STATE_RETRY:
            currentStepNode.setAttribute("state", "retry");
            currentStepUpdateNode.innerHTML = jsonObj.message;
            break;
        case flashStatuses.FLASH_STATE_WARN:
            currentStepNode.setAttribute("state", "warn");
            currentStepUpdateNode.innerHTML = jsonObj.message;
            break;
        case flashStatuses.FLASH_STATE_FAIL:
            currentStepNode.setAttribute("state", "fail");
            currentStepUpdateNode.innerHTML = jsonObj.message;
            break;
        case flashStatuses.FLASH_STATE_DONE:
            currentStepNode.setAttribute("state", "done");
            currentStepUpdateNode.innerHTML = jsonObj.message;
            confetti({
                spread: 180,
                particleCount: 150,
                gravity: 0.5,
                disableForReducedMotion: true,
            });
            break;
    }
}

function appendDebug(message) {
    enableDebugLog();
    const ansiFormatted = ansiUp.ansi_to_html(message);
    const entry = document.createElement("pre");
    entry.innerHTML = ansiFormatted;
    const debugLog = document.getElementById("debug-log");

    // ansiNode = document.createRange().createContextualFragment(ansiFormatted);

    if (debugLog.hasAttribute("ready")) {
        debugLog.removeAttribute("ready");
        debugLog.innerHTML = "";
    }

    debugLog.appendChild(entry);
    debugLog.scrollTo(0, debugLog.scrollHeight);
}

function processChunkedResponse(response) {
    const reader = response.body.getReader();
    const decoder = new TextDecoder();

    return readChunk();

    function readChunk() {
        return reader.read().then(appendChunks);
    }

    function appendChunks(result) {
        const chunk = decoder.decode(result.value || new Uint8Array(), {
            stream: !result.done,
        });
        const subChunks = chunk.split("\n");
        subChunks.forEach((subChunk) => {
            if (subChunk === "") return;
            jsonChunk = JSON.parse(subChunk);
            if (jsonChunk.type === "flashStatus") {
                appendFlashStatus(jsonChunk);
            } else {
                // console.log('debug message: ' + jsonChunk.message)
                appendDebug(jsonChunk.message);
            }
        });

        if (result.done) {
            // console.log("returning");
        } else {
            // console.log("recursing");
            return readChunk();
        }
    }
}

document.getElementById("fw_select").onchange = function () {
    const selection = document.getElementById("fw_select").value;
    switch (selection) {
        case "tasmota":
            document.getElementById("fw_url").value = tasmotaUrl;
            document.getElementById("fw_url").style.display = "none";
            break;
        case "custom":
            document.getElementById("fw_url").style.display = "inline";
            document.getElementById("fw_url").value = "";
            break;
    }
};

function buildFlashTaskTable() {
    const flashTasks = [
        {
            primary: "Downloading and flashing new firmware.",
            secondary: "This may take a few minutes.",
            type: "intermediate",
        },
        {
            primary: "Erasing Espressif bootloader.",
            type: "intermediate",
        },
        {
            primary: "Writing Arduino bootloader.",
            type: "intermediate",
        },
        {
            primary:
                "Saving initialization instructions for Arduino bootloader.",
            type: "intermediate",
        },
        {
            primary: "Performing housekeeping.",
            type: "intermediate",
        },
        {
            primary: "Restarting...",
            type: "done",
        },
    ];

    const spinnerNodeInnerHtml = `
    <div class="loader sk-fading-circle">
        <div class="sk-circle1 sk-circle"></div>
        <div class="sk-circle2 sk-circle"></div>
        <div class="sk-circle3 sk-circle"></div>
        <div class="sk-circle4 sk-circle"></div>
        <div class="sk-circle5 sk-circle"></div>
        <div class="sk-circle6 sk-circle"></div>
        <div class="sk-circle7 sk-circle"></div>
        <div class="sk-circle8 sk-circle"></div>
        <div class="sk-circle9 sk-circle"></div>
        <div class="sk-circle10 sk-circle"></div>
        <div class="sk-circle11 sk-circle"></div>
        <div class="sk-circle12 sk-circle"></div>
    </div>
    `;

    const successNodeInnerHtml = '<div class="success emoji">✅</div>';
    const failNodeInnerHtml = '<div class="fail emoji">❌</div>';
    const warnNodeInnerHtml = '<div class="warn emoji">⚠️</div>';
    const doneNodeInnerHtml = '<div class="done emoji">🎉</div>';
    const logButtonInnerHtml = `
      <button class="button button-clear log-button debug-log" onClick="viewLog()" type="button" hidden>
        View Log
      </button>
      `;

    flashTasks.forEach((task, index) => {
        const taskRow = document.createElement("tr");
        taskRow.setAttribute("state", "inactive");
        taskRow.setAttribute("id", "step-" + (index + 1));

        /* Populate Icon Cell */
        const taskIcon = document.createElement("td");
        taskIcon.setAttribute("class", "task-icon");
        taskRow.appendChild(taskIcon);
        switch (task.type) {
            case "intermediate":
                taskIcon.innerHTML =
                    spinnerNodeInnerHtml +
                    successNodeInnerHtml +
                    warnNodeInnerHtml +
                    failNodeInnerHtml;
                break;
            case "done":
                taskIcon.innerHTML = doneNodeInnerHtml;
                break;
        }

        /* Populate Message Cell */
        const taskMsg = document.createElement("td");
        taskMsg.setAttribute("class", "task-msg");

        const taskMsgPrimary = document.createElement("p");
        taskMsgPrimary.setAttribute("class", "primary");
        taskMsgPrimary.innerHTML = task.primary;
        taskMsg.appendChild(taskMsgPrimary);

        if (task.secondary) {
            const taskMsgSecondary = document.createElement("p");
            taskMsgSecondary.setAttribute("class", "secondary");
            taskMsgSecondary.innerHTML = task.secondary;
            taskMsg.appendChild(taskMsgSecondary);
        }

        const taskMsgUpdate = document.createElement("p");
        taskMsgUpdate.setAttribute("class", "update");
        taskMsgUpdate.setAttribute("id", "step-" + (index + 1) + "-update");
        taskMsg.appendChild(taskMsgUpdate);

        /* Populate Log Button Cell */
        const taskLogBtn = document.createElement("td");
        taskLogBtn.setAttribute("id", "step-" + (index + 1) + "-logBtn");
        taskLogBtn.setAttribute("class", "logBtn");
        taskLogBtn.innerHTML = logButtonInnerHtml;

        /* Append Cells to Row */
        taskRow.appendChild(taskIcon);
        taskRow.appendChild(taskMsg);
        taskRow.appendChild(taskLogBtn);

        /* Append Row to Table */
        document.getElementById("flash-status").appendChild(taskRow);
    });

    return true;
}

/**
 * Debug log is a work in progress on the backend. Frontend code is complete, but all
 * debug-log elements are turned off by default. If we receive a debug log message from the
 * backend, we'll turn on the debug log feature on the frontend on-the-fly.
 */
function enableDebugLog() {
    if (!document.body.getAttribute("debug-log-enabled")) {
        document.body.setAttribute("debug-log-enabled", "");
        debugElements = document.getElementsByClassName("debug-log");
        [].forEach.call(debugElements, function (element) {
            element.removeAttribute("hidden");
        });
    }
}

function viewLog() {
    const debugPanelDisplay = window.getComputedStyle(
        document.getElementById("debug-panel"),
        null
    ).display;
    if (debugPanelDisplay === "none") {
        document.getElementById("show-device-btn").click();
    } else {
        document.getElementById("debug-panel").scrollIntoView();
    }
}
