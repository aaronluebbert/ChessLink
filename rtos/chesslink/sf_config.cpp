#include "sf_config.h"

// same presets as his CONNECT_ITALL screen
const SfTimePreset SF_TIME_PRESETS[] = {
    {  60, 0, "1+0"  },
    { 180, 0, "3+0"  },
    { 300, 0, "5+0"  },
    { 300, 3, "5+3"  },
    { 600, 0, "10+0" },
    { 600, 5, "10+5" },
    { 900,10, "15+10"},
};
const int SF_TIME_COUNT = (int)(sizeof(SF_TIME_PRESETS)/sizeof(SF_TIME_PRESETS[0]));

const char *const SF_COLOR_LABELS[3] = { "White", "Black", "Random" };
const char *const SF_COLOR_VALUES[3] = { "white", "black", "random" };
