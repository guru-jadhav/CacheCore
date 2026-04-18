#include "lru_store.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
std::atomic<bool> running{true};

struct config {
    std::uint16_t  port;
    std::vector<LRUStoreConfig> dbConfig;
};


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

bool invalidFileFormat(const std::string& configPath) {
    std::filesystem::path configFile(configPath);
    return configFile.extension() != FILE_EXTENSION;
}

bool checkAllDigits(const std::string& line, int startIndex){
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

bool parsePort(const std::string& line, int startIndex, config& config){
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

int parseMaxCapacity(const std::string& line, int startIndex, LRUStoreConfig& temp){
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

int parseTTL(const std::string& line, int startIndex, LRUStoreConfig& temp){
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

int parseEvictInterval(const std::string& line, int startIndex, LRUStoreConfig& temp){
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

bool parseDBConfig(const std::string& line, int startIndex, config& config){
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

bool helper(const std::string& line, config& config){
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

int configParser(const std::string& configPath, config& config) {

    if (!std::filesystem::exists(configPath)) {
        std::cerr << fileParsingMessage::NO_SUCH_FILE_FOUND;
        return -1;
    }

    if (!std::filesystem::is_regular_file(configPath)) {
        std::cerr << fileParsingMessage::PROVIDE_VALID_FILE;
        return -1;
    }

    if (invalidFileFormat(configPath)) {
        std::cerr << fileParsingMessage::INVALID_FORMAT;
        return -1;
    }

    std::ifstream configFile(configPath);

    if (!configFile.is_open()) {
        std::cerr << fileParsingMessage::ERROR_WHILE_OPENING;
        return -1;
    }

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
            return -1;
        }

    }

    // if (bytesRead < 0) {
    //     std::cerr << "Error: Failed to read from file.\n";
    // }

    // not need of .close() - destructor handles it (RAII)
    // configFile.close();

    if (configFile.bad()) {
        std::cerr << fileParsingMessage::ERROR_WHILE_IO;
        return -1;
    }

    return 0;
}

int main(int argc, char* argv[]) {

    if(argc < 2){
        std::cerr<<"Error: Missing configuration file.\n";
        std::cerr<<"Usage: "<<argv[0]<<" <path_to_config.json>\n";
        return EXIT_FAILURE;
    }
	
    std::string configPath = argv[1];
    std::cout<<"Initializing KV-Store using : " << configPath << "\n";
    config config;
    if(configParser(configPath, config) == -1){
        return -1;
    }

    if (config.port == 0) {
      std::cerr << "Error: PORT not specified in config\n";
      return -1;
    }
    if (config.dbConfig.empty()) {
      std::cerr << "Error: no DB instances specified in config\n";
      return -1;
    }

    std::cout << "Initialized Store with these config :\n";
    std::cout << "PORT: " << config.port << "\n";
    std::cout << "DB:\n";

    for (int i = 0; i < config.dbConfig.size(); i++) {
        std::cout << "DB[" << i << "]: ";
        std::cout << "maxCapacity="   << config.dbConfig[i].maxCapacity << ", ";
        std::cout << "ttl="           << config.dbConfig[i].ttl << ", ";
        std::cout << "evictInterval=" << config.dbConfig[i].evictInterval << "\n";
    }

    signal(SIGINT, [](int){ running = false; });

    // call server.start();

    while(running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout<<"\n";

    // call server.stop();
    return 0;
}

/*

By Default in - Blocking Mode

| Operation | What condition causes blocking? |
| --------- | ------------------------------- |
| accept    | no pending connections          |
| recv      | no data available               |
| send      | no buffer space                 |
| connect   | handshake not finished          |



we can make these calls - non-blocking
1 - Using fcntl
2 - At creation time (cleaner for new sockets
    # socket(..., SOCK_NONBLOCK, ...)
    # accept4(..., SOCK_NONBLOCK)
        - accept() 
            → returns a blocking socket 
            → you then call fcntl() to change it
        - accept4() 
            → accept4 = accept + immediate configuration
            → returns a socket with flags already set (like SOCK_NONBLOCK) 
            → accept4() makes it atomic (one step, safer, faster)
*/