from flask import Flask, send_from_directory, request
from flask_socketio import SocketIO, emit, join_room, leave_room
import ctypes
import os
from stockfish import Stockfish
import chess
from dataclasses import dataclass
import time
import threading
import random
import uuid

# ==========================
# Initialize Flask & SocketIO
# ==========================
app = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*")

# ==========================
# Load matchmaking DLL
# ==========================
dll_path = os.path.join(os.path.dirname(__file__), 'matchmaking.dll')

# Load DLL (cdecl exports)
matchmaking = ctypes.CDLL(dll_path)

# Constants matching the C file
UUID_LEN = 64

# ctypes struct that mirrors the C `Match`
class CMatch(ctypes.Structure):
    _fields_ = [
        ('player1', ctypes.c_char * UUID_LEN),
        ('player2', ctypes.c_char * UUID_LEN),
        ('match_id', ctypes.c_int)
    ]

# --- Function signatures (based on matchmaking.c) ---
matchmaking.init_engine.restype = None
matchmaking.init_engine.argtypes = []

# optional functions (if present in your C)
# safe to set them; if the symbol doesn't exist you'll get an AttributeError at import time
# if your DLL doesn't contain these functions, you can remove or guard them.
try:
    matchmaking.set_max_rating_diff.restype = None
    matchmaking.set_max_rating_diff.argtypes = [ctypes.c_int]
except AttributeError:
    pass

try:
    matchmaking.set_shard.restype = None
    matchmaking.set_shard.argtypes = [ctypes.c_int, ctypes.c_int]
except AttributeError:
    pass

# add_player(const char* uuid, int rating)
matchmaking.add_player.restype = None
matchmaking.add_player.argtypes = [ctypes.c_char_p, ctypes.c_int]

# add_bot(const char* uuid, int rating)
matchmaking.add_bot.restype = None
matchmaking.add_bot.argtypes = [ctypes.c_char_p, ctypes.c_int]

# remove_player(const char* uuid)
matchmaking.remove_player.restype = None
matchmaking.remove_player.argtypes = [ctypes.c_char_p]

# int get_match(Match* out)
matchmaking.get_match.restype = ctypes.c_int
matchmaking.get_match.argtypes = [ctypes.POINTER(CMatch)]

# int get_all_matches(Match* buf, int max_buf)
matchmaking.get_all_matches.restype = ctypes.c_int
matchmaking.get_all_matches.argtypes = [ctypes.POINTER(CMatch), ctypes.c_int]

# register_match_callback(match_callback_t cb) -> void
# callback signature in C: void callback(const char* p1, const char* p2, int match_id, int rating1, int rating2)
CALLBACK_TYPE = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int)
try:
    matchmaking.register_match_callback.restype = None
    matchmaking.register_match_callback.argtypes = [CALLBACK_TYPE]
except AttributeError:
    # If register_match_callback isn't present in your DLL, ignore.
    pass

# free_engine()
try:
    matchmaking.free_engine.restype = None
    matchmaking.free_engine.argtypes = []
except AttributeError:
    pass

# Initialize engine (call once)
matchmaking.init_engine()

# ------- Helper wrappers for Pythonic use ----------
def add_player(uuid_str: str, rating: int):
    print(f"🟢 [DLL] Adding player {uuid_str} ({rating})")
    matchmaking.add_player(uuid_str.encode('utf-8'), int(rating))

def remove_player(uuid_str: str):
    print(f"🔴 [DLL] Removing player {uuid_str}")
    matchmaking.remove_player(uuid_str.encode('utf-8'))

def add_bot(uuid_str: str, rating: int):
    print(f"🤖 [DLL] Adding bot {uuid_str} ({rating})")
    matchmaking.add_bot(uuid_str.encode('utf-8'), int(rating))

def try_get_match():
    m = CMatch()
    ok = matchmaking.get_match(ctypes.byref(m))
    if not ok:
        print("⚠️ [DLL] No match found this time")
        return None
    # decode UUIDs
    p1 = bytes(m.player1).split(b'\x00', 1)[0].decode('utf-8')
    p2 = bytes(m.player2).split(b'\x00', 1)[0].decode('utf-8')
    print(f"✅ [DLL] Match created: {p1} vs {p2}, match_id={m.match_id}")
    return {'player1': p1, 'player2': p2, 'match_id': int(m.match_id)}

