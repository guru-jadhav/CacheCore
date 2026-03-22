// #include <string>        // angle brackets — for system/standard library headers
// #include "../../include/lru_store.h"
#include "lru_store.h"      // quotes — for your own project headers
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

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

    while(!tllHeap.empty()){
        tllHeap.pop();
    }
};

bool LRUStore::isExpired(Node* curr){
    if(curr->expTime == std::nullopt){
        return false;
    }
    return curr->expTime <= std::chrono::steady_clock::now();
    
};

void LRUStore::addToTTLHeap(const std::string& _key, std::chrono::steady_clock::time_point _expTime){
    TTLNode newNode = TTLNode(
        _key, 
        _expTime
    );
    tllHeap.push(newNode);
};

void LRUStore::addExpTime(Node* curr, const bool isExpires, const size_t duration){
    if(isExpires){
        auto computedExpTime = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(ttl, duration));
        curr->expTime = computedExpTime;
        addToTTLHeap(curr->key, computedExpTime);
    }
};


void LRUStore::removeExpKeys(){
    while(!tllHeap.empty() && tllHeap.top().expTime <= std::chrono::steady_clock::now()){
        TTLNode curr = tllHeap.top();
        const auto& isPresent  = store.find(curr.key);
        if(isPresent == store.end() || (isPresent->second->expTime != std::nullopt && isPresent->second->expTime <= std::chrono::steady_clock::now())){
            DEL(curr.key);
        }
        tllHeap.pop();
    }
};

std::optional<std::string> LRUStore::GET(const std::string& _key){
    
    const auto& isPresent = store.find(_key);
    if(isPresent == store.end()){
        return std::nullopt;
    }

    if(isPresent->second->expTime && isExpired(isPresent->second)){
        DEL(isPresent->second->key);
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

    const auto& isPresent = store.find(_key);
    if(isPresent != store.end()){
        Node* curr = isPresent->second;
        curr->value = _value;
        moveToHead(isPresent->second);
        addExpTime(curr, isExpires);
        // if(isExpires){
        //     addToTTLHeap(_key);
        // }
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
    // if(isExpires){
    //     addToTTLHeap(_key);
    // }
    return true;
};

void LRUStore::DEL(const std::string& _key){
    const auto& isPresent = store.find(_key);
    if(isPresent == store.end()){
        return;
    }

    Node* toDelete = isPresent->second;
    deleteNode(toDelete);
};

bool LRUStore::EXISTS(const std::string& _key){
    return store.find(_key) != store.end();
};

void LRUStore::CLEAR(){
    // delete all the nodes and then clear the map
    clearStore();
    store.clear();
};

void LRUStore::EXPIRE(const std::string& _key, const size_t duration){
    const auto& isPresent  = store.find(_key);
    if(isPresent == store.end()){
        return;
    }
    Node* curr = isPresent->second;
    addExpTime(curr, true, duration);
    // auto computedExpTime = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(ttl, duration));
    // curr->expTime = computedExpTime;
    // addToTTLHeap(_key, duration);
};