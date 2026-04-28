from flask import Flask, render_template
from flask_socketio import SocketIO, emit, join_room, leave_room

app = Flask(__name__)
app.config["SECRET_KEY"] = "change-me"
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="eventlet")

ROOM = "main"
peers = []


@app.route("/")
def index():
    return render_template("index.html")


@socketio.on("join")
def on_join():
    sid = __import__("flask").request.sid
    if sid in peers:
        return
    if len(peers) >= 2:
        emit("full")
        return

    join_room(ROOM)
    peers.append(sid)

    if len(peers) == 1:
        emit("ready", {"role": "caller"})
    else:
        emit("ready", {"role": "callee"})
        emit("peer-joined", to=peers[0])


@socketio.on("signal")
def on_signal(data):
    sid = __import__("flask").request.sid
    target = next((p for p in peers if p != sid), None)
    if target:
        emit("signal", data, to=target)


@socketio.on("disconnect")
def on_disconnect():
    sid = __import__("flask").request.sid
    if sid in peers:
        peers.remove(sid)
        emit("peer-left", to=ROOM)


if __name__ == "__main__":
    socketio.run(app, host="0.0.0.0", port=5000, debug=True)
