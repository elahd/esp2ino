/**
 * Loading Spinner
 */
var spinnerOpts = {
	lines: 13, // The number of lines to draw
	length: 38, // The length of each line
	width: 17, // The line thickness
	radius: 45, // The radius of the inner circle
	scale: 0.1, // Scales overall size of the spinner
	corners: 1, // Corner roundness (0..1)
	speed: 1, // Rounds per second
	rotate: 0, // The rotation offset
	animation: 'spinner-line-fade-quick', // The CSS animation name for the lines
	direction: 1, // 1: clockwise, -1: counterclockwise
	color: '#2e2e2e', // CSS color or array of colors
	fadeColor: 'transparent', // CSS color or array of colors
	top: '50%', // Top position relative to parent
	left: '50%', // Left position relative to parent
	shadow: '0 0 1px transparent', // Box-shadow for the lines
	zIndex: 2000000000, // The z-index (defaults to 2e9)
	className: 'spinner', // The CSS class to assign to the spinner
	position: 'absolute', // Element positioning
  };
/**
 * Debug info slider
 */

const acc = document.getElementsByClassName("accordion");
let i;

for (i = 0; i < acc.length; i++) {
	acc[i].addEventListener("click", function () {
		const panel = this.nextElementSibling;
		if (panel.style.display === "block") {
			panel.style.display = "none";
			this.innerHTML = "Show Debug Info";
		} else {
			panel.style.display = "block";
			document.getElementById("debug-panel").scrollIntoView();
			this.innerHTML = "Hide Debug Info";
		}
	});
}

/**
 * Populate page data
 */

window.onload = function () {
	fetch("info")
		.then((response) => response.json())
		.then((jsonResponse) => {
			jsonResponse.forEach((entry) => {
				document.getElementById(entry.id).innerHTML = entry.innerHTML;
			});
		})
		.catch(console.error);
};

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

const ansiUp = new AnsiUp();
ansiUp.use_classes = true;
const tasmotaUrl = "http://ota.tasmota.com/tasmota/release/tasmota.bin";
document.getElementById("flashBtn").onclick = function () {
	const encodedUrl = document.getElementById("fw_url").value;
	var extension = encodedUrl.split(/[#?]/)[0].split('.').pop().trim();
	if (!encodedUrl) {
		alert("You must enter a firmware URL before flashing.");
	}
	else if (extension != "bin") {
		alert("File must be an uncompressed .bin.");
	} else {
		document.getElementById("flashBtn").disabled = true;
		document.getElementById("progress").scrollIntoView();
		fetch("flash?url=" + encodedUrl)
			.then(processChunkedResponse)
			.then(onChunkedResponseComplete)
			.catch(onChunkedResponseError);
	}
};

function onChunkedResponseComplete(result) {
	console.log("Response closed!", result);
}

function onChunkedResponseError(err) {
	console.error("Error! " + err);
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
		console.log(chunk);
		var subChunks = chunk.split("\n");
		subChunks.forEach((subChunk) => {
			if (subChunk == "🎉") {
				confetti({
					spread: 180,
					particleCount: 150,
					gravity: 0.5,
					disableForReducedMotion: true,
				});
			} else {
				const ansiFormatted = ansiUp.ansi_to_html(subChunk);
				const entry = document.createElement("pre");
				entry.innerHTML = ansiFormatted;
				const progress = document.getElementById("progress");

				// ansiNode = document.createRange().createContextualFragment(ansiFormatted);

				if (progress.hasAttribute("ready")) {
					progress.removeAttribute("ready");
					progress.innerHTML = "";
				}

				progress.appendChild(entry);
				progress.scrollTo(0, progress.scrollHeight);
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

/* Set Tasmota URL for initial page load. */
document.getElementById("fw_url").value = tasmotaUrl;

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
