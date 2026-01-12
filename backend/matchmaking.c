/*
matchmaking.c

Scalable, high-performance matchmaking engine.
Design:
 - Rating buckets (configurable BUCKET_SIZE, MAX_RATING)
 - Chained dynamic hashmap for player -> bucket node (resizes)
 - Memory pool for player nodes (fast allocations)
 - Internal thread-safety with CRITICAL_SECTION (Windows)
 - Exported C API for DLL usage.

Exports:
 - void init_engine();
 - void add_player(const char* player_uuid, int rating);
 - void add_bot(const char* bot_uuid, int rating);
 - void remove_player(const char* player_uuid);
 - int get_match(Match* out);           // returns 1 if match found, 0 otherwise
 - int get_all_matches(Match* buf, int max_buf); // returns number of entries filled
 - void free_engine();

Notes:
 - UUID expected to be null-terminated string length <= UUID_LEN-1.
 - Rating values outside range are clamped.
 - This file targets Windows (CRITICAL_SECTION). For pthreads, replace locking code.
*/
/*
matchmaking.c  (updated)

Features:
 - Per-bucket SRWLOCK (fine-grained concurrency)
 - Rating-bucketed matching with closest-rating selection within max_rating_diff
 - Dynamic chained hashmap (resize)
 - Memory pool (free-list)
 - Async logger thread that appends to matches.log (non-blocking)
 - Python callback registration for match events
 - Shard config API (set_shard)
 - Exports: init_engine, add_player, add_bot, remove_player, get_match,
            get_all_matches, set_max_rating_diff, set_shard, register_match_callback, free_engine

Notes:
 - Windows-specific (SRWLOCK, CreateThread, CRITICAL_SECTION).
 - UUID length <= UUID_LEN-1.
 - Python callback signature (Cdecl): void callback(const char* p1, const char* p2, int match_id, int rating1, int rating2)
 - Use ctypes with restype/argtypes in Python to interact.

Compile:
 - MinGW:
     gcc -O2 -shared -o matchmaking.dll matchmaking.c -Wl,--out-implib,libmatchmaking.a
 - MSVC:
     cl /O2 /LD matchmaking.c /Fe:matchmaking.dll
*/

#define SCAN_NODES_PER_BUCKET 16
#define SCAN_CAND_NODES_PER_BUCKET 32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>


#ifndef EXPORT
#define EXPORT __declspec(dllexport)
#endif

/* -------------------- Config -------------------- */
#define UUID_LEN 64
#define DEFAULT_HASH_CAP 32768
#define LOAD_FACTOR_THRESHOLD 0.7
#define POOL_CHUNK 65536
#define BUCKET_SIZE 20
#define MAX_RATING 4000
#define BUCKET_COUNT ((MAX_RATING/BUCKET_SIZE) + 5)
#define MATCH_HISTORY_DEFAULT 262144
#define LOG_PATH "matches.log"

/* -------------------- Types -------------------- */
typedef struct PlayerNode {
    char uuid[UUID_LEN];
    int rating;
    struct PlayerNode* prev;
    struct PlayerNode* next;
    struct PlayerNode* hashmap_next;
    uint64_t arrival_seq;
} PlayerNode;

typedef struct Bucket {
    PlayerNode* head;
    PlayerNode* tail;
    int count;
    SRWLOCK lock;
} Bucket;

typedef struct {
    char player1[UUID_LEN];
    char player2[UUID_LEN];
    int match_id;
} Match;

typedef struct {
    char p1[UUID_LEN];
    char p2[UUID_LEN];
    int match_id;
    int r1;
    int r2;
} MatchLog;

typedef struct LogNode {
    MatchLog log;
    struct LogNode* next;
} LogNode;

/* -------------------- Globals -------------------- */
static Bucket* buckets = NULL;
static int bucket_count = BUCKET_COUNT;

static PlayerNode** hashmap = NULL;
static size_t hashmap_capacity = 0;
static size_t hashmap_count = 0;
static CRITICAL_SECTION hashmap_lock;

typedef struct PoolChunk {
    PlayerNode* nodes;
    struct PoolChunk* next;
} PoolChunk;
static PoolChunk* pool_chunks = NULL;
static PlayerNode* free_list = NULL;

static uint64_t global_arrival_seq = 1;

