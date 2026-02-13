#pragma once
#include <cstdint>
#include <cstddef>

struct GPTConfig {
    uint16_t n_embd;
    uint8_t  n_layer;
    uint8_t  n_head;
    uint16_t block_size;
    uint16_t vocab_size;
    uint16_t n_tokens;
};

struct GPTWeights {
    // Pointers into PSRAM-loaded file (zero-copy)
    const float*   tok_emb;      // [vocab_size * n_embd]
    const float*   pos_emb;      // [block_size * n_embd]
    struct Layer {
        const float*   norm1_gamma;  // [n_embd]
        const int8_t*  q_w;         // [n_embd * n_embd]
        const float*   q_s;         // [n_embd] scales
        const int8_t*  k_w;
        const float*   k_s;
        const int8_t*  v_w;
        const float*   v_s;
        const int8_t*  o_w;
        const float*   o_s;
        const float*   norm2_gamma;
        const int8_t*  mlp_up_w;    // [4*n_embd * n_embd]
        const float*   mlp_up_s;    // [4*n_embd]
        const int8_t*  mlp_down_w;  // [n_embd * 4*n_embd]
        const float*   mlp_down_s;  // [n_embd]
    };
    Layer* layers;   // [n_layer]
    const float*   final_norm_gamma;
    const int8_t*  lm_head_w;   // [vocab_size * n_embd]
    const float*   lm_head_s;   // [vocab_size]
};

struct KVCache {
    float* k;  // [n_layer * block_size * n_embd] in PSRAM
    float* v;  // same
};

struct GPTBuffers {
    // Scratch in internal SRAM for speed
    float* x;        // [n_embd]
    float* xb;       // [n_embd]
    float* q;        // [n_embd]
    float* att;      // [n_head * block_size]
    float* mlp_buf;  // [4 * n_embd]
    float* logits;   // [vocab_size]
};

struct TokenMap {
    char** tokens;    // [vocab_size] array of C strings
};

struct MiniGPT {
    GPTConfig   config;
    GPTWeights  weights;
    KVCache     cache;
    GPTBuffers  buffers;
    TokenMap    tokenMap;
    uint8_t*    fileData;  // Raw file in PSRAM (owns the allocation)
    size_t      fileSize;
    int         pos;       // Current sequence position
};

// Callback for streaming: called with each generated token string
typedef void (*GPTStreamCallback)(const char* token_str, void* user_data);

// API
bool gpt_load(MiniGPT* model, const char* path);
void gpt_free(MiniGPT* model);
char* gpt_generate(MiniGPT* model, const char* prompt, int max_tokens,
                   float temperature, GPTStreamCallback cb, void* user_data);
