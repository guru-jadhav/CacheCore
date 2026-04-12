#pragma once
#include "lru_store.h"
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
std::atomic<bool> running{true};

struct config {
    std::size_t port;
    std::vector<LRUStoreConfig> dbConfig;
};

void configParser(std::string& configPath, config& config){

    int fileFd = open(configPath.c_str(), O_RDONLY);

    if(fileFd < 0){
        perror("Error opening file");
        return;
    }

    char buffer[4096];
    ssize_t bytesRead;


    while ((bytesRead = read(fileFd, buffer, sizeof(buffer))) > 0) {
        std::cout.write(buffer, bytesRead);
    }
    std::cout<<"\n";

    if (bytesRead < 0) {
        std::cerr << "Error: Failed to read from file.\n";
    }

    close(fileFd);


    // pending - config.json parser and store initialization
}

int main(int argc, char* argv[]) {

    if(argc < 2){
        std::cerr<<"Error: Missing configuration file.\n";
        std::cerr<<"Usage: "<<argv[0]<<" <path_to_config.json>\n";
        return EXIT_FAILURE;
    }
	
    std::string configPath = argv[1];
    std::cout<<"Initializing KV-Store using config: " << configPath << "\n";
    config config;
    configParser(configPath, config);



    signal(SIGINT, [](int){ running = false; });

    // call server.start();

    while(running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // call server.stop();
    return 0;
}