static Match* match_history = NULL;
static size_t match_history_cap = MATCH_HISTORY_DEFAULT;
static size_t match_history_head = 0;
static uint64_t match_counter = 0;
static CRITICAL_SECTION history_lock;

/* async logger */
static CRITICAL_SECTION logger_lock;
static CONDITION_VARIABLE logger_cv;
static LogNode* logger_queue_head = NULL;
static LogNode* logger_queue_tail = NULL;
static HANDLE logger_thread = NULL;
static volatile int logger_thread_running = 0;

/* shard config */
static int shard_id = 0;
static int shard_count = 1;

static volatile int engine_inited = 0;
static int max_rating_diff = 200;

/* Python callback */
typedef void (__cdecl *match_callback_t)(const char*, const char*, int, int, int);
static match_callback_t py_match_callback = NULL;

/* -------------------- Utils -------------------- */
static unsigned long fnv1a_hash(const char* s) {
    unsigned long h = 2166136261u;
    while (*s) { h ^= (unsigned char)(*s++); h *= 16777619u; }
    return h;
}
static int clamp_rating(int r) {
    if (r < 0) return 0;
    if (r > MAX_RATING*2) return MAX_RATING*2;
    return r;
}
static int rating_to_bucket_index(int rating) {
    int idx = rating / BUCKET_SIZE;
    if (idx < 0) idx = 0;
    if (idx >= bucket_count) idx = bucket_count - 1;
    return idx;
}

/* -------------------- Memory pool -------------------- */
static PlayerNode* pool_alloc_nodes(size_t count) {
    PlayerNode* nodes = (PlayerNode*)malloc(sizeof(PlayerNode)*count);
    if (!nodes) return NULL;
    PoolChunk* pc = (PoolChunk*)malloc(sizeof(PoolChunk));
    if (!pc) { free(nodes); return NULL; }
    pc->nodes = nodes;
    pc->next = pool_chunks;
    pool_chunks = pc;
    for (size_t i=0;i<count;++i) {
        nodes[i].next = free_list;
        free_list = &nodes[i];
    }
    return nodes;
}
static PlayerNode* node_alloc() {
    if (!free_list) {
        if (!pool_alloc_nodes(POOL_CHUNK)) return NULL;
    }
    PlayerNode* n = free_list;
    free_list = free_list->next;
    memset(n,0,sizeof(PlayerNode));
    return n;
}
static void node_free(PlayerNode* n) {
    if (!n) return;
    n->next = free_list;
    free_list = n;
}

/* -------------------- Hashmap -------------------- */
static int hashmap_init(size_t capacity) {
    hashmap = (PlayerNode**)calloc(capacity, sizeof(PlayerNode*));
    if (!hashmap) return 0;
    hashmap_capacity = capacity;
    hashmap_count = 0;
    return 1;
}
static void hashmap_free_all() {
    if (hashmap) free(hashmap);
    hashmap = NULL;
    hashmap_capacity = 0;
    hashmap_count = 0;
}
static void hashmap_insert(PlayerNode* node) {
    if (!node) return;
    EnterCriticalSection(&hashmap_lock);
    if (hashmap_capacity == 0) hashmap_init(DEFAULT_HASH_CAP);
    unsigned long h = fnv1a_hash(node->uuid);
    size_t idx = h % hashmap_capacity;
    node->hashmap_next = hashmap[idx];
    hashmap[idx] = node;
    hashmap_count++;
    LeaveCriticalSection(&hashmap_lock);
}
static PlayerNode* hashmap_find(const char* uuid) {
    if (!hashmap) return NULL;
    unsigned long h = fnv1a_hash(uuid);
    size_t idx = h % hashmap_capacity;
    EnterCriticalSection(&hashmap_lock);
    PlayerNode* p = hashmap[idx];
    while (p) {
        if (strcmp(p->uuid, uuid) == 0) { LeaveCriticalSection(&hashmap_lock); return p; }
        p = p->hashmap_next;
    }
    LeaveCriticalSection(&hashmap_lock);
    return NULL;
}
static void hashmap_remove(const char* uuid) {
    if (!hashmap) return;
    unsigned long h = fnv1a_hash(uuid);
    size_t idx = h % hashmap_capacity;
    EnterCriticalSection(&hashmap_lock);
    PlayerNode* p = hashmap[idx];
    PlayerNode* prev = NULL;
    while (p) {
        if (strcmp(p->uuid, uuid) == 0) {
            if (prev) prev->hashmap_next = p->hashmap_next;
            else hashmap[idx] = p->hashmap_next;
            hashmap_count--;
            LeaveCriticalSection(&hashmap_lock);
            return;
        }
        prev = p;
        p = p->hashmap_next;
    }
    LeaveCriticalSection(&hashmap_lock);
}

