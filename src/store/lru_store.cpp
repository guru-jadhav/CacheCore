// #include <string>        // angle brackets — for system/standard library headers
// #include "../../include/lru_store.h"
#include "lru_store.h"      // quotes — for your own project headers
#include <chrono>
#include <climits>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

void LRUStore::evictionLoop(){
    while(true){
        std::unique_lock<std::mutex> lock(evictionMtx);
        std::unique_lock<std::mutex> heapLock(heapMtx);
        
        /*
            Smart wakeup: instead of sleeping a fixed evictInterval, sleep until the
            nearest key's expiry time. If heap is empty, fall back to evictInterval.
            When a new key with earlier expiry is added (via scheduleTTL),
            evictionCv.notify_one() wakes this thread early to recalculate wakeup time.
        */
        std::chrono::time_point<std::chrono::steady_clock> nextEvictionTime = 
            !ttlHeap.empty() 
            ? ttlHeap.top().expTime 
            : std::chrono::steady_clock::now() + std::chrono::seconds(evictInterval);
        
        // release heapMtx before sleeping — evictionCv needs only evictionMtx
        heapLock.unlock(); 

        evictionCv.wait_until(lock, nextEvictionTime, [this]{
            return stopEviction.load();
        });
        if (stopEviction) {
            break;
        }
        purgeExpiredKeys();
    }
};

bool LRUStore::isAllDigits(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    int startIndex = 0;
    
    if (value[0] == '-') {
        if (value.size() == 1) {
            return false;
        }
        startIndex = 1;
    }

    for (int i = startIndex; i < value.size(); i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    
    return true;
}

void LRUStore::insertNode(const std::string& _key, const std::string& _value, const bool isExpires){
    if(store.size() >= maxCapacity){
        Node* toDelete = dummyTail->prev;
        deleteNode(toDelete);
    }
    
    Node* newNode = new Node(_key, _value);
    store[_key] = newNode;
    linkToHead(newNode);
    setExpiry(newNode, isExpires);
}

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

bool LRUStore::isExpired(const Node* curr){
    if(curr->expTime == std::nullopt){
        return false;
    }
    return curr->expTime <= std::chrono::steady_clock::now();
    
};

void LRUStore::scheduleTTL(const std::string& _key, std::chrono::steady_clock::time_point _expTime){
    std::unique_lock<std::mutex> lock(heapMtx);

    TTLEntry newNode = TTLEntry(
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
        evictionCv.notify_one();
    }
};

void LRUStore::setExpiry(Node* curr, const bool isExpires, const size_t duration){
    if(isExpires){
        auto computedExpTime = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(ttl, duration));
        curr->expTime = computedExpTime;
        scheduleTTL(curr->key, computedExpTime);
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


void LRUStore::purgeExpiredKeys(){
    std::unique_lock<std::mutex> lock(heapMtx);
    std::vector<std::string> toDeleteKeys;

    while(!ttlHeap.empty() && ttlHeap.top().expTime <= std::chrono::steady_clock::now()){
        TTLEntry curr = ttlHeap.top();
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
        std::lock_guard<std::mutex> storeLock(storeMtx);
        const auto& isPresent  = store.find(key);
        if(isPresent != store.end() && isPresent->second->expTime != std::nullopt && isPresent->second->expTime <= std::chrono::steady_clock::now()){
            deleteNode(isPresent->second);
        }
    }
};

std::optional<std::string> LRUStore::GET(const std::string& _key){
    std::lock_guard<std::mutex> lock(storeMtx);
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

    std::lock_guard<std::mutex> lock(storeMtx);
    const auto& isPresent = store.find(_key);
    if(isPresent != store.end()){
        Node* curr = isPresent->second;
        curr->value = _value;
        moveToHead(isPresent->second);
        setExpiry(curr, isExpires);
        return true;
    }
    insertNode(_key, _value, isExpires);
    return true;
};

void LRUStore::DEL(const std::string& _key){
    std::lock_guard<std::mutex> lock(storeMtx);
    const auto& isPresent = store.find(_key);
    if(isPresent == store.end()){
        return;
    }

    Node* toDelete = isPresent->second;
    deleteNode(toDelete);
};

bool LRUStore::EXISTS(const std::string& _key){
    std::lock_guard<std::mutex>lock(storeMtx);
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
    std::lock_guard<std::mutex> strLock(storeMtx);
    std::lock_guard<std::mutex> heapLock(heapMtx);
    clearStore();
    store.clear();
};

void LRUStore::EXPIRE(const std::string& _key, const size_t duration){
    std::lock_guard<std::mutex> lock(storeMtx);
    const auto& isPresent  = store.find(_key);
    if(isPresent == store.end()){
        return;
    }
    Node* curr = isPresent->second;
    setExpiry(curr, true, duration);
};


std::optional<std::string> LRUStore::INCR(const std::string& _key){
    std::lock_guard<std::mutex> strLock(storeMtx);
    const auto& isPresent = store.find(_key);

    if(isPresent == store.end() || isExpired(isPresent->second)){

        // this means we have the key but it was expired
        if(isPresent != store.end()){
            deleteNode(isPresent->second);
        }
        insertNode(_key, "1", false);
        return "1";
    }

    std::string& oldValueStr = isPresent->second->value;

    if(!isAllDigits(oldValueStr)){
        return std::nullopt;
    }

    long long newValue = 0;

    try {
        newValue = std::stoll(oldValueStr);
    } catch (...) {
        // if we have LLONG_MAX + 1 value then it throws std::out_of_range
        // even though we never increment beyound LLONG_MAX, but what if user
        // sets very long out-of-bound value and tried to incremt it, then boom
        return std::nullopt;
    }
    
    // but what if we do ++ and this moves out newValue out-of-bond ?
    // we need a check here -> overflow check before increment
    if(newValue == LLONG_MAX){
        return std::nullopt;
    }

    newValue++;
    std::string strCounter = std::to_string(newValue);
    oldValueStr = strCounter;
    moveToHead(isPresent->second);

    return strCounter;
};