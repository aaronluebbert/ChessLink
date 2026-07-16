#!/bin/bash

if [ -z "$LICHESS_TOKEN" ]; then
    echo "Error: LICHESS_TOKEN is not set."
    echo "Run: export LICHESS_TOKEN='your_token_here'"
    exit 1
fi

if [ -z "$1" ]; then
    echo "Usage: ./stop_game.sh GAME_ID"
    exit 1
fi

GAME_ID="$1"

curl --fail-with-body "https://lichess.org/api/board/game/$GAME_ID/abort" \
    --request POST \
    --header "Authorization: Bearer $LICHESS_TOKEN"