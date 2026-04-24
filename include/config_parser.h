#pragma once
#include "config.h"
#include <optional>
#include <string>

/**
 * @brief Stateless utility class acting as a scoped namespace with access control.
 * All methods are static since no instance state exists — instantiating this class
 * would be meaningless. The class boundary (over a namespace) exists solely to
 * enforce access control, keeping internal parsing helpers private from callers.
 *
 * @note Cannot be instantiated meaningfully — all interaction is via ConfigParser::load().
 */

class ConfigParser {
    private:
        static bool invalidFileFormat(const std::string& configPath);
        static bool checkAllDigits(const std::string& line, int startIndex);
        static bool parsePort(const std::string& line, int startIndex, Config& config);
        static int parseMaxCapacity(const std::string& line, int startIndex, LRUStoreConfig& temp);
        static int parseTTL(const std::string& line, int startIndex, LRUStoreConfig& temp);
        static int parseEvictInterval(const std::string& line, int startIndex, LRUStoreConfig& temp);
        static bool parseDBConfig(const std::string& line, int startIndex, Config& config);
        static bool helper(const std::string& line, Config& config);
        
    public:
        static std::optional<Config> load(const std::string& configPath);
};