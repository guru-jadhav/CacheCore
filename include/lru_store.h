#pragma once
// #pragma once tells the compiler — "no matter how many times you encounter this file, only process it once."
// #pragma once must be the absolute first line of the file. If another file includes this header before the guard 
// is registered, double inclusion can still happen. Move it to line 1.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <string>
#include <optional>
#include <vector>
#include <mutex>


struct TTLNode {
    std::string key;
    std::chrono::steady_clock::time_point expTime;

    bool operator>(const TTLNode& other) const {
        return expTime > other.expTime;
    }

    TTLNode(const std::string& _key, const std::chrono::steady_clock::time_point& _expTime) : key(_key), expTime(_expTime) {}
};


struct Node {
    std::string key;
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expTime = std::nullopt;
    Node* prev;
    Node* next;

    Node(const std::string& _key, const std::string& _value) : key(_key), value(_value), prev(nullptr), next(nullptr) {}
};

/*
    using initializer struct as params can grow more and more and maintaing them 
    can become a chaos
*/ 
struct LRUStoreConfig {
    size_t maxCapacity   = 100;
    size_t ttl           = 60;
    size_t evictInterval = 60;

    LRUStoreConfig(){};

    LRUStoreConfig(size_t _maxCapacity, size_t _ttl, size_t _evictInterval)
        : maxCapacity(_maxCapacity)
        , ttl(_ttl)
        , evictInterval(_evictInterval)
    {}
};

class LRUStore {

    void removeNode(Node* curr);
    void moveToHead(Node* curr);
    void linkToHead(Node* curr);
    void deleteNode(Node* toDelete);
    void clearStore();
    bool isExpired(const Node* curr);
    void addExpTime(Node* curr, const bool isExpires, const size_t duration = 0);
    void addToTTLHeap(const std::string& key, std::chrono::steady_clock::time_point _expTime);
    void removeExpKeys();
    bool isAllDigits(const std::string& value);
    void addNewNodeToStore(const std::string& _key, const std::string& _val, const bool isExpire);

    std::unordered_map<std::string, Node*> store;
    Node* dummyHead;
    Node* dummyTail;
    // store.size() returns size_t which is unsigned. maxCapacity is int which is signed. 
    // This will trigger a compiler warning and can behave unexpectedly if maxCapacity is somehow negative.
    const size_t maxCapacity;
    const size_t ttl;
    const size_t evictInterval;
    std::priority_queue<TTLNode, std::vector<TTLNode>, std::greater<TTLNode>> ttlHeap;
    std::mutex mapMtx;
    std::mutex heapMtx;
    std::mutex evictionMtx;
    std::condition_variable cv;
    std::atomic<bool> stopEviction{false};
    std::thread evictionThread;

    void evictionLoop(){
        while(true){
            std::unique_lock<std::mutex> lock(evictionMtx);
            std::unique_lock<std::mutex> heapLock(heapMtx);
            
            /*
                Smart wakeup: instead of sleeping a fixed evictInterval, sleep until the
                nearest key's expiry time. If heap is empty, fall back to evictInterval.
                When a new key with earlier expiry is added (via addToTTLHeap),
                cv.notify_one() wakes this thread early to recalculate wakeup time.
            */
            std::chrono::time_point<std::chrono::steady_clock> nextEvictionTime = 
                !ttlHeap.empty() 
                ? ttlHeap.top().expTime 
                : std::chrono::steady_clock::now() + std::chrono::seconds(evictInterval);
            
            // release heapMtx before sleeping — cv needs only evictionMtx
            heapLock.unlock(); 

            cv.wait_until(lock, nextEvictionTime, [this]{
                return stopEviction.load();
            });
            if (stopEviction) {
                break;
            }
            removeExpKeys();
        }
    };

    public:

        //  ttl(std::max(ttl, _ttl) this is wrong we are reading ttl even before it's initialized it
        //  ttl(std::max((size_t)60, _ttl))
        LRUStore(const LRUStoreConfig& config) 
            : maxCapacity(config.maxCapacity)
            , ttl(std::max((size_t)60, config.ttl))
            , evictInterval(std::max((size_t)10, config.evictInterval))
        {
            dummyHead = new Node("-1", "-1");
            dummyTail = new Node("-1", "-1");

            dummyHead->next = dummyTail;
            dummyTail->prev = dummyHead;

            evictionThread = std::thread(&LRUStore::evictionLoop, this);
        }

        ~LRUStore(){

            /*
                Shutdown sequence — order matters:
                1. Set flag first so thread sees it when woken
                2. notify_one() wakes thread immediately instead of waiting up to evictInterval
                3. join() blocks until thread fully exits — prevents crash on scope exit
                   (if std::thread is destroyed while still joinable -> std::terminate() -> crash)
                   Memory cleanup happens AFTER join() — thread must stop before we free anything
            */
            
            stopEviction = true;
            cv.notify_one();
            if(evictionThread.joinable()){
                evictionThread.join();
            }

            std::lock_guard<std::mutex> lock(mapMtx);
            std::lock_guard<std::mutex> heapLock(heapMtx);
            Node* curr = dummyHead;
            while(curr){
                Node* nextNode = curr->next;
                delete curr;
                curr = nextNode;
            }

            while(!ttlHeap.empty()){
                ttlHeap.pop();
            }
        }
        
        std::optional<std::string> GET(const std::string& _key);

        // will send this default flag from cliet
        bool SET(const std::string& _key, const std::string& _value, const bool isExpires);

        // we can make the DEL as void -> if exists we delete it we just return
        void DEL(const std::string& _key);

        bool EXISTS(const std::string& _key);

        void CLEAR();

        void EXPIRE(const std::string& _key, const size_t duration);

        std::optional<std::string> INCR(const std::string& _key);
};