def get_all_matches(limit=100):
    arr_type = CMatch * limit
    arr = arr_type()
    count = matchmaking.get_all_matches(arr, limit)
    res = []
    for i in range(count):
        mm = arr[i]
        p1 = bytes(mm.player1).split(b'\x00',1)[0].decode('utf-8')
        p2 = bytes(mm.player2).split(b'\x00',1)[0].decode('utf-8')
        res.append({'match_id': int(mm.match_id), 'player1': p1, 'player2': p2})
    return res

# Optional: register Python callback to receive async match logs/events from the DLL logger thread.
_py_cb_ref = None  # keep reference so GC doesn't collect it
def register_match_callback(py_func):
    global _py_cb_ref
    def _cb(p1, p2, match_id, r1, r2):
        s1 = p1.decode('utf-8') if p1 else ''
        s2 = p2.decode('utf-8') if p2 else ''
        try:
            py_func(s1, s2, int(match_id), int(r1), int(r2))
        except Exception as e:
            print("Callback error:", e)
    _py_cb_ref = CALLBACK_TYPE(_cb)
    try:
        matchmaking.register_match_callback(_py_cb_ref)
    except Exception as e:
        print("register_match_callback failed:", e)

# ==========================
# Stockfish config
# ==========================
stockfish_path = "stockfish/stockfish-windows-x86-64.exe"
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

# ==========================
# Global variables
# ==========================
clients_lock = threading.Lock()
clients = {}        # player_id -> {'sid': ..., 'skill': ..., 'name': ..., 'wants_bot': ..., 'auto_bot_after_timeout': ...}
active_games = {}   # player_id -> room name

player_timer_threads = {}  # player_id -> threading.Timer
toss_results = {}          # room -> {'winner': player_id, 'loser': player_id}
game_colors = {}           # room -> {player_id: 'white'/'black'}

@dataclass
class GameSession:
    board: chess.Board
    engine: Stockfish
    difficulty: str
    is_game_over: bool = False
    moves_history: list = None

game_sessions = {}  # player_id -> GameSession

# ==========================
# Auto-bot timer function
# ==========================
def start_bot_timer(player_id, difficulty='expert', timeout=30):
    def assign_bot():
        with clients_lock:
            if player_id in active_games:
                print(f"⚠️ Player {player_id} already in a game, skipping bot assign")
                return
            if player_id not in clients:
                print(f"⚠️ Player {player_id} disconnected, skipping bot assign")
                return

            sf = get_stockfish_instance(difficulty)
            board = chess.Board()
            session = GameSession(board=board, engine=sf, difficulty=difficulty, moves_history=[])
            game_sessions[player_id] = session
            room = f"bot_game_{player_id}"
            join_room(room, sid=clients[player_id]['sid'])
            active_games[player_id] = room

            print(f"🤖 Auto-bot assigned to {player_id} | Room: {room}")
            socketio.emit('start_game', {'color': 'white', 'opponent': 'Bot', 'opponent_skill': 0}, to=clients[player_id]['sid'])

    t = threading.Timer(timeout, assign_bot)
    player_timer_threads[player_id] = t
    t.start()

# ==========================
# Flask routes
# ==========================
@app.route('/')
def serve_index():
    return send_from_directory('../frontend', 'index.html')

@app.route('/<path:path>')
def serve_static_files(path):
    return send_from_directory('../frontend', path)




@socketio.on('debug_matches')
def handle_debug_matches():
    try:
        matches = get_all_matches()
        print("🔎 Current DLL matches:", matches)
        emit('debug_matches', matches)
    except Exception as e:
        print("DLL debug error:", e)
        emit('debug_matches', [])


# ==========================
# SocketIO events
# ==========================
@socketio.on('connect')
def handle_connect():
    print("✅ Client connected:", request.sid)
    player_id = str(uuid.uuid4())
    socketio.emit('set_player_id', {'player_id': player_id}, to=request.sid)

@socketio.on('disconnect')
def handle_disconnect():
    sid = request.sid
    print("❌ Client disconnected:", sid)

    with clients_lock:
        for player_id, info in list(clients.items()):
            if info['sid'] == sid:
                print(f"Removing player {player_id} from matchmaking")
                try:
                    remove_player(player_id)
                except Exception as e:
                    print("DLL remove_player error:", e)

                # Cancel bot timer if exists
                if player_timer_threads.get(player_id):
                    try:
                        player_timer_threads[player_id].cancel()
                    except Exception:
                        pass
                    player_timer_threads.pop(player_id, None)

                clients.pop(player_id)
                room = active_games.pop(player_id, None)
                if room:
                    try:
                        leave_room(room, sid=sid)
                    except Exception:
                        pass
                game_sessions.pop(player_id, None)
                break