/* -------------------- Buckets -------------------- */
static void buckets_init() {
    buckets = (Bucket*)malloc(sizeof(Bucket)*bucket_count);
    for (int i=0;i<bucket_count;++i) {
        buckets[i].head = buckets[i].tail = NULL;
        buckets[i].count = 0;
        InitializeSRWLock(&buckets[i].lock);
    }
}
static void bucket_push_tail_unlocked(int idx, PlayerNode* node) {
    node->prev = node->next = NULL;
    if (!buckets[idx].tail) buckets[idx].head = buckets[idx].tail = node;
    else {
        node->prev = buckets[idx].tail;
        buckets[idx].tail->next = node;
        buckets[idx].tail = node;
    }
    buckets[idx].count++;
}
static void bucket_remove_node_unlocked(int idx, PlayerNode* node) {
    if (!node) return;
    if (node->prev) node->prev->next = node->next;
    else buckets[idx].head = node->next;
    if (node->next) node->next->prev = node->prev;
    else buckets[idx].tail = node->prev;
    node->prev = node->next = NULL;
    buckets[idx].count--;
}

/* -------------------- Match history -------------------- */
static void history_init(size_t cap) {
    match_history_cap = cap;
    match_history = (Match*)malloc(sizeof(Match)*cap);
    memset(match_history,0,sizeof(Match)*cap);
    match_history_head = 0;
    match_counter = 0;
    InitializeCriticalSection(&history_lock);
}
static void history_push(const char* p1, const char* p2) {
    EnterCriticalSection(&history_lock);
    Match m;
    strncpy(m.player1,p1,UUID_LEN-1); m.player1[UUID_LEN-1]=0;
    strncpy(m.player2,p2,UUID_LEN-1); m.player2[UUID_LEN-1]=0;
    m.match_id = (int)(++match_counter);
    match_history[match_history_head] = m;
    match_history_head = (match_history_head + 1) % match_history_cap;
    LeaveCriticalSection(&history_lock);
}

/* -------------------- Async logger -------------------- */
static DWORD WINAPI logger_thread_func(LPVOID arg) {
    while (logger_thread_running) {
        EnterCriticalSection(&logger_lock);
        while (!logger_queue_head && logger_thread_running) {
            SleepConditionVariableCS(&logger_cv, &logger_lock, INFINITE);
        }
        LogNode* head = logger_queue_head;
        logger_queue_head = NULL;
        logger_queue_tail = NULL;
        LeaveCriticalSection(&logger_lock);

        if (!head) continue;
        FILE* f = fopen(LOG_PATH,"a");
        if (!f) {
            LogNode* n = head;
            while (n) { LogNode* nx = n->next; free(n); n = nx; }
            continue;
        }
        LogNode* cur = head;
        while (cur) {
            MatchLog* L = &cur->log;
            fprintf(f, "%d,%s,%d,%s,%d\n", L->match_id, L->p1, L->r1, L->p2, L->r2);
            if (py_match_callback) {
                py_match_callback(L->p1, L->p2, L->match_id, L->r1, L->r2);
            }
            LogNode* nx = cur->next;
            free(cur);
            cur = nx;
        }
        fflush(f);
        fclose(f);
    }
    return 0;
}
static void logger_enqueue(const char* p1, const char* p2, int match_id, int r1, int r2) {
    LogNode* n = (LogNode*)malloc(sizeof(LogNode));
    if (!n) return;
    memset(n,0,sizeof(LogNode));
    n->next = NULL;
    n->log.match_id = match_id;
    n->log.r1 = r1; n->log.r2 = r2;
    strncpy(n->log.p1, p1, UUID_LEN-1); n->log.p1[UUID_LEN-1]=0;
    strncpy(n->log.p2, p2, UUID_LEN-1); n->log.p2[UUID_LEN-1]=0;

    EnterCriticalSection(&logger_lock);
    if (!logger_queue_head) { logger_queue_head = n; logger_queue_tail = n; }
    else { logger_queue_tail->next = n; logger_queue_tail = n; }
    LeaveCriticalSection(&logger_lock);
    WakeConditionVariable(&logger_cv);
}
/* -------------------- Engine core ops -------------------- */

