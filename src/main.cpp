#include "config_parser.h"
#include "tcp_server.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
std::atomic<bool> running{true};

void printConfig(const Config& config) {
    std::cout << "Initialized Store with these config :\n";
    std::cout << "PORT: " << config.port << "\n";
    std::cout << "DB:\n";
    for (int i = 0; i < config.dbConfig.size(); i++) {
        std::cout << "DB[" << i << "]: ";
        std::cout << "maxCapacity=" << config.dbConfig[i].maxCapacity << ", ";
        std::cout << "ttl=" << config.dbConfig[i].ttl << ", ";
        std::cout << "evictInterval=" << config.dbConfig[i].evictInterval << "\n";
    }
}

int main(int argc, char* argv[]) {

    if(argc < 2){
        std::cerr<<"Error: Missing configuration file.\n";
        std::cerr<<"Usage: "<<argv[0]<<" <path_to_config.json>\n";
        return EXIT_FAILURE;
    }


    std::string configPath = argv[1];
    std::cout<<"Initializing CacheCore using : " << configPath << "\n";

    auto config = ConfigParser::load(configPath);
    if(!config){
        return -1;
    }

    if (config->port == 0) {
      std::cerr << "Error: PORT not specified in config\n";
      return -1;
    }
    if (config->dbConfig.empty()) {
      std::cerr << "Error: no DB instances specified in config\n";
      return -1;
    }


    printConfig(*config);

    signal(SIGINT, [](int){ running = false; });

    TCPServer server = TCPServer(*config);
    server.start();

    while(running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();

    return 0;
}