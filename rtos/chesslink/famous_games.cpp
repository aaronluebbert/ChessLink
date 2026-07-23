// famous_games.cpp -- the game data
//
// classic decisive games, each ending in checkmate so the replay finishes with
// the real mating move on the board. every line here is verified legal by
// replaying it through the engine (see test_famous.cpp).

#include "famous_games.h"

const FamousGame FAMOUS_GAMES[] = {
    {
        "MAGvsGABRIEL",   // Morphy, Paris Opera 1858 -- the "Opera Game"
        0,
        {
            "e2e4","e7e5",  "g1f3","d7d6", "d2d4","c8g4",  "d4e5","g4f3",
            "d1f3","d6e5",  "f1c4","g8f6", "f3b3","d8e7",  "b1c3","c7c6",
            "c1g5","b7b5",  "c3b5","c6b5", "c4b5","b8d7",  "e1c1","a8d8",
            "d1d7","d8d7",  "h1d1","e7e6", "b5d7","f6d7",  "b3b8","d7b8",
            "d1d8", 0
        },
        33, true
    },
    {
        "Scholar Mate",   // the 4-move mate, a teaching classic
        0,
        {
            "e2e4","e7e5", "f1c4","b8c6", "d1h5","g8f6", "h5f7", 0
        },
        7, true
    },
    {
        "Legal Mate",     // Legal vs Saint Brie, Paris 1750 -- the queen sac
        0,
        {
            "e2e4","e7e5", "g1f3","d7d6", "f1c4","c8g4", "b1c3","g7g6",
            "f3e5","g4d1", "c4f7","e8e7", "c3d5", 0
        },
        13, true
    },
    {
        "Immortal Game", // Anderssen vs Kieseritzky, London 1851
        0,
        {
            "e2e4","e7e5",  "f2f4","e5f4",  "f1c4","d8h4", "e1f1","b7b5",
            "c4b5","g8f6",  "g1f3","h4h6",  "d2d3","f6h5", "f3h4","h6g5",
            "h4f5","c7c6",  "g2g4","h5f6",  "h1g1","c6b5", "h2h4","g5g6",
            "h4h5","g6g5",  "d1f3","f6g8",  "c1f4","g5f6", "b1c3","f8c5",
            "c3d5","f6b2",  "f4d6","c5g1",  "e4e5","b2a1", "f1e2","b8a6",
            "f5g7","e8d8",  "f3f6","g8f6",  "d6e7", 0
        },
        45, true
    },
};

const int FAMOUS_GAME_COUNT = (int)(sizeof(FAMOUS_GAMES) / sizeof(FAMOUS_GAMES[0]));
