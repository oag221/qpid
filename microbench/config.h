#pragma once

#include <iostream>
#include <libgen.h>
#include <unistd.h>

struct config_t {
    bool order = 0;
    uint8_t max_levels = 32;
    uint16_t threads = 1;
    uint32_t buckets = 1048576; // # buckets for closed addressing unordered maps
    uint32_t chunksize = 128;  // size of chunks, for chunked data structures
    uint32_t num_queues = 128;
    uint32_t max_batch_size = 64;
    uint32_t delta = 0;
    
    uint32_t pool_reserve = 1000;
    uint32_t pool_init_chunks = 1000;

};