from fastapi import FastAPI
from pydantic import BaseModel
import chess

app = FastAPI()

board = chess.Board()

class MoveRequest(BaseModel):
    move: str

@app.get("/")
def home():
    return {"message": "Chess server is running"}

@app.get("/board")
def get_board():
    return {
        "fen": board.fen(),
        "turn": "white" if board.turn == chess.WHITE else "black",
        "check": board.is_check(),
        "checkmate": board.is_checkmate(),
        "legal_moves": [move.uci() for move in board.legal_moves]
    }

@app.post("/move")
def make_move(req: MoveRequest):
    try:
        move = chess.Move.from_uci(req.move)
    except:
        return {
            "valid": False,
            "message": "Move format is wrong. Use UCI format like e2e4."
        }

    if move in board.legal_moves:
        board.push(move)
        return {
            "valid": True,
            "move": req.move,
            "fen": board.fen(),
            "turn": "white" if board.turn == chess.WHITE else "black",
            "check": board.is_check(),
            "checkmate": board.is_checkmate(),
            "legal_moves": [m.uci() for m in board.legal_moves]
        }

    return {
        "valid": False,
        "message": "Illegal move",
        "move": req.move,
        "fen": board.fen(),
        "turn": "white" if board.turn == chess.WHITE else "black",
        "check": board.is_check(),
        "checkmate": board.is_checkmate(),
        "legal_moves": [m.uci() for m in board.legal_moves]
    }

@app.post("/reset")
def reset_board():
    board.reset()
    return {
        "message": "Board reset",
        "fen": board.fen(),
        "turn": "white",
        "legal_moves": [m.uci() for m in board.legal_moves]
    }