@socketio.on('add_player')
def handle_add_player(data):
    player_id = data['player_id']
    skill = data['skill']
    name = data.get('name', f"Player{player_id}")
    wants_bot = data.get('bot', False)
    difficulty = data.get('difficulty', 'expert')
    auto_bot_after_timeout = data.get('auto_bot_after_timeout', False)
    timeout = data.get('timeout', 30)

    with clients_lock:
        if player_id in active_games:
            print(f"⚠️ Player {name} is already in active game")
            return

        clients[player_id] = {
            'sid': request.sid,
            'skill': skill,
            'name': name,
            'wants_bot': wants_bot,
            'auto_bot_after_timeout': auto_bot_after_timeout
        }

    # Manual Bot Mode
    if wants_bot and not auto_bot_after_timeout:
        sf = get_stockfish_instance(difficulty)
        board = chess.Board()
        session = GameSession(board=board, engine=sf, difficulty=difficulty, moves_history=[])
        game_sessions[player_id] = session

        room = f"bot_game_{player_id}"
        join_room(room, sid=request.sid)
        active_games[player_id] = room

        print(f"🤖 Player {name} matched with Bot | Room: {room}")
        socketio.emit('start_game', {'color': 'white', 'opponent': 'Bot', 'opponent_skill': 0}, to=request.sid)
        return

    # Auto Bot Timer
    if wants_bot and auto_bot_after_timeout:
        start_bot_timer(player_id, difficulty, timeout)

    # Human Matchmaking
    if not wants_bot:
        print(f"📥 Player {name} ({skill}) joined matchmaking")
        try:
            # use wrapper that encodes uuid
            add_player(player_id, skill)
        except Exception as e:
            print("DLL add_player error:", e)

        # Cancel existing bot timer if any
        if player_timer_threads.get(player_id):
            try:
                player_timer_threads[player_id].cancel()
            except Exception:
                pass
            player_timer_threads.pop(player_id, None)

        # Check for match immediately
                # Check for match(s) immediately — drain as many matches as available
                # Drain all matches immediately
        try:
            while True:
                match = CMatch()
                try:
                    ok = bool(matchmaking.get_match(ctypes.byref(match)))
                except Exception as ex:
                    print("DLL get_match error:", ex)
                    break

                if not ok:
                    break

                p1 = bytes(match.player1).split(b'\x00', 1)[0].decode('utf-8')
                p2 = bytes(match.player2).split(b'\x00', 1)[0].decode('utf-8')
                match_id = int(match.match_id)
                room = f"game_{match_id}"

                # ====== THREAD SAFE PART ======
                with clients_lock:
                    info1 = clients.get(p1)
                    info2 = clients.get(p2)

                    if (p1 in active_games) or (p2 in active_games):
                        print(f"⚠️ One of {p1},{p2} already in active game. Skipping {match_id}")
                        continue

                    # Skip manual bot users
                    if info1 and info1.get('wants_bot') and not info1.get('auto_bot_after_timeout'):
                        print(f"⚠️ {p1} is manual-bot only. Skipping {match_id}")
                        continue
                    if info2 and info2.get('wants_bot') and not info2.get('auto_bot_after_timeout'):
                        print(f"⚠️ {p2} is manual-bot only. Skipping {match_id}")
                        continue

                    if not info1 and not info2:
                        print(f"⚠️ Both players missing in clients. Skipping {p1},{p2}")
                        continue

                    # Reserve game room immediately
                    active_games[p1] = room
                    active_games[p2] = room

                # ====== NETWORK OPS OUTSIDE LOCK ======
                try:
                    if info1:
                        join_room(room, sid=info1['sid'])
                    if info2:
                        join_room(room, sid=info2['sid'])
                except Exception as e:
                    print("join_room error:", e)

                if info1 and info2:
                    sid1, sid2 = info1['sid'], info2['sid']
                    name1, name2 = info1.get('name', p1), info2.get('name', p2)
                    skill1, skill2 = info1.get('skill', 0), info2.get('skill', 0)

                    print(f"🎯 Match found! {name1} ({skill1}) vs {name2} ({skill2}) | Match ID: {match_id}")

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

                    # Toss result safely
                    with clients_lock:
                        winner_id = random.choice([p1, p2])
                        loser_id = p1 if winner_id == p2 else p2
                        toss_results[room] = {'winner': winner_id, 'loser': loser_id}

                    socketio.emit('toss_result', {'winner': winner_id, 'loser': loser_id}, room=room)

                else:
                    print(f"ℹ️ Partial match: {p1} present={bool(info1)}, {p2} present={bool(info2)}")


        except Exception as e:
            print("DLL get_match overarching error:", e)


