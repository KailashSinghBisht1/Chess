from flask import Flask, send_from_directory, request
from flask_socketio import SocketIO, emit, join_room, leave_room
import ctypes
import os
from stockfish import Stockfish
import chess
from dataclasses import dataclass
import time

# Initialize Flask app
app = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*")

# Load matchmaking DLL
dll_path = os.path.join(os.path.dirname(__file__), 'matchmaking.dll')
matchmaking = ctypes.CDLL(dll_path)

# Define C-style Match struct
class Match(ctypes.Structure):
    _fields_ = [
        ('player1_id', ctypes.c_int),
        ('player2_id', ctypes.c_int),
        ('match_id', ctypes.c_int)
    ]

# Declare DLL function signatures
matchmaking.init_engine.restype = None
matchmaking.add_player.argtypes = [ctypes.c_int, ctypes.c_int]
matchmaking.get_match.argtypes = [ctypes.POINTER(Match)]
matchmaking.get_match.restype = ctypes.c_int

# Initialize matchmaking engine
matchmaking.init_engine()

# Stockfish path
stockfish_path = "stockfish/stockfish-windows-x86-64.exe"

# Difficulty settings
difficulty_settings = {
    'easy':    {'depth': 5,  'skill': 5,  'threads': 1, 'thinking_time': 1000},
    'medium':  {'depth': 10, 'skill': 10, 'threads': 2, 'thinking_time': 2000},
    'hard':    {'depth': 15, 'skill': 15, 'threads': 3, 'thinking_time': 3000},
    'expert':  {'depth': 20, 'skill': 20, 'threads': 4, 'thinking_time': 3500},
}

def get_stockfish_instance(difficulty='expert'):
    config = difficulty_settings.get(difficulty, difficulty_settings['expert'])

    sf = Stockfish(
        path=stockfish_path,
        parameters={
            "Threads": config['threads'],
            "Minimum Thinking Time": config['thinking_time'],
            "Skill Level": config['skill'],
            "Contempt": 0,
            "Ponder": True
        }
    )
    sf.set_depth(config['depth'])

    return sf

clients = {}        # player_id -> {'sid': ..., 'skill': ..., 'name': ...}
active_games = {}   # player_id -> room name

@dataclass
class GameSession:
    board: chess.Board
    engine: Stockfish
    difficulty: str
    is_game_over: bool = False
    moves_history: list = None

game_sessions = {}  # player_id -> GameSession

@app.route('/')
def serve_index():
    return send_from_directory('../frontend', 'index.html')

@app.route('/<path:path>')
def serve_static_files(path):
    return send_from_directory('../frontend', path)

@socketio.on('connect')
def handle_connect():
    print("✅ Client connected:", request.sid)

@socketio.on('disconnect')
def handle_disconnect():
    sid = request.sid
    print("❌ Client disconnected:", sid)

@socketio.on('add_player')
def handle_add_player(data):
    player_id = data['player_id']
    skill = data['skill']
    name = data.get('name', f"Player{player_id}")

    clients[player_id] = {
        'sid': request.sid,
        'skill': skill,
        'name': name
    }

    print(f"📥 Player {name} ({skill}) joined matchmaking")
    matchmaking.add_player(player_id, skill)

    match = Match()
    while matchmaking.get_match(ctypes.byref(match)):
        p1, p2, match_id = match.player1_id, match.player2_id, match.match_id

        info1 = clients.get(p1)
        info2 = clients.get(p2)

        if info1 and info2:
            sid1 = info1['sid']
            sid2 = info2['sid']
            skill1 = info1['skill']
            skill2 = info2['skill']
            name1 = info1['name']
            name2 = info2['name']

            print(f"🎯 Match found! {name1} ({skill1}) vs {name2} ({skill2}) | Match ID: {match_id}")

            room = f"game_{match_id}"
            join_room(room, sid=sid1)
            join_room(room, sid=sid2)

            active_games[p1] = room
            active_games[p2] = room

            socketio.emit('start_game', {
                'color': 'white',
                'opponent': name2,
                'opponent_skill': skill2
            }, to=sid1)

            socketio.emit('start_game', {
                'color': 'black',
                'opponent': name1,
                'opponent_skill': skill1
            }, to=sid2)

        else:
            print("⚠️ One or both matched players are not connected")

@socketio.on('move')
def handle_move(data):
    player_id = data.get("player_id")
    move = data.get("move")
    room = active_games.get(player_id)

    if room:
        emit("opponent_move", move, room=room, include_self=False)
        print(f"♟️ Move from {player_id} in room {room}: {move}")
        emit("play_move_sound", to=room)
    else:
        print(f"⚠️ No active room for player {player_id}")

@socketio.on('get_ai_move')
def handle_ai_move(data):
    player_id = data.get('player_id')
    difficulty = data.get('difficulty', 'expert')
    fen = data.get('fen')

    if not player_id or not fen:
        emit('error', {'message': 'Invalid request'})
        return

    depth = difficulty_settings.get(difficulty, difficulty_settings['expert'])['depth']

    if player_id not in game_sessions:
        board = chess.Board(fen)
        engine = get_stockfish_instance(difficulty)
        session = GameSession(board=board, engine=engine, difficulty=difficulty, moves_history=[])
        game_sessions[player_id] = session
    else:
        session = game_sessions[player_id]
        session.board.set_fen(fen)

    if session.is_game_over:
        emit('ai_move', {'move': None, 'status': 'Game is already over'}, to=request.sid)
        return

    session.engine.set_fen_position(session.board.fen())
    session.engine.set_depth(depth)

    print(f"[{difficulty.upper()}] Bot Thinking | Depth: {depth} | FEN: {session.board.fen()}")
    start_time = time.time()
    best_move = session.engine.get_best_move()
    end_time = time.time()

    thinking_time = round(end_time - start_time, 2)
    print(f"Bot thinking time: {thinking_time} seconds")
    print(f"Best AI Move: {best_move}")

    if best_move is None:
        emit('ai_move', {'move': None}, to=request.sid)
        return

    uci_move = chess.Move.from_uci(best_move)
    if uci_move not in session.board.legal_moves:
        emit('ai_move', {'move': None, 'status': 'Invalid move'}, to=request.sid)
        return

    session.board.push(uci_move)
    session.moves_history.append(best_move)

    move = {
        'from': best_move[:2],
        'to': best_move[2:4],
        'promotion': best_move[4] if len(best_move) > 4 else None
    }

    if session.board.is_checkmate():
        session.is_game_over = True
        emit('ai_move', {
            'move': move,
            'status': 'checkmate',
            'winner': 'ai',
            'thinking_time': thinking_time
        }, to=request.sid)
        emit("play_win_sound", to=request.sid)
        print("♟️ Checkmate delivered by AI.")
        return

    elif session.board.is_stalemate() or session.board.is_insufficient_material():
        session.is_game_over = True
        emit('ai_move', {
            'move': move,
            'status': 'draw',
            'thinking_time': thinking_time
        }, to=request.sid)
        print("🤝 Game ended in a draw.")
        return

    emit('ai_move', {
        'move': move,
        'thinking_time': thinking_time
    }, to=request.sid)

# Run Server
if __name__ == '__main__':
    print(f"🔧 Loaded DLL from: {dll_path}")
    socketio.run(app, debug=True, port=5000)
