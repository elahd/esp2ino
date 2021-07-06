/**
 * TODO:
 * - When connected via router, use webui to connect to a different wifi network. esp2ino will successfully connect to the new network, but the webui will report a failure. (Reconnecting breaks the web session.) Webui should attempt to ping the server behind the scenes, then refresh if ping is successful.
 */

const fwUrl = document.getElementById('fw_url');
const fwFile = document.getElementById('fw_file');
const flashBtn = document.getElementById('flashBtn');

/**
 * Debug info slider
 */
tasmotaUrl = 'http://ota.tasmota.com/tasmota/release/tasmota.bin';
const acc = document.getElementsByClassName('accordion');
let i;

for (i = 0; i < acc.length; i++) {
	acc[i].addEventListener('click', function () {
		const panel = this.nextElementSibling;
		if (panel.style.display === 'block') {
			panel.style.display = 'none';
			this.innerHTML = 'Show Device Info';
		} else {
			panel.style.display = 'block';
			document.getElementById('debug-panel').scrollIntoView();
			this.innerHTML = 'Hide Device Info';
		}
	});
}

/**
 * Populate page data
 */
function populateDeviceInfo() {
	document.getElementById('fw_url').value = tasmotaUrl;
	fetch('info')
		.then(response => response.json())
		.then(jsonResponse => {
			jsonResponse.forEach(entry => {
				const element = document.getElementById(entry.id);
				const priorValue = element.innerHTML;
				element.innerHTML = entry.innerHTML;
				if (
					!['', entry.innerHTML].includes(priorValue) &&
					entry.id != 'debug-log'
				) {
					element.setAttribute('class', 'changed');
				}
			});
		})
		.catch(console.error);
}

/**
 * Download Backup
 */
document.getElementById('backup').onclick = function () {
	window.location.href = '/backup';
};

/**
 * Revert
 */
document.getElementById('undoBtn').onclick = function () {
	window.location.href = '/undo';
};

/**
 * Flasher
 */

document.getElementById('fw_select').onchange = function () {
	const selection = document.getElementById('fw_select').value;

	switch (selection) {
		case 'tasmota':
			fwUrl.value = tasmotaUrl;
			fwUrl.style.display = 'none';
			fwFile.style.display = 'none';
			flashBtn.innerHTML = 'Install';
			break;
		case 'custom':
			fwUrl.style.display = 'inline';
			fwUrl.value = '';
			fwFile.style.display = 'none';
			flashBtn.innerHTML = 'Install';
			break;
		case 'upload':
			fwUrl.value = tasmotaUrl;
			fwUrl.style.display = 'none';
			fwFile.style.display = 'inline';
			flashBtn.innerHTML = 'Upload & Install';
			break;
	}
};

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

const flashStatuses = {
	FLASH_STATE_DONE: '6',
	FLASH_STATE_FAIL: '3',
	FLASH_STATE_INACTIVE: '0',
	FLASH_STATE_RETRY: '4',
	FLASH_STATE_RUNNING: '1',
	FLASH_STATE_SUCCESS: '2',
	FLASH_STATE_WARN: '5',
};

