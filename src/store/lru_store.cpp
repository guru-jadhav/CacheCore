// #include <string>        // angle brackets — for system/standard library headers
// #include "../../include/lru_store.h"
#include "lru_store.h"      // quotes — for your own project headers
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

void LRUStore::removeNode(Node* curr){
    Node* prevNode = curr->prev;
    Node* nextNode = curr->next;

    prevNode->next = nextNode;
    nextNode->prev = prevNode;

    curr->prev = nullptr;
    curr->next = nullptr;
};

void LRUStore::moveToHead(Node* curr){
    if(dummyHead->next == curr){
        return;
    }
    
    removeNode(curr);
    linkToHead(curr);
};

void LRUStore::linkToHead(Node* curr){
    Node* headNext = dummyHead->next;

    dummyHead->next = curr;
    curr->prev = dummyHead;
    curr->next = headNext;
    headNext->prev = curr;
};

void LRUStore::deleteNode(Node* toDelete){
    removeNode(toDelete);
    store.erase(toDelete->key);
    delete toDelete;
};

void LRUStore::clearStore(){
    Node* curr = dummyHead->next;
    while(curr != dummyTail){
        Node* nextNode = curr->next;
        delete curr;
        curr = nextNode;
    }
    dummyHead->next = dummyTail;
    dummyTail->prev = dummyHead;

    while(!ttlHeap.empty()){
        ttlHeap.pop();
    }
};

bool LRUStore::isExpired(Node* curr){
    if(curr->expTime == std::nullopt){
        return false;
    }
    return curr->expTime <= std::chrono::steady_clock::now();
    
};

void LRUStore::addToTTLHeap(const std::string& _key, std::chrono::steady_clock::time_point _expTime){
    std::unique_lock<std::mutex> lock(heapMtx);

    TTLNode newNode = TTLNode(
        _key, 
        _expTime
    );
    bool needtoNotify = ttlHeap.empty() || _expTime < ttlHeap.top().expTime;

    ttlHeap.push(newNode);
    lock.unlock();

    /*
        1 - we don't want the background bgThread to sleep waiting for 60s to complete
        we will trigger the wake up of the bgThread when we have nearest possible 
        expTime

        2 - whenever a new expTime comes in which is < our nearest expTime -> we want our
        thread to wake up at that exact time to clean up -> so we notify the thread to wakeup
        at that exact time using the below condition

    */
    if(needtoNotify){
        cv.notify_one();
    }
};

void LRUStore::addExpTime(Node* curr, const bool isExpires, const size_t duration){
    if(isExpires){
        auto computedExpTime = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(ttl, duration));
        curr->expTime = computedExpTime;
        addToTTLHeap(curr->key, computedExpTime);
    }else{
        /*
            if we had an existing key and with an exp time and now the
            user updates the key -> calls SET with isExpires = false then
            with the expTime of that node should be = nullopt, meaning it should never expire
            but if we don't put the else condition then we will never make an already existing
            key->expTime = nullopt and at somepoint the bgThread will clean it up -> wrong behaviour
        */
        curr->expTime = std::nullopt;
    }
};


void LRUStore::removeExpKeys(){
    std::unique_lock<std::mutex> lock(heapMtx);
    std::vector<std::string> toDeleteKeys;

    while(!ttlHeap.empty() && ttlHeap.top().expTime <= std::chrono::steady_clock::now()){
        TTLNode curr = ttlHeap.top();
        /*
            collect all the expired keys this way we keep our store mutex free 
            and GET|SET can use that mutex in the main thread
        */
        toDeleteKeys.push_back(curr.key);
        ttlHeap.pop();
    }
    lock.unlock();

    for(std::string key : toDeleteKeys){
        /*
            Not using DEL() here because we need to verify the key's expTime
            hasn't been updated (via EXPIRE/SET) since this heap entry was created.
            DEL() deletes unconditionally — we only want to delete if still expired.
        */

        // DEL(key);
        std::lock_guard<std::mutex> storeLock(mapMtx);
        const auto& isPresent  = store.find(key);
        if(isPresent != store.end() && isPresent->second->expTime != std::nullopt && isPresent->second->expTime <= std::chrono::steady_clock::now()){
            deleteNode(isPresent->second);
        }
    }
};

std::optional<std::string> LRUStore::GET(const std::string& _key){
    std::lock_guard<std::mutex> lock(mapMtx);
    const auto& isPresent = store.find(_key);
    if(isPresent == store.end()){
        return std::nullopt;
    }

    if(isPresent->second->expTime && isExpired(isPresent->second)){
        /*
            DEL(isPresent->second->key);
            dead-lock because we have already acquired lock and DEL() will again try
            to acquire the lock again which is already acquired by the caller
        */
        deleteNode(isPresent->second);
        return std::nullopt;
    }
    
    // move the queried key to the head
    moveToHead(isPresent->second);

    return isPresent->second->value;
};

bool LRUStore::SET(const std::string& _key, const std::string& _value, const bool isExpires){
    if(maxCapacity == 0){
        return false;
    }

    std::lock_guard<std::mutex> lock(mapMtx);
    const auto& isPresent = store.find(_key);
    if(isPresent != store.end()){
        Node* curr = isPresent->second;
        curr->value = _value;
        moveToHead(isPresent->second);
        addExpTime(curr, isExpires);
        return true;
    }

    if(store.size() >= maxCapacity){
        Node* toDelete = dummyTail->prev;
        deleteNode(toDelete);
    }
    
    Node* newNode = new Node(_key, _value);
    store[_key] = newNode;
    linkToHead(newNode);
    addExpTime(newNode, isExpires);
    return true;
};

void LRUStore::DEL(const std::string& _key){
    std::lock_guard<std::mutex> lock(mapMtx);
    const auto& isPresent = store.find(_key);
    if(isPresent == store.end()){
        return;
    }

    Node* toDelete = isPresent->second;
    deleteNode(toDelete);
};

bool LRUStore::EXISTS(const std::string& _key){
    std::lock_guard<std::mutex>lock(mapMtx);
    const auto& isPresent = store.find(_key);
    if(isPresent == store.end()){
        return false;
    }
    if(isExpired(isPresent->second)){
        deleteNode(isPresent->second);
        return false;
    }
    return true;
};

void LRUStore::CLEAR(){
    // delete all the nodes and then clear the map
    std::lock_guard<std::mutex> strLock(mapMtx);
    std::lock_guard<std::mutex> heapLock(heapMtx);
    clearStore();
    store.clear();
};

void LRUStore::EXPIRE(const std::string& _key, const size_t duration){
    std::lock_guard<std::mutex> lock(mapMtx);
    const auto& isPresent  = store.find(_key);
    if(isPresent == store.end()){
        return;
    }
    Node* curr = isPresent->second;
    addExpTime(curr, true, duration);
};