# ===== Color choice event =====
@socketio.on('color_choice')
def handle_color_choice(data):
    chosen_player = data.get('player_id')
    chosen_color = data.get('color')

    if chosen_player is None or chosen_color not in ('white', 'black'):
        print("Invalid color_choice data:", data)
        return

    # Find room
    found_room = None
    for room, tr in toss_results.items():
        if tr.get('winner') == chosen_player:
            found_room = room
            break

    if not found_room:
        print("No toss_result room found for winner:", chosen_player)
        return

    tr = toss_results[found_room]
    winner_id = tr['winner']
    loser_id = tr['loser']

    winner_color = chosen_color
    loser_color = 'black' if winner_color == 'white' else 'white'
    game_colors[found_room] = {winner_id: winner_color, loser_id: loser_color}

    with clients_lock:
        winner_sid = clients.get(winner_id, {}).get('sid')
        loser_sid = clients.get(loser_id, {}).get('sid')

    if winner_sid:
        socketio.emit('assign_color', {'color': winner_color}, to=winner_sid)
    if loser_sid:
        socketio.emit('assign_color', {'color': loser_color}, to=loser_sid)

    print(f"Colors assigned in {found_room}: {winner_id}->{winner_color}, {loser_id}->{loser_color}")
    toss_results.pop(found_room, None)

# ===== Move events =====
@socketio.on('move')
def handle_move(data):
    player_id = data.get("player_id")
    move = data.get("move")
    room = active_games.get(player_id)

    if room:
        emit("opponent_move", move, room=room, include_self=False)
        emit("play_move_sound", to=room)
        print(f"♟️ Move from {player_id} in room {room}: {move}")
    else:
        print(f"⚠️ No active room for player {player_id}")

# ===== AI Move =====
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

    start_time = time.time()
    best_move = session.engine.get_best_move()
    end_time = time.time()
    thinking_time = round(end_time - start_time, 2)

    if not best_move:
        emit('ai_move', {'move': None}, to=request.sid)
        return

    uci_move = chess.Move.from_uci(best_move)
    if not session.board.is_legal(uci_move):
        emit('ai_move', {'move': None, 'status': 'Invalid move'}, to=request.sid)
        return

    session.board.push(uci_move)
    session.moves_history.append(best_move)

    move = {
        'from': best_move[:2],
        'to': best_move[2:4],
        'promotion': best_move[4] if len(best_move) > 4 else None
    }

    # Game over checks
    if session.board.is_checkmate():
        session.is_game_over = True
        emit('ai_move', {'move': move, 'status': 'checkmate', 'winner': 'ai', 'thinking_time': thinking_time}, to=request.sid)
        emit("play_win_sound", to=request.sid)
        with clients_lock:
            game_sessions.pop(player_id, None)
            active_games.pop(player_id, None)
        return
    elif session.board.is_stalemate() or session.board.is_insufficient_material():
        session.is_game_over = True
        emit('ai_move', {'move': move, 'status': 'draw', 'thinking_time': thinking_time}, to=request.sid)
        with clients_lock:
            game_sessions.pop(player_id, None)
            active_games.pop(player_id, None)
        return

    emit('ai_move', {'move': move, 'thinking_time': thinking_time}, to=request.sid)

# ===== Get all matches =====
@socketio.on('get_all_matches')
def handle_get_all_matches():
    matches_array = (CMatch * 1000)()
    try:
        count = matchmaking.get_all_matches(matches_array, 1000)
        result = []
        for i in range(count):
            m = matches_array[i]
            p1 = bytes(m.player1).split(b'\x00', 1)[0].decode('utf-8')
            p2 = bytes(m.player2).split(b'\x00', 1)[0].decode('utf-8')
            result.append({'match_id': m.match_id, 'player1_id': p1, 'player2_id': p2})
        emit('all_matches', result)
    except Exception as e:
        print("DLL get_all_matches error:", e)
        emit('all_matches', [])

# ==========================
# Run server
# ==========================
if __name__ == '__main__':
    print(f"🔧 Loaded DLL from: {dll_path}")
    socketio.run(app, debug=True, port=5000)