const ansiUp = new AnsiUp();
ansiUp.use_classes = true;
document.getElementById('flashBtn').onclick = function () {
	const encodedUrl = document.getElementById('fw_url').value;
	const extension = encodedUrl.split(/[#?]/)[0].split('.').pop().trim();
	const prefix = encodedUrl.split(':')[0];

	if (document.getElementById('fw_select').value == 'upload') {
		buildFlashTaskTable('upload');
		if (document.getElementById('fw_file').value == '') {
			alert('You must select a firmware file to flash.');
		} else {
			document.getElementById('flashBtn').disabled = true;
			document.getElementById('flash-status-hr').hidden = false;
			document.getElementById('flash-status-container').hidden = false;
			document.getElementById('flash-status-container').scrollIntoView();
			appendFlashStatus({
				message: '',
				state: '1',
				step: '1',
				type: 'flashStatus',
			});
			fetch('upload', {
				body: fwFile.files[0],
				headers: {
					'Content-Type': fwFile.files[0].type,
				},
				method: 'PUT',
			})
				.then(processChunkedResponse)
				.then(onChunkedResponseComplete)
				.catch(onChunkedResponseError);
		}
	} else if (internetAvailable == false) {
		alert(
			'esp2ino must be connected to Wi-Fi to download firmware from the internet.',
		);
	} else if (!encodedUrl) {
		alert('You must enter a firmware URL before flashing.');
	} else if (extension !== 'bin') {
		alert('File must be an uncompressed .bin.');
	} else if (prefix !== 'http') {
		alert('Firmware URL must begin with http. https is not supported.');
	} else {
		buildFlashTaskTable('download');
		document.getElementById('flashBtn').disabled = true;
		document.getElementById('flash-status-hr').hidden = false;
		document.getElementById('flash-status-container').hidden = false;
		document.getElementById('flash-status-container').scrollIntoView();
		fetch('flash?url=' + encodedUrl)
			.then(processChunkedResponse)
			.then(onChunkedResponseComplete)
			.catch(onChunkedResponseError);
	}
};
function onChunkedResponseComplete(result) {
	console.log('Response closed!', result);
	// If anything fails, this will get us updated device state info
	// I.e.: partition content, etc.
	populateDeviceInfo();
}

function onChunkedResponseError(err) {
	console.error('Error! ' + err);
	alert('Unknown error. Check browser console.');
}

function appendFlashStatus(jsonObj) {
	const currentStepNode = document.getElementById('step-' + jsonObj.step);
	const currentStepUpdateNode = document.getElementById(
		'step-' + jsonObj.step + '-update',
	);
	switch (jsonObj.state) {
		case flashStatuses.FLASH_STATE_RUNNING:
			currentStepNode.setAttribute('state', 'running');
			break;
		case flashStatuses.FLASH_STATE_SUCCESS:
			currentStepNode.setAttribute('state', 'success');
			break;
		case flashStatuses.FLASH_STATE_RETRY:
			currentStepNode.setAttribute('state', 'retry');
			currentStepUpdateNode.innerHTML = jsonObj.message;
			break;
		case flashStatuses.FLASH_STATE_WARN:
			currentStepNode.setAttribute('state', 'warn');
			currentStepUpdateNode.innerHTML = jsonObj.message;
			break;
		case flashStatuses.FLASH_STATE_FAIL:
			currentStepNode.setAttribute('state', 'fail');
			currentStepUpdateNode.innerHTML = jsonObj.message;
			break;
		case flashStatuses.FLASH_STATE_DONE:
			currentStepNode.setAttribute('state', 'done');
			currentStepUpdateNode.innerHTML = jsonObj.message;
			confetti({
				disableForReducedMotion: true,
				gravity: 0.5,
				particleCount: 150,
				spread: 180,
			});
			break;
	}
}

function appendDebug(message) {
	enableDebugLog();
	const ansiFormatted = ansiUp.ansi_to_html(message);
	const entry = document.createElement('pre');
	entry.innerHTML = ansiFormatted;
	const debugLog = document.getElementById('debug-log');

	// ansiNode = document.createRange().createContextualFragment(ansiFormatted);

	if (debugLog.hasAttribute('ready')) {
		debugLog.removeAttribute('ready');
		debugLog.innerHTML = '';
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
		const subChunks = chunk.split('\n');
		subChunks.forEach(subChunk => {
			if (subChunk === '') {
				return;
			}
			jsonChunk = JSON.parse(subChunk);
			if (jsonChunk.type === 'flashStatus') {
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

function buildFlashTaskTable(method = 'download') {
	const flashTasks = [
		{
			/* Filled in below. */
		},
		{
			primary: 'Erasing Espressif bootloader.',
			type: 'intermediate',
		},
		{
			primary: 'Writing Arduino bootloader.',
			type: 'intermediate',
		},
		{
			primary: 'Saving initialization instructions for Arduino bootloader.',
			type: 'intermediate',
		},
		{
			primary: 'Performing housekeeping.',
			type: 'intermediate',
		},
		{
			primary: 'Restarting...',
			type: 'done',
		},
	];

	if (method == 'download') {
		flashTasks[0] = {
			primary: 'Downloading and flashing new firmware.',
			secondary: 'This may take a few minutes.',
			type: 'intermediate',
		};
	} else {
		flashTasks[0] = {
			primary: 'Uploading and flashing new firmware.',
			secondary:
				'ESP8266 uploads are <em>slow</em>. This may take a few minutes. Be patient...',
			type: 'intermediate',
		};
	}

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
		const taskRow = document.createElement('tr');
		taskRow.setAttribute('state', 'inactive');
		taskRow.setAttribute('id', 'step-' + (index + 1));

		/* Populate Icon Cell */
		const taskIcon = document.createElement('td');
		taskIcon.setAttribute('class', 'task-icon');
		taskRow.appendChild(taskIcon);
		switch (task.type) {
			case 'intermediate':
				taskIcon.innerHTML =
					spinnerNodeInnerHtml +
					successNodeInnerHtml +
					warnNodeInnerHtml +
					failNodeInnerHtml;
				break;
			case 'done':
				taskIcon.innerHTML = doneNodeInnerHtml;
				break;
		}

		/* Populate Message Cell */
		const taskMsg = document.createElement('td');
		taskMsg.setAttribute('class', 'task-msg');

		const taskMsgPrimary = document.createElement('p');
		taskMsgPrimary.setAttribute('class', 'primary');
		taskMsgPrimary.innerHTML = task.primary;
		taskMsg.appendChild(taskMsgPrimary);

		if (task.secondary) {
			const taskMsgSecondary = document.createElement('p');
			taskMsgSecondary.setAttribute('class', 'secondary');
			taskMsgSecondary.innerHTML = task.secondary;
			taskMsg.appendChild(taskMsgSecondary);
		}

		const taskMsgUpdate = document.createElement('p');
		taskMsgUpdate.setAttribute('class', 'update');
		taskMsgUpdate.setAttribute('id', 'step-' + (index + 1) + '-update');
		taskMsg.appendChild(taskMsgUpdate);

		/* Populate Log Button Cell */
		const taskLogBtn = document.createElement('td');
		taskLogBtn.setAttribute('id', 'step-' + (index + 1) + '-logBtn');
		taskLogBtn.setAttribute('class', 'logBtn');
		taskLogBtn.innerHTML = logButtonInnerHtml;

		/* Append Cells to Row */
		taskRow.appendChild(taskIcon);
		taskRow.appendChild(taskMsg);
		taskRow.appendChild(taskLogBtn);

		/* Append Row to Table */
		document.getElementById('flash-status').appendChild(taskRow);
	});

	return true;
}

/**
 * Debug log is a work in progress on the backend. Frontend code is complete, but all
 * debug-log elements are turned off by default. If we receive a debug log message from the
 * backend, we'll turn on the debug log feature on the frontend on-the-fly.
 */
function enableDebugLog() {
	if (!document.body.getAttribute('debug-log-enabled')) {
		document.body.setAttribute('debug-log-enabled', '');
		debugElements = document.getElementsByClassName('debug-log');
		[].forEach.call(debugElements, element => {
			element.removeAttribute('hidden');
		});
	}
}

// function viewLog() {
// 	const debugPanelDisplay = window.getComputedStyle(
// 		document.getElementById('debug-panel'),
// 		null,
// 	).display;
// 	if (debugPanelDisplay === 'none') {
// 		document.getElementById('show-device-btn').click();
// 	} else {
// 		document.getElementById('debug-panel').scrollIntoView();
// 	}
// }

/**
 * Wi-Fi Status
 */

internetAvailable = false;

function submitWifiConnect() {
	const ssid = document.getElementById('ssid').value;

	const requestUrl =
		'wifi?' +
		new URLSearchParams({
			pswd: document.getElementById('pswd').value,
			ssid: ssid,
		});

	wifiStatusChanger('connecting', null, ssid);

	fetch(requestUrl)
		// Give esp2ino time to disconnect from current Wi-Fi network.
		.then(() => new Promise(resolve => setTimeout(resolve, 5000)))
		.then(() =>
			// Spend up to 20 seconds polling for Wi-Fi.
			Promise.race([
				getCurrentWifi(ssid),
				new Promise((resolve, reject) => {
					setTimeout(reject, 20000);
				}),
			]),
		)
		.catch(error => {
			console.error(error);
			wifiStatusChanger('connect_failed', null, ssid);
		});

	return true;
}

const getCurrentWifi = desiredSsid => {
	fetch('wifiStatus')
		.then(response => response.json())
		.then(jsonResponse => {
			if (desiredSsid && jsonResponse.ssid !== desiredSsid) {
				wifiStatusChanger('connect_failed', null, desiredSsid);
			} else if (jsonResponse.status == 'connected') {
				wifiStatusChanger('connected', null, jsonResponse.ssid);
				internetAvailable = true;
			} else {
				wifiStatusChanger('notconnected');
				internetAvailable = false;
			}
		});
};

function wifiStatusChanger(newState, numNetworks, ssid, map) {
	// none, waiting, idle, list, connected
	// none, initialized, notconnected, scanning, scanned, connecting, connected

	const wifiButton = document.getElementById('wifi-connect');
	const wifiStatusText = document.getElementById('wifi-status-text');
	const wifiWrapper = document.getElementById('wifi-wrapper');
	// const wifiBar = document.getElementById('wifi-status-bar');

	const wifiStateMap = {
		connect_failed: {
			button: 'Scan Again',
			status: 'Failed to connect to ' + (ssid ? ssid : 'Wi-Fi'),
			template: 'idle',
		},
		connected: {
			button: 'Change Network',
			status: 'Connected to ' + (ssid ? ssid : 'Wi-Fi'),
			template: 'idle',
		},
		connecting: {
			button: null,
			status: 'Connecting to ' + (ssid ? ssid : 'Wi-Fi') + '...',
			template: 'waiting',
		},
		initialized: {
			button: null,
			status: 'Checking Wi-Fi connection...',
			template: 'waiting',
		},
		notconnected: {
			button: 'Connect to Wi-Fi',
			status: 'No Internet Connection',
			template: 'idle',
		},
		scanned: {
			button: 'Scan Again',

			status:
				numNetworks == 0 ? 'No Wi-Fi Networks Found' : 'Select a network:',
			template: 'list',
		},
		scanning: {
			button: null,
			status: 'Scanning for Wi-Fi networks...',
			template: 'waiting',
		},
	};

	newMap = wifiStateMap[newState];
	if (!newMap) {
		return false;
	}

	wifiStatusText.innerHTML = newMap.status;
	wifiButton.innerHTML = newMap.button ? newMap.button : null;

	if (newState == 'scanned' && !numNetworks) {
		wifiWrapper.setAttribute('state', 'idle');
	} else {
		wifiWrapper.setAttribute('state', newMap.template);
	}

	return true;
}

function getWifiNetworks() {
	// Clear SSID list
	document.getElementById('ssid').innerHTML = null;
	document.getElementById('pswd').value = '';

	wifiStatusChanger('scanning');
	numNetworksFound = 0;

	fetch('wifi')
		.then(response => response.json())
		.then(jsonResponse => {
			if (jsonResponse.type != 'wifi_results') {
				throw 'Wi-Fi scan failed.';
			} else {
				return jsonResponse.wifi_results;
			}
		})
		.then(wifiResults => wifiResults.sort(wifiSort))
		.then(sortedArray => {
			// Remove duplicate entries by SSID. Keeps strongest.
			const seen = {};
			return sortedArray.filter(item => {
				const k = item.ssid;
				return seen.hasOwnProperty(k) ? false : (seen[k] = true);
			});
		})
		.then(dedupedArray => {
			dedupedArray.forEach(entry => {
				if (entry.ssid == '') {
					return;
				}
				wifiList = document.getElementById('ssid');
				// <option value="0-13">0-13</option>

				const wifiOption = document.createElement('option');
				wifiOption.setAttribute('value', entry.ssid);
				wifiOption.innerHTML = entry.ssid + ' (' + entry.signal + '%)';

				wifiList.appendChild(wifiOption);
			});
			numNetworksFound = dedupedArray.length;
		})
		.then(() => {
			wifiStatusChanger('scanned', numNetworksFound);
		})
		.catch(console.error);
}

function wifiSort(a, b) {
	// Sort by signal strength.
	// if (a.signal > b.signal) return -1;
	// if (b.signal > a.signal) return 1;

	// Alpha sort by ssid.
	if (a.ssid.toLowerCase() > b.ssid.toLowerCase()) {
		return 1;
	}
	if (b.ssid.toLowerCase() > a.ssid.toLowerCase()) {
		return -1;
	}

	return 0;
}

// function generateSignalBars(pct) {
//     const fullBars = "▁▂▃▄▅▆▇█";
//     return fullBars.slice(0,Math.ceil(pct/12.5));
// }
window.onload = populateDeviceInfo();

document.getElementById('wifi-connect').onclick = function () {
	getWifiNetworks();
};

document.getElementById('wifi-submit').onclick = function () {
	submitWifiConnect();
};

wifiStatusChanger('initialized');
getCurrentWifi();

document.getElementById('wifi-status-spinner').innerHTML = spinnerNodeInnerHtml;
