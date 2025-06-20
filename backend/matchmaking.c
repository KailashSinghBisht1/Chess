#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT
#endif

#define MAX_MATCHES 1000
#define HASH_SIZE 1024

typedef enum { RED, BLACK } Color;

// ==========================
// MATCH STRUCT
// ==========================
typedef struct {
    int player1_id;
    int player2_id;
    int match_id;
} Match;

// ==========================
// QUEUE
// ==========================
typedef struct QNode {
    int player_id;
    struct QNode* next;
} QNode;

typedef struct {
    QNode* front;
    QNode* rear;
} Queue;

Queue playerQueue;

void init_queue() {
    playerQueue.front = NULL;
    playerQueue.rear = NULL;
}

void enqueue(int player_id) {
    QNode* newNode = (QNode*)malloc(sizeof(QNode));
    newNode->player_id = player_id;
    newNode->next = NULL;
    if (playerQueue.rear == NULL) {
        playerQueue.front = playerQueue.rear = newNode;
    } else {
        playerQueue.rear->next = newNode;
        playerQueue.rear = newNode;
    }
}

int dequeue() {
    if (playerQueue.front == NULL) return -1;
    QNode* temp = playerQueue.front;
    int id = temp->player_id;
    playerQueue.front = playerQueue.front->next;
    if (playerQueue.front == NULL) playerQueue.rear = NULL;
    free(temp);
    return id;
}

// ==========================
// RED-BLACK TREE
// ==========================
typedef struct RBNode {
    int player_id;
    int rating;
    Color color;
    struct RBNode *left, *right, *parent;
} RBNode;

RBNode* NIL;
RBNode* root;

RBNode* create_node(int id, int rating) {
    RBNode* node = (RBNode*)malloc(sizeof(RBNode));
    node->player_id = id;
    node->rating = rating;
    node->left = node->right = node->parent = NIL;
    node->color = RED;
    return node;
}

void left_rotate(RBNode* x) {
    RBNode* y = x->right;
    x->right = y->left;
    if (y->left != NIL) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == NIL) root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

void right_rotate(RBNode* x) {
    RBNode* y = x->left;
    x->left = y->right;
    if (y->right != NIL) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == NIL) root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

void insert_fixup(RBNode* z) {
    while (z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            RBNode* y = z->parent->parent->right;
            if (y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    left_rotate(z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                right_rotate(z->parent->parent);
            }
        } else {
            RBNode* y = z->parent->parent->left;
            if (y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                left_rotate(z->parent->parent);
            }
        }
    }
    root->color = BLACK;
}

void insert_rb(int id, int rating) {
    RBNode* z = create_node(id, rating);
    RBNode* y = NIL;
    RBNode* x = root;

    while (x != NIL) {
        y = x;
        if (z->rating < x->rating) x = x->left;
        else x = x->right;
    }
    z->parent = y;
    if (y == NIL) root = z;
    else if (z->rating < y->rating) y->left = z;
    else y->right = z;

    insert_fixup(z);
}

RBNode* find_closest_match(int rating, int threshold) {
    RBNode* best = NULL;
    RBNode* current = root;

    while (current != NIL) {
        int diff = abs(current->rating - rating);
        if (diff <= threshold) {
            best = current;
            break;
        }
        if (rating < current->rating)
            current = current->left;
        else
            current = current->right;
    }

    return best;
}

// ==========================
// HASH MAP (for rating lookup)
// ==========================
typedef struct {
    int key;
    int value;
} Entry;

Entry hashmap[HASH_SIZE];

int hash(int key) {
    return key % HASH_SIZE;
}

void put(int key, int value) {
    int idx = hash(key);
    while (hashmap[idx].key != 0 && hashmap[idx].key != key) {
        idx = (idx + 1) % HASH_SIZE;
    }
    hashmap[idx].key = key;
    hashmap[idx].value = value;
}

int get(int key) {
    int idx = hash(key);
    while (hashmap[idx].key != 0) {
        if (hashmap[idx].key == key) return hashmap[idx].value;
        idx = (idx + 1) % HASH_SIZE;
    }
    return -1;
}

// ==========================
// MATCHMAKING
// ==========================
Match match_list[MAX_MATCHES];
int match_counter = 1;
int match_count = 0;

DLL_EXPORT void init_engine() {
    NIL = (RBNode*)malloc(sizeof(RBNode));
    NIL->color = BLACK;
    NIL->left = NIL->right = NIL;
    root = NIL;
    init_queue();
    memset(hashmap, 0, sizeof(hashmap));
}

DLL_EXPORT void add_player(int player_id, int rating) {
    enqueue(player_id);
    insert_rb(player_id, rating);
    put(player_id, rating);
}

DLL_EXPORT int get_match(Match* match_out) {
    if (playerQueue.front == NULL || playerQueue.front->next == NULL)
        return 0;

    int p1 = dequeue();
    int p1_rating = get(p1);

    // Find second player within skill range (±150)
    QNode* prev = NULL;
    QNode* curr = playerQueue.front;
    while (curr != NULL) {
        int p2 = curr->player_id;
        int p2_rating = get(p2);
        if (abs(p1_rating - p2_rating) <= 150) {
            // Remove curr from queue
            if (prev == NULL) {
                playerQueue.front = curr->next;
                if (playerQueue.front == NULL)
                    playerQueue.rear = NULL;
            } else {
                prev->next = curr->next;
                if (curr == playerQueue.rear)
                    playerQueue.rear = prev;
            }

            int p2_id = curr->player_id;
            free(curr);

            match_out->player1_id = p1;
            match_out->player2_id = p2_id;
            match_out->match_id = match_counter++;

            match_list[match_count++] = *match_out;
            return 1;
        }
        prev = curr;
        curr = curr->next;
    }

    // No suitable match found, requeue player
    enqueue(p1);
    return 0;
}
