"use strict";

/* callVideo frontend: no framework, just the browser's native WebRTC APIs.
 * The C server only relays the JSON messages below between the two peers
 * in a room; it never sees any audio/video. */

const ICE_SERVERS = [{ urls: "stun:stun.l.google.com:19302" }];

const joinScreen = document.getElementById("join-screen");
const callScreen = document.getElementById("call-screen");
const roomInput = document.getElementById("room-input");
const joinBtn = document.getElementById("join-btn");
const joinError = document.getElementById("join-error");
const localVideo = document.getElementById("local-video");
const remoteVideo = document.getElementById("remote-video");
const remotePlaceholder = document.getElementById("remote-placeholder");
const micBtn = document.getElementById("mic-btn");
const camBtn = document.getElementById("cam-btn");
const hangupBtn = document.getElementById("hangup-btn");
const statusEl = document.getElementById("status");

let ws = null;
let pc = null;
let localStream = null;
let micOn = true;
let camOn = true;

function setStatus(text) {
  statusEl.textContent = text || "";
}

function showJoinError(text) {
  joinError.textContent = text;
  joinError.classList.remove("hidden");
}

function sendSignal(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  }
}

function newPeerConnection() {
  const conn = new RTCPeerConnection({ iceServers: ICE_SERVERS });

  conn.onicecandidate = (event) => {
    if (event.candidate) {
      sendSignal({ type: "candidate", candidate: event.candidate });
    }
  };

  conn.ontrack = (event) => {
    remoteVideo.srcObject = event.streams[0];
    remotePlaceholder.classList.add("hidden");
  };

  conn.onconnectionstatechange = () => {
    if (["disconnected", "failed", "closed"].includes(conn.connectionState)) {
      setStatus("Connexion avec l'autre personne perdue.");
    } else if (conn.connectionState === "connected") {
      setStatus("");
    }
  };

  if (localStream) {
    for (const track of localStream.getTracks()) {
      conn.addTrack(track, localStream);
    }
  }

  return conn;
}

async function handleSignal(msg) {
  switch (msg.type) {
    case "peer-joined": {
      // We were already in the room: the newcomer waits for our offer.
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      sendSignal({ type: "offer", sdp: pc.localDescription });
      break;
    }
    case "offer": {
      await pc.setRemoteDescription(new RTCSessionDescription(msg.sdp));
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      sendSignal({ type: "answer", sdp: pc.localDescription });
      break;
    }
    case "answer": {
      await pc.setRemoteDescription(new RTCSessionDescription(msg.sdp));
      break;
    }
    case "candidate": {
      try {
        await pc.addIceCandidate(new RTCIceCandidate(msg.candidate));
      } catch (err) {
        console.warn("addIceCandidate failed", err);
      }
      break;
    }
    case "peer-left": {
      remoteVideo.srcObject = null;
      remotePlaceholder.classList.remove("hidden");
      setStatus("L'autre personne a quitté l'appel.");
      if (pc) pc.close();
      pc = newPeerConnection();
      break;
    }
    case "error": {
      setStatus(msg.message || "Erreur du serveur de signalisation.");
      break;
    }
    default:
      break;
  }
}

async function joinRoom(room) {
  try {
    localStream = await navigator.mediaDevices.getUserMedia({ video: true, audio: true });
  } catch (err) {
    showJoinError("Impossible d'accéder à la caméra/micro : " + err.message);
    return;
  }

  localVideo.srcObject = localStream;

  joinScreen.classList.add("hidden");
  callScreen.classList.remove("hidden");
  remotePlaceholder.classList.remove("hidden");

  pc = newPeerConnection();

  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  ws = new WebSocket(`${proto}//${location.host}/ws?room=${encodeURIComponent(room)}`);

  ws.onopen = () => setStatus("En attente de l'autre personne…");

  ws.onmessage = (event) => {
    let msg;
    try {
      msg = JSON.parse(event.data);
    } catch {
      return;
    }
    handleSignal(msg);
  };

  ws.onclose = () => setStatus("Connexion au serveur interrompue.");
  ws.onerror = () => setStatus("Erreur de connexion au serveur.");
}

function stopLocalStream() {
  if (localStream) {
    for (const track of localStream.getTracks()) track.stop();
    localStream = null;
  }
}

function leaveCall() {
  if (ws) {
    ws.close();
    ws = null;
  }
  if (pc) {
    pc.close();
    pc = null;
  }
  stopLocalStream();

  localVideo.srcObject = null;
  remoteVideo.srcObject = null;
  remotePlaceholder.classList.remove("hidden");
  setStatus("");

  micOn = true;
  camOn = true;
  micBtn.classList.remove("off");
  camBtn.classList.remove("off");

  callScreen.classList.add("hidden");
  joinScreen.classList.remove("hidden");
  joinError.classList.add("hidden");
}

joinBtn.addEventListener("click", () => {
  const room = roomInput.value.trim();
  joinError.classList.add("hidden");
  if (!room) {
    showJoinError("Entre un code de salon.");
    return;
  }
  joinBtn.disabled = true;
  joinRoom(room).finally(() => {
    joinBtn.disabled = false;
  });
});

roomInput.addEventListener("keydown", (e) => {
  if (e.key === "Enter") joinBtn.click();
});

micBtn.addEventListener("click", () => {
  if (!localStream) return;
  micOn = !micOn;
  for (const track of localStream.getAudioTracks()) track.enabled = micOn;
  micBtn.classList.toggle("off", !micOn);
});

camBtn.addEventListener("click", () => {
  if (!localStream) return;
  camOn = !camOn;
  for (const track of localStream.getVideoTracks()) track.enabled = camOn;
  camBtn.classList.toggle("off", !camOn);
});

hangupBtn.addEventListener("click", leaveCall);

window.addEventListener("beforeunload", () => {
  if (ws) ws.close();
});