/* internal add */
static void engine_add_player_internal(const char* uuid, int rating) {
    if (!uuid || !uuid[0]) return;
    int r = clamp_rating(rating);
    int bidx = rating_to_bucket_index(r);

    /* quick check if already exists */
    PlayerNode* existing = hashmap_find(uuid);
    if (existing) {
        /* update if rating changed */
        if (existing->rating != r) {
            int oldidx = rating_to_bucket_index(existing->rating);
            /* lock both old and new buckets (order: smaller index first) */
            int a = oldidx, b = bidx;
            if (a > b) { int t=a; a=b; b=t; }
            AcquireSRWLockExclusive(&buckets[a].lock);
            if (b != a) AcquireSRWLockExclusive(&buckets[b].lock);
            bucket_remove_node_unlocked(oldidx, existing);
            existing->rating = r;
            bucket_push_tail_unlocked(bidx, existing);
            if (b != a) ReleaseSRWLockExclusive(&buckets[b].lock);
            ReleaseSRWLockExclusive(&buckets[a].lock);
        }
        return;
    }

    /* allocate node */
    PlayerNode* n = node_alloc();
    if (!n) return;
    strncpy(n->uuid, uuid, UUID_LEN-1); n->uuid[UUID_LEN-1]=0;
    n->rating = r;
    n->arrival_seq = InterlockedIncrement64((volatile LONG64*)&global_arrival_seq);
    n->prev = n->next = n->hashmap_next = NULL;

    /* insert into bucket */
    AcquireSRWLockExclusive(&buckets[bidx].lock);
    bucket_push_tail_unlocked(bidx, n);
    ReleaseSRWLockExclusive(&buckets[bidx].lock);

    /* insert into hashmap */
    hashmap_insert(n);
}

/* exported wrappers */
EXPORT void init_engine() {
    if (engine_inited) return;
    /* init basics */
    InitializeCriticalSection(&hashmap_lock);
    InitializeCriticalSection(&logger_lock);
    InitializeConditionVariable(&logger_cv);
    buckets_init();
    hashmap_init(DEFAULT_HASH_CAP);
    pool_alloc_nodes(POOL_CHUNK);
    history_init(MATCH_HISTORY_DEFAULT);

    /* start logger thread */
    logger_thread_running = 1;
    logger_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)logger_thread_func, NULL, 0, NULL);

    engine_inited = 1;
}

EXPORT void set_max_rating_diff(int diff) {
    if (diff < 0) diff = 0;
    max_rating_diff = diff;
}

EXPORT void set_shard(int s_id, int s_count) {
    if (s_count <= 0) return;
    shard_id = s_id >= 0 ? s_id : 0;
    shard_count = s_count;
}

/* register python callback */
EXPORT void register_match_callback(match_callback_t cb) {
    py_match_callback = cb;
}

/* add player / bot */
EXPORT void add_player(const char* uuid, int rating) {
    if (!engine_inited) init_engine();
    /* optional: shard check here; if this process not responsible, ignore
       (we assume Python shard routing will call correct DLL instance) */
    engine_add_player_internal(uuid, rating);
}

EXPORT void add_bot(const char* uuid, int rating) {
    add_player(uuid, rating);
}

/* remove */
EXPORT void remove_player(const char* uuid) {
    if (!engine_inited || !uuid) return;
    PlayerNode* p = hashmap_find(uuid);
    if (!p) return;
    int bidx = rating_to_bucket_index(p->rating);
    /* lock bucket */
    AcquireSRWLockExclusive(&buckets[bidx].lock);
    bucket_remove_node_unlocked(bidx, p);
    ReleaseSRWLockExclusive(&buckets[bidx].lock);
    hashmap_remove(uuid);
    node_free(p);
}

