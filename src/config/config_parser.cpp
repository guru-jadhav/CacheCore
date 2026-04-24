#include "config_parser.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fileParsingMessage {
    const std::string INVALID_FORMAT        = "Error: invalid config file format\n";
    const std::string NO_SUCH_FILE_FOUND    = "Error: provided file doesn't exist\n";
    const std::string ERROR_WHILE_OPENING   = "Error opening file\n";
    const std::string PROVIDE_VALID_FILE    = "Error: please provide valid config file\n";
    const std::string ERROR_WHILE_IO        = "Error: I/O error while reading file\n";
}

const std::string FILE_EXTENSION = ".conf";
const std::string PORT_PREFIX = "PORT=";
const std::string DB_PREFIX = "DB";
const std::string MAX_CAPACITY_PREFIX = "maxCapacity=";
const std::string TTL_PREFIX = "ttl=";
const std::string EVICTNTERVAL_PREFIX = "evictInterval=";

bool ConfigParser::invalidFileFormat(const std::string& configPath) {
    std::filesystem::path configFile(configPath);
    return configFile.extension() != FILE_EXTENSION;
}

bool ConfigParser::checkAllDigits(const std::string& line, int startIndex){
    //std::cout<<line.size()<<" "<<startIndex<<"\n";
    //std::cout<<"start checking digit\n";
    for(int i = startIndex; i < line.size(); i++){
        //std::cout<<line[i]<<" ";
        if(line[i] < '0' || line[i] > '9'){
            return false;
        }
    }
    return true;
}

bool ConfigParser::parsePort(const std::string& line, int startIndex, Config& config){
    //std::cout<<"in parsePort\n";
    std::string prefix = line.substr(startIndex, 5);
    //std::cout<<prefix<<"\n";
    if(prefix != PORT_PREFIX){
        return false;
    }

    //std::cout<<"before checkAllDigits\n";
    if(!checkAllDigits(line, 5)){
        // if we find invalid port - 123abd or xyz
        // this will also handle -ve port numbers
        //std::cout<<"inside if \n";
        return false;
    }
    //std::cout<<"after checkAllDigits\n";

    std::string portNoStr = line.substr(5);
    uint16_t _port;
    try {
        int parsedPort = std::stoi(portNoStr);
        // check if out of range for uint16_t
        if(parsedPort < 1 || parsedPort > 65535){
            return false;
        }

        _port = parsedPort;
    } catch (...) {
        return false;
    }

    config.port = _port;
    return true;
}

int ConfigParser::parseMaxCapacity(const std::string& line, int startIndex, LRUStoreConfig& temp){
    std::string prefix = line.substr(startIndex, 12);
    if(prefix != MAX_CAPACITY_PREFIX){
        return -1;
    }

    int i = startIndex + 12;
    std::string maxCapacityStr;

    while(i < line.size() && line[i] != ' '){
        if(line[i] < '0' || line[i] > '9'){
            return -1;
        }
        maxCapacityStr += line[i];
        i++;
    }

    size_t _maxCapacity;

    try {
        long parseMaxCapacity = std::stol(maxCapacityStr);

        if(parseMaxCapacity < 0){
            return -1;
        }

        _maxCapacity = parseMaxCapacity;
    } catch (...) {
        return -1;
    }

    temp.maxCapacity = _maxCapacity;
    return i;
}

int ConfigParser::parseTTL(const std::string& line, int startIndex, LRUStoreConfig& temp){
    std::string prefix = line.substr(startIndex, 4);
    if(prefix != TTL_PREFIX){
        return -1;
    }

    int i = startIndex + 4;
    std::string ttlStr;
    while(i < line.size() && line[i] != ' '){
        if(line[i] < '0' || line[i] > '9'){
            return -1;
        }
        ttlStr += line[i];
        i++;
    }

    size_t _ttl;

    try {
        long parseTTL = std::stol(ttlStr);
        if(parseTTL < 0){
            return -1;
        }
        _ttl = parseTTL;
    } catch (...) {
        return -1;
    }
    temp.ttl = _ttl;
    return i;
}

