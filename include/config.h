#pragma once
#include "lru_store.h"
#include <cstdint>
#include <vector>

struct Config {
    std::uint16_t port = 0;
    std::vector<LRUStoreConfig> dbConfig;
};