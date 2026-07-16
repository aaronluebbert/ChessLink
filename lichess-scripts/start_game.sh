#!/bin/bash

if [ -z "$LICHESS_TOKEN" ]; then
    echo "Error: LICHESS_TOKEN is not set."
    echo "Run: export LICHESS_TOKEN='your_token_here'"
    exit 1
fi

curl --fail-with-body https://lichess.org/api/challenge/LeelaChess \
    --request POST \
    --header "Content-Type: application/x-www-form-urlencoded" \
    --header "Authorization: Bearer $LICHESS_TOKEN" \
    --data-urlencode "clock.limit=300" \
    --data-urlencode "clock.increment=1" \
    --data-urlencode "rated=false" \
    --data-urlencode "color=white" \
    --data-urlencode "variant=standard" \
    --data-urlencode "fen=rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" \
    --data-urlencode "keepAliveStream=true"