/* get_match: returns 1 if match found (fills out), 0 otherwise
   Selection policy:
     - pick earliest-arrived head among all buckets as candidate p1
     - search for best p2 within max_rating_diff (closest rating). Tie -> FIFO by arrival_seq
*/
/* Two-pass get_match:
   Pass 1: find any exact-rating pair (diff == 0) with earliest arrival tie-break.
   Pass 2: if none, find global minimum non-zero diff <= max_rating_diff (tie -> earliest).
*/
EXPORT int get_match(Match* out) {
    if (!engine_inited || !out) return 0;

    PlayerNode* found_a = NULL;
    PlayerNode* found_b = NULL;
    int found_a_idx = -1;
    int found_b_idx = -1;
    int found_diff = INT_MAX;
    uint64_t found_seq = (uint64_t)(-1);

    /* ---------- PASS 1: exact matches (diff == 0) ---------- */
    for (int bi = 0; bi < bucket_count; ++bi) {
        AcquireSRWLockShared(&buckets[bi].lock);
        PlayerNode* cur = buckets[bi].head;
        int scanned_nodes = 0;
        while (cur && scanned_nodes < SCAN_NODES_PER_BUCKET) {
            int target_rating = cur->rating;
            /* define search range for candidates (within max_rating_diff) */
            int low = target_rating - max_rating_diff;
            int high = target_rating + max_rating_diff;
            if (low < 0) low = 0;
            if (high > MAX_RATING*2) high = MAX_RATING*2;
            int low_b = rating_to_bucket_index(low);
            int high_b = rating_to_bucket_index(high);

            for (int bj = low_b; bj <= high_b; ++bj) {
                AcquireSRWLockShared(&buckets[bj].lock);
                PlayerNode* cand = buckets[bj].head;
                int scanned_cand = 0;
                while (cand && scanned_cand < SCAN_CAND_NODES_PER_BUCKET) {
                    if (cand != cur && cand->rating == target_rating) {
                        /* compute earliest-arrival tie-break (earliest of the two players) */
                        uint64_t pair_seq = (cur->arrival_seq < cand->arrival_seq) ? cur->arrival_seq : cand->arrival_seq;
                        if (!found_a || pair_seq < found_seq) {
                            found_a = cur;
                            found_b = cand;
                            found_a_idx = bi;
                            found_b_idx = bj;
                            found_diff = 0;
                            found_seq = pair_seq;
                        }
                        /* We can stop scanning further candidates for this candidate bucket */
                        break;
                    }
                    cand = cand->next;
                    scanned_cand++;
                }
                ReleaseSRWLockShared(&buckets[bj].lock);
                if (found_diff == 0 && found_a_idx == bi) break;
            }

            if (found_diff == 0) {
                ReleaseSRWLockShared(&buckets[bi].lock);
                goto GOT_PAIR;
            }

            cur = cur->next;
            scanned_nodes++;
        }
        ReleaseSRWLockShared(&buckets[bi].lock);
    }

    /* ---------- PASS 2: nearest match (if no exact pairs) ---------- */
    for (int bi = 0; bi < bucket_count; ++bi) {
        AcquireSRWLockShared(&buckets[bi].lock);
        PlayerNode* cur = buckets[bi].head;
        int scanned_nodes = 0;
        while (cur && scanned_nodes < SCAN_NODES_PER_BUCKET) {
            int target_rating = cur->rating;
            int low = target_rating - max_rating_diff;
            int high = target_rating + max_rating_diff;
            if (low < 0) low = 0;
            if (high > MAX_RATING*2) high = MAX_RATING*2;
            int low_b = rating_to_bucket_index(low);
            int high_b = rating_to_bucket_index(high);

            for (int bj = low_b; bj <= high_b; ++bj) {
                AcquireSRWLockShared(&buckets[bj].lock);
                PlayerNode* cand = buckets[bj].head;
                int scanned_cand = 0;
                while (cand && scanned_cand < SCAN_CAND_NODES_PER_BUCKET) {
                    if (cand != cur) {
                        int diff = abs(cand->rating - target_rating);
                        if (diff <= max_rating_diff) {
                            uint64_t pair_seq = (cur->arrival_seq < cand->arrival_seq) ? cur->arrival_seq : cand->arrival_seq;
                            if (diff < found_diff || (diff == found_diff && pair_seq < found_seq)) {
                                found_diff = diff;
                                found_a = cur;
                                found_b = cand;
                                found_a_idx = bi;
                                found_b_idx = bj;
                                found_seq = pair_seq;
                            }
                        }
                    }
                    cand = cand->next;
                    scanned_cand++;
                }
                ReleaseSRWLockShared(&buckets[bj].lock);
            }

            cur = cur->next;
            scanned_nodes++;
        }
        ReleaseSRWLockShared(&buckets[bi].lock);
    }

GOT_PAIR:
    if (!found_a || !found_b) return 0;

    /* Acquire exclusive locks for both buckets in index order */
    int a = found_a_idx, b = found_b_idx;
    if (a > b) { int t=a; a=b; b=t; }
    AcquireSRWLockExclusive(&buckets[a].lock);
    if (b != a) AcquireSRWLockExclusive(&buckets[b].lock);

    /* re-validate nodes are still present by uuid */
    PlayerNode* p1 = NULL;
    PlayerNode* p2 = NULL;
    PlayerNode* scan = buckets[found_a_idx].head;
    while (scan) { if (strcmp(scan->uuid, found_a->uuid) == 0) { p1 = scan; break; } scan = scan->next; }
    scan = buckets[found_b_idx].head;
    while (scan) { if (strcmp(scan->uuid, found_b->uuid) == 0) { p2 = scan; break; } scan = scan->next; }

    if (!p1 || !p2) {
        if (b != a) ReleaseSRWLockExclusive(&buckets[b].lock);
        ReleaseSRWLockExclusive(&buckets[a].lock);
        return 0; /* race — nodes moved/taken */
    }

    /* final diff check */
    if (abs(p1->rating - p2->rating) > max_rating_diff) {
        if (b != a) ReleaseSRWLockExclusive(&buckets[b].lock);
        ReleaseSRWLockExclusive(&buckets[a].lock);
        return 0;
    }

    /* remove nodes */
    bucket_remove_node_unlocked(found_a_idx, p1);
    bucket_remove_node_unlocked(found_b_idx, p2);

    if (b != a) ReleaseSRWLockExclusive(&buckets[b].lock);
    ReleaseSRWLockExclusive(&buckets[a].lock);

    /* remove from hashmap */
    hashmap_remove(p1->uuid);
    hashmap_remove(p2->uuid);

    /* fill out match */
    strncpy(out->player1, p1->uuid, UUID_LEN-1); out->player1[UUID_LEN-1]=0;
    strncpy(out->player2, p2->uuid, UUID_LEN-1); out->player2[UUID_LEN-1]=0;
    out->match_id = (int)(++match_counter);

    /* push history and enqueue log */
    history_push(out->player1, out->player2);
    logger_enqueue(out->player1, out->player2, out->match_id, p1->rating, p2->rating);

    /* return nodes to pool */
    node_free(p1);
    node_free(p2);

    return 1;
}


