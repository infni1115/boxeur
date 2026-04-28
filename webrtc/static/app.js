const localVideo = document.getElementById("localVideo");
const remoteVideo = document.getElementById("remoteVideo");
const statusEl = document.getElementById("status");
const splash = document.getElementById("splash");
const splashTitle = document.getElementById("splashTitle");
const splashSub = document.getElementById("splashSub");
const startBtn = document.getElementById("startBtn");
const controls = document.querySelector(".controls");
const micBtn = document.getElementById("micBtn");
const camBtn = document.getElementById("camBtn");
const endBtn = document.getElementById("endBtn");

const ICE = { iceServers: [{ urls: "stun:stun.l.google.com:19302" }] };

let socket, pc, localStream, role;

function setStatus(text, live = false) {
    statusEl.textContent = text;
    statusEl.classList.toggle("live", live);
}

function showSplash(title, sub, btn = false) {
    splashTitle.textContent = title;
    splashSub.textContent = sub;
    startBtn.hidden = !btn;
    splash.classList.remove("hidden");
}

function hideSplash() {
    splash.classList.add("hidden");
    controls.hidden = false;
}

async function getMedia() {
    localStream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: "user", width: { ideal: 1280 }, height: { ideal: 720 } },
        audio: true,
    });
    localVideo.srcObject = localStream;
}

function createPeer() {
    pc = new RTCPeerConnection(ICE);

    localStream.getTracks().forEach((t) => pc.addTrack(t, localStream));

    pc.ontrack = (e) => {
        if (remoteVideo.srcObject !== e.streams[0]) {
            remoteVideo.srcObject = e.streams[0];
            setStatus("connected", true);
            hideSplash();
        }
    };

    pc.onicecandidate = (e) => {
        if (e.candidate) socket.emit("signal", { type: "ice", candidate: e.candidate });
    };

    pc.onconnectionstatechange = () => {
        if (["disconnected", "failed", "closed"].includes(pc.connectionState)) {
            setStatus("disconnected");
        }
    };
}

async function makeOffer() {
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    socket.emit("signal", { type: "offer", sdp: offer });
}

async function handleSignal(data) {
    if (!pc) createPeer();

    if (data.type === "offer") {
        await pc.setRemoteDescription(new RTCSessionDescription(data.sdp));
        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);
        socket.emit("signal", { type: "answer", sdp: answer });
    } else if (data.type === "answer") {
        await pc.setRemoteDescription(new RTCSessionDescription(data.sdp));
    } else if (data.type === "ice") {
        try {
            await pc.addIceCandidate(new RTCIceCandidate(data.candidate));
        } catch (err) {
            console.warn("ICE add failed", err);
        }
    }
}

async function start() {
    startBtn.disabled = true;
    try {
        await getMedia();
    } catch (err) {
        showSplash("Camera blocked", "Please allow camera & microphone in your browser settings.", true);
        startBtn.disabled = false;
        return;
    }

    socket = io();

    socket.on("connect", () => socket.emit("join"));

    socket.on("ready", ({ role: r }) => {
        role = r;
        if (r === "caller") {
            setStatus("waiting for peer…");
            showSplash("Waiting for peer", "Share this link to invite someone.", false);
        } else {
            setStatus("joining…");
            createPeer();
        }
    });

    socket.on("peer-joined", async () => {
        setStatus("connecting…");
        createPeer();
        await makeOffer();
    });

    socket.on("signal", handleSignal);

    socket.on("peer-left", () => {
        setStatus("peer left");
        cleanupPeer();
        showSplash("Peer left", "Waiting for someone to join…", false);
    });

    socket.on("full", () => {
        showSplash("Room full", "This call already has two participants.", false);
    });
}

function cleanupPeer() {
    if (pc) {
        pc.close();
        pc = null;
    }
    remoteVideo.srcObject = null;
}

micBtn.addEventListener("click", () => {
    const track = localStream?.getAudioTracks()[0];
    if (!track) return;
    track.enabled = !track.enabled;
    micBtn.classList.toggle("off", !track.enabled);
});

camBtn.addEventListener("click", () => {
    const track = localStream?.getVideoTracks()[0];
    if (!track) return;
    track.enabled = !track.enabled;
    camBtn.classList.toggle("off", !track.enabled);
});

endBtn.addEventListener("click", () => {
    cleanupPeer();
    if (socket) socket.disconnect();
    localStream?.getTracks().forEach((t) => t.stop());
    setStatus("call ended");
    controls.hidden = true;
    showSplash("Call ended", "Tap to start a new call", true);
    startBtn.disabled = false;
});

startBtn.addEventListener("click", start);
