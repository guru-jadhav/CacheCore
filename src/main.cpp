#include "config_parser.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

std::atomic<bool> running{true};

int main(int argc, char* argv[]) {

    if(argc < 2){
        std::cerr<<"Error: Missing configuration file.\n";
        std::cerr<<"Usage: "<<argv[0]<<" <path_to_config.json>\n";
        return EXIT_FAILURE;
    }

    std::string configPath = argv[1];
    std::cout<<"Initializing KV-Store using : " << configPath << "\n";

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

    std::cout << "Initialized Store with these config :\n";
    std::cout << "PORT: " << config->port << "\n";
    std::cout << "DB:\n";

    for (int i = 0; i < config->dbConfig.size(); i++) {
        std::cout << "DB[" << i << "]: ";
        std::cout << "maxCapacity="   << config->dbConfig[i].maxCapacity << ", ";
        std::cout << "ttl="           << config->dbConfig[i].ttl << ", ";
        std::cout << "evictInterval=" << config->dbConfig[i].evictInterval << "\n";
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


/*
pending

-1 — wire config into server, get it actually running end to end
0 — Python test client, verify everything works
1 - Move implementations to .cpp files (tcp_server.h and resp.h)
2 - refactor and remove duplicte code
3 - add proper error message while parsing and when parse successful - move implementation to other class
4 - rename all function and helper fucntion
5 - move all error message to namespace
6 - move all constant to namespace

*/