int ConfigParser::parseEvictInterval(const std::string& line, int startIndex, LRUStoreConfig& temp){
    std::string prefix = line.substr(startIndex, 14);
    if(prefix != EVICTNTERVAL_PREFIX){
        return -1;
    }

    int i = startIndex + 14;
    std::string evictIntervalStr;
    while(i < line.size() && line[i] != ' '){
        if(line[i] < '0' || line[i] > '9'){
            return -1;
        }
        evictIntervalStr += line[i];
        i++;
    }

    size_t _evictInterval;

    try {
        long parsedEvictInterval = std::stol(evictIntervalStr);
        if(parsedEvictInterval < 0){
            return -1;
        }

            _evictInterval = parsedEvictInterval;
    } catch (...) {
        return -1;
    }

    temp.evictInterval = _evictInterval;
    return i;
}

bool ConfigParser::parseDBConfig(const std::string& line, int startIndex, Config& config){
    //std::cout<<"in parseDBConfig\n";
    std::string prefix = line.substr(startIndex, 2);

    if(prefix != DB_PREFIX){
        return false;
    }

    int i = 3;
    while(i < line.size() && line[i] == ' '){
        i++;
    }

    LRUStoreConfig tempConfig;

    if((i = parseMaxCapacity(line, i, tempConfig)) == -1){
        return false;
    }

    while(i < line.size() && line[i] == ' '){
        i++;
    }

    if((i = parseTTL(line, i, tempConfig)) == -1){
        return false;
    }

    while(i < line.size() && line[i] == ' '){
        i++;
    }

    if((i = parseEvictInterval(line, i, tempConfig)) == -1){
        return false;
    }

    config.dbConfig.emplace_back(tempConfig);
    return true;
}

bool ConfigParser::helper(const std::string& line, Config& config){
    /*
        we can only allow:
        # - any chars
        port = any valid +ve port number - 0 - 65335
        DB maxCapacity=0 to any, ttl=0 to any, evictInterval=0 to any
        we can have any number of DB

    */

    int i = 0;
    while (i < line.size() && line[i] == ' ') {
      i++;
    }

    if(i >= line.size()){
        // blank line with all spaces
        return true;
    }

    // if(line[i] != 'P' || line[i] != 'D'){
    //     // some invalid char/corrupted config here
    //     return false;
    // }

    // //std::cout<<i<<":"<<line<<"\n";

    if(line[i] == 'P'){
        // call helper for extracting PORT number
        // if we find multiple "PORT=" then we wil override the previous port
        return parsePort(line, i, config);
    }else if (line[i] == 'D'){
        // call helper for extracing db config
        return parseDBConfig(line, i, config);
    }else{
        return false;
    }
}

std::optional<Config> ConfigParser::load(const std::string& configPath){
    if (!std::filesystem::exists(configPath)) {
        std::cerr << fileParsingMessage::NO_SUCH_FILE_FOUND;
        return std::nullopt;
    }

    if (!std::filesystem::is_regular_file(configPath)) {
        std::cerr << fileParsingMessage::PROVIDE_VALID_FILE;
        return std::nullopt;
    }

    if (invalidFileFormat(configPath)) {
        std::cerr << fileParsingMessage::INVALID_FORMAT;
        return std::nullopt;
    }

    std::ifstream configFile(configPath);

    if (!configFile.is_open()) {
        std::cerr << fileParsingMessage::ERROR_WHILE_OPENING;
        return std::nullopt;
    }

    Config config;
    std::string line;
    //std::cout<<"file open\n";
    while(std::getline(configFile, line)){
        if(line.empty() || line[0] == '#'){
            // //std::cout<<"empty line\n";
            continue;
        }
        // //std::cout<<line<<"\n";
        // parse 1 - port, 2 - DB

        if(!helper(line, config)){
            // some invalid property may have occured while parsing, we need to error out;
            return std::nullopt;
        }
    }

    // not need of .close() - destructor handles it (RAII)
    // configFile.close();

    if (configFile.bad()) {
        std::cerr << fileParsingMessage::ERROR_WHILE_IO;
        return std::nullopt;
    }

    return config;
}