/* get_all_matches: returns up to max_buf recent matches (newest first) */
EXPORT int get_all_matches(Match* buf, int max_buf) {
    if (!buf || max_buf <= 0) return 0;
    EnterCriticalSection(&history_lock);
    int written = 0;
    if (!match_history) { LeaveCriticalSection(&history_lock); return 0; }
    int idx = (int)((match_history_head + match_history_cap - 1) % match_history_cap);
    for (int i=0; i < (int)match_history_cap && written < max_buf; ++i) {
        Match m = match_history[idx];
        if (m.match_id == 0) break;
        buf[written++] = m;
        idx = (idx + match_history_cap - 1) % match_history_cap;
    }
    LeaveCriticalSection(&history_lock);
    return written;
}


/* free_engine */
EXPORT void free_engine() {
    if (!engine_inited) return;
    logger_thread_running = 0;
    WakeConditionVariable(&logger_cv);
    if (logger_thread) {
        WaitForSingleObject(logger_thread, 2000);
        CloseHandle(logger_thread);
        logger_thread = NULL;
    }
    if (buckets) { free(buckets); buckets = NULL; }
    hashmap_free_all();
    if (match_history) { free(match_history); match_history = NULL; }
    DeleteCriticalSection(&history_lock);
    DeleteCriticalSection(&hashmap_lock);
    DeleteCriticalSection(&logger_lock);
    /* free memory pool */
    PoolChunk* pc = pool_chunks;
    while (pc) {
        PoolChunk* next = pc->next;
        if (pc->nodes) {
            free(pc->nodes);   /* free the array of PlayerNode */
        }
        free(pc);              /* free the chunk header */
        pc = next;
    }
    pool_chunks = NULL;
free_list = NULL;

    engine_inited = 0;
}

/* DLL entry */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        InitializeCriticalSection(&hashmap_lock);
        InitializeCriticalSection(&logger_lock);
        InitializeConditionVariable(&logger_cv);
        break;
    case DLL_PROCESS_DETACH:
        free_engine();
        break;
    }
    return TRUE;
}
