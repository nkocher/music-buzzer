#include "mini_gpt.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <cstring>
#include <cmath>

// Align pointer to 4-byte boundary
static inline size_t align4(size_t offset) {
    return (offset + 3) & ~3;
}

// RMS normalization
static void rmsnorm(float* out, const float* x, const float* gamma, int n) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) {
        ss += x[i] * x[i];
    }
    ss = ss / n + 1e-5f;
    ss = 1.0f / sqrtf(ss);
    for (int i = 0; i < n; i++) {
        out[i] = x[i] * ss * gamma[i];
    }
}

// INT8 matrix-vector multiply with dequantization
// out[rows] = weight_int8[rows x cols] @ in[cols], then scale per row
static void matmul_int8(float* out, const float* in, const int8_t* weight,
                        const float* scales, int rows, int cols) {
    for (int r = 0; r < rows; r++) {
        const int8_t* row_ptr = weight + r * cols;
        float sum = 0.0f;

        // 4x loop unroll for ESP32-S3 performance
        int c = 0;
        for (; c + 3 < cols; c += 4) {
            sum += (float)row_ptr[c]   * in[c];
            sum += (float)row_ptr[c+1] * in[c+1];
            sum += (float)row_ptr[c+2] * in[c+2];
            sum += (float)row_ptr[c+3] * in[c+3];
        }
        // Handle remainder
        for (; c < cols; c++) {
            sum += (float)row_ptr[c] * in[c];
        }

        out[r] = sum * scales[r];
    }
}

// Softmax
static void softmax(float* x, int n) {
    float max_val = x[0];
    for (int i = 1; i < n; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }

    float inv_sum = 1.0f / (sum + 1e-10f);
    for (int i = 0; i < n; i++) {
        x[i] *= inv_sum;
    }
}

// Sample token from logits with temperature and top-k filtering
static int sample_token(const float* logits, int vocab_size, float temperature, int top_k = 40) {
    // Copy and apply temperature
    float* probs = (float*)malloc(vocab_size * sizeof(float));
    if (!probs) return 0;

    for (int i = 0; i < vocab_size; i++) {
        probs[i] = logits[i] / temperature;
    }

    // Apply top-k filtering
    if (top_k > 0 && top_k < vocab_size) {
        // Find the top_k-th largest value
        float* sorted_logits = (float*)malloc(vocab_size * sizeof(float));
        if (sorted_logits) {
            memcpy(sorted_logits, probs, vocab_size * sizeof(float));

            // Simple selection: find k-th largest by partial sort
            for (int i = 0; i < top_k; i++) {
                for (int j = i + 1; j < vocab_size; j++) {
                    if (sorted_logits[j] > sorted_logits[i]) {
                        float tmp = sorted_logits[i];
                        sorted_logits[i] = sorted_logits[j];
                        sorted_logits[j] = tmp;
                    }
                }
            }
            float threshold = sorted_logits[top_k - 1];
            free(sorted_logits);

            // Zero out logits below threshold
            for (int i = 0; i < vocab_size; i++) {
                if (probs[i] < threshold) {
                    probs[i] = -1e9f;  // Large negative to avoid NaN in softmax
                }
            }
        }
    }

    softmax(probs, vocab_size);

    // Random sample using esp_random()
    uint32_t r = esp_random();
    float threshold = (float)r / (float)UINT32_MAX;

    float cumsum = 0.0f;
    int selected = 0;
    for (int i = 0; i < vocab_size; i++) {
        cumsum += probs[i];
        if (cumsum >= threshold) {
            selected = i;
            break;
        }
    }

    free(probs);
    return selected;
}

// Load model from LittleFS
bool gpt_load(MiniGPT* model, const char* path) {
    Serial.printf("[GPT] Loading model from %s\n", path);

    // Open file
    if (!LittleFS.begin()) {
        Serial.println("[GPT] LittleFS mount failed");
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[GPT] Failed to open %s\n", path);
        return false;
    }

    model->fileSize = f.size();
    Serial.printf("[GPT] File size: %u bytes\n", model->fileSize);

    // Allocate in PSRAM
    model->fileData = (uint8_t*)heap_caps_malloc(model->fileSize, MALLOC_CAP_SPIRAM);
    if (!model->fileData) {
        Serial.println("[GPT] PSRAM allocation failed");
        f.close();
        return false;
    }

    // Read entire file
    size_t bytes_read = f.read(model->fileData, model->fileSize);
    f.close();

    if (bytes_read != model->fileSize) {
        Serial.printf("[GPT] Read failed: %u/%u bytes\n", bytes_read, model->fileSize);
        heap_caps_free(model->fileData);
        return false;
    }

    Serial.printf("[GPT] File loaded into PSRAM (%u bytes)\n", model->fileSize);

    // Parse header (32 bytes)
    const uint8_t* ptr = model->fileData;

    // Check magic "MGPT"
    if (memcmp(ptr, "MGPT", 4) != 0) {
        Serial.println("[GPT] Invalid magic number");
        heap_caps_free(model->fileData);
        return false;
    }
    ptr += 4;

    uint8_t version = ptr[0];
    uint8_t quant_type = ptr[1];
    ptr += 2;

    if (version != 1) {
        Serial.printf("[GPT] Unsupported version: %d\n", version);
        heap_caps_free(model->fileData);
        return false;
    }

    if (quant_type != 1) {
        Serial.printf("[GPT] Expected INT8 quantization, got %d\n", quant_type);
        heap_caps_free(model->fileData);
        return false;
    }

    // Read config (little-endian)
    model->config.n_embd = ptr[0] | (ptr[1] << 8);
    ptr += 2;
    model->config.n_layer = ptr[0];
    model->config.n_head = ptr[1];
    ptr += 2;
    model->config.block_size = ptr[0] | (ptr[1] << 8);
    ptr += 2;
    model->config.vocab_size = ptr[0] | (ptr[1] << 8);
    ptr += 2;
    model->config.n_tokens = ptr[0] | (ptr[1] << 8);
    ptr += 2;

    // Skip reserved bytes (14 bytes)
    ptr += 14;

    Serial.printf("[GPT] Config: n_embd=%d, n_layer=%d, n_head=%d, block_size=%d, vocab=%d\n",
        model->config.n_embd, model->config.n_layer, model->config.n_head,
        model->config.block_size, model->config.vocab_size);

    // Parse token mapping
    size_t offset = 32;  // After header
    model->tokenMap.tokens = (char**)malloc(model->config.vocab_size * sizeof(char*));
    if (!model->tokenMap.tokens) {
        Serial.println("[GPT] Token map allocation failed");
        heap_caps_free(model->fileData);
        return false;
    }

    for (int i = 0; i < model->config.vocab_size; i++) {
        uint8_t len = model->fileData[offset];
        offset++;

        model->tokenMap.tokens[i] = (char*)malloc(len + 1);
        if (!model->tokenMap.tokens[i]) {
            Serial.printf("[GPT] Token %d allocation failed\n", i);
            // Free previously allocated tokens
            for (int j = 0; j < i; j++) {
                free(model->tokenMap.tokens[j]);
            }
            free(model->tokenMap.tokens);
            heap_caps_free(model->fileData);
            return false;
        }

        memcpy(model->tokenMap.tokens[i], model->fileData + offset, len);
        model->tokenMap.tokens[i][len] = '\0';
        offset += len;
    }

    // Align to 4 bytes
    offset = align4(offset);

    Serial.printf("[GPT] Token map parsed (%d tokens), offset now %u\n",
        model->config.vocab_size, offset);

    // Set up weight pointers (zero-copy)
    int n_embd = model->config.n_embd;
    int n_layer = model->config.n_layer;
    int vocab_size = model->config.vocab_size;
    int block_size = model->config.block_size;

    // Token embedding
    model->weights.tok_emb = (const float*)(model->fileData + offset);
    offset += vocab_size * n_embd * sizeof(float);

    // Position embedding
    model->weights.pos_emb = (const float*)(model->fileData + offset);
    offset += block_size * n_embd * sizeof(float);

    // Allocate layer array
    model->weights.layers = (GPTWeights::Layer*)malloc(n_layer * sizeof(GPTWeights::Layer));
    if (!model->weights.layers) {
        Serial.println("[GPT] Layer array allocation failed");
        for (int i = 0; i < model->config.vocab_size; i++) {
            free(model->tokenMap.tokens[i]);
        }
        free(model->tokenMap.tokens);
        heap_caps_free(model->fileData);
        return false;
    }

    // Parse each layer
    for (int l = 0; l < n_layer; l++) {
        GPTWeights::Layer& layer = model->weights.layers[l];

        // norm1_gamma
        layer.norm1_gamma = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);

        // Q weights (int8 + scales)
        layer.q_w = (const int8_t*)(model->fileData + offset);
        offset += n_embd * n_embd * sizeof(int8_t);
        layer.q_s = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);

        // K weights
        layer.k_w = (const int8_t*)(model->fileData + offset);
        offset += n_embd * n_embd * sizeof(int8_t);
        layer.k_s = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);

        // V weights
        layer.v_w = (const int8_t*)(model->fileData + offset);
        offset += n_embd * n_embd * sizeof(int8_t);
        layer.v_s = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);

        // O weights
        layer.o_w = (const int8_t*)(model->fileData + offset);
        offset += n_embd * n_embd * sizeof(int8_t);
        layer.o_s = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);

        // norm2_gamma
        layer.norm2_gamma = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);

        // MLP up
        layer.mlp_up_w = (const int8_t*)(model->fileData + offset);
        offset += 4 * n_embd * n_embd * sizeof(int8_t);
        layer.mlp_up_s = (const float*)(model->fileData + offset);
        offset += 4 * n_embd * sizeof(float);

        // MLP down
        layer.mlp_down_w = (const int8_t*)(model->fileData + offset);
        offset += n_embd * 4 * n_embd * sizeof(int8_t);
        layer.mlp_down_s = (const float*)(model->fileData + offset);
        offset += n_embd * sizeof(float);
    }

    // Final norm
    model->weights.final_norm_gamma = (const float*)(model->fileData + offset);
    offset += n_embd * sizeof(float);

    // LM head
    model->weights.lm_head_w = (const int8_t*)(model->fileData + offset);
    offset += vocab_size * n_embd * sizeof(int8_t);
    model->weights.lm_head_s = (const float*)(model->fileData + offset);
    offset += vocab_size * sizeof(float);

    Serial.printf("[GPT] Weight pointers set, final offset=%u\n", offset);

    // Allocate KV cache in PSRAM
    size_t kv_size = n_layer * block_size * n_embd * sizeof(float);
    model->cache.k = (float*)heap_caps_malloc(kv_size, MALLOC_CAP_SPIRAM);
    model->cache.v = (float*)heap_caps_malloc(kv_size, MALLOC_CAP_SPIRAM);

    if (!model->cache.k || !model->cache.v) {
        Serial.println("[GPT] KV cache allocation failed");
        if (model->cache.k) heap_caps_free(model->cache.k);
        if (model->cache.v) heap_caps_free(model->cache.v);
        free(model->weights.layers);
        for (int i = 0; i < model->config.vocab_size; i++) {
            free(model->tokenMap.tokens[i]);
        }
        free(model->tokenMap.tokens);
        heap_caps_free(model->fileData);
        return false;
    }

    Serial.printf("[GPT] KV cache allocated in PSRAM (%u bytes x 2)\n", kv_size);

    // Allocate activation buffers in internal SRAM (prefer fast memory)
    model->buffers.x = (float*)heap_caps_malloc(n_embd * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->buffers.xb = (float*)heap_caps_malloc(n_embd * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->buffers.q = (float*)heap_caps_malloc(n_embd * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->buffers.att = (float*)heap_caps_malloc(model->config.n_head * block_size * sizeof(float),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->buffers.mlp_buf = (float*)heap_caps_malloc(4 * n_embd * sizeof(float),
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model->buffers.logits = (float*)heap_caps_malloc(vocab_size * sizeof(float),
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!model->buffers.x || !model->buffers.xb || !model->buffers.q ||
        !model->buffers.att || !model->buffers.mlp_buf || !model->buffers.logits) {
        Serial.println("[GPT] Activation buffer allocation failed");
        // Free everything
        if (model->buffers.x) heap_caps_free(model->buffers.x);
        if (model->buffers.xb) heap_caps_free(model->buffers.xb);
        if (model->buffers.q) heap_caps_free(model->buffers.q);
        if (model->buffers.att) heap_caps_free(model->buffers.att);
        if (model->buffers.mlp_buf) heap_caps_free(model->buffers.mlp_buf);
        if (model->buffers.logits) heap_caps_free(model->buffers.logits);
        heap_caps_free(model->cache.k);
        heap_caps_free(model->cache.v);
        free(model->weights.layers);
        for (int i = 0; i < model->config.vocab_size; i++) {
            free(model->tokenMap.tokens[i]);
        }
        free(model->tokenMap.tokens);
        heap_caps_free(model->fileData);
        return false;
    }

    Serial.println("[GPT] Activation buffers allocated in internal SRAM");

    model->pos = 0;

    Serial.println("[GPT] Model loaded successfully!");
    Serial.printf("[GPT] Free heap: %u, Free PSRAM: %u\n",
        ESP.getFreeHeap(), ESP.getFreePsram());

    return true;
}

// Free model
void gpt_free(MiniGPT* model) {
    if (model->fileData) {
        heap_caps_free(model->fileData);
        model->fileData = nullptr;
    }

    if (model->tokenMap.tokens) {
        for (int i = 0; i < model->config.vocab_size; i++) {
            if (model->tokenMap.tokens[i]) {
                free(model->tokenMap.tokens[i]);
            }
        }
        free(model->tokenMap.tokens);
        model->tokenMap.tokens = nullptr;
    }

    if (model->weights.layers) {
        free(model->weights.layers);
        model->weights.layers = nullptr;
    }

    if (model->cache.k) {
        heap_caps_free(model->cache.k);
        model->cache.k = nullptr;
    }

    if (model->cache.v) {
        heap_caps_free(model->cache.v);
        model->cache.v = nullptr;
    }

    if (model->buffers.x) heap_caps_free(model->buffers.x);
    if (model->buffers.xb) heap_caps_free(model->buffers.xb);
    if (model->buffers.q) heap_caps_free(model->buffers.q);
    if (model->buffers.att) heap_caps_free(model->buffers.att);
    if (model->buffers.mlp_buf) heap_caps_free(model->buffers.mlp_buf);
    if (model->buffers.logits) heap_caps_free(model->buffers.logits);

    Serial.println("[GPT] Model freed");
}

// Forward pass for single token
static void gpt_forward_token(MiniGPT* model, int token_id) {
    GPTConfig& cfg = model->config;
    GPTWeights& w = model->weights;
    KVCache& cache = model->cache;
    GPTBuffers& buf = model->buffers;

    int n_embd = cfg.n_embd;
    int n_layer = cfg.n_layer;
    int n_head = cfg.n_head;
    int head_dim = n_embd / n_head;
    int pos = model->pos;

    // Start with token + position embedding
    const float* tok_emb = w.tok_emb + token_id * n_embd;
    const float* pos_emb = w.pos_emb + pos * n_embd;

    for (int i = 0; i < n_embd; i++) {
        buf.x[i] = tok_emb[i] + pos_emb[i];
    }

    // Transformer layers
    for (int l = 0; l < n_layer; l++) {
        GPTWeights::Layer& layer = w.layers[l];

        // RMSNorm
        rmsnorm(buf.xb, buf.x, layer.norm1_gamma, n_embd);

        // Q, K, V projections
        matmul_int8(buf.q, buf.xb, layer.q_w, layer.q_s, n_embd, n_embd);

        float* k_cache = cache.k + l * cfg.block_size * n_embd + pos * n_embd;
        float* v_cache = cache.v + l * cfg.block_size * n_embd + pos * n_embd;

        matmul_int8(k_cache, buf.xb, layer.k_w, layer.k_s, n_embd, n_embd);
        matmul_int8(v_cache, buf.xb, layer.v_w, layer.v_s, n_embd, n_embd);

        // Multi-head attention
        for (int h = 0; h < n_head; h++) {
            float* q_head = buf.q + h * head_dim;
            float* att_head = buf.att + h * cfg.block_size;

            // Compute attention scores for all positions up to current
            for (int t = 0; t <= pos; t++) {
                float* k_t = cache.k + l * cfg.block_size * n_embd + t * n_embd + h * head_dim;

                float score = 0.0f;
                for (int d = 0; d < head_dim; d++) {
                    score += q_head[d] * k_t[d];
                }
                score /= sqrtf((float)head_dim);
                att_head[t] = score;
            }

            // Softmax over valid positions
            softmax(att_head, pos + 1);

            // Weighted sum of values
            float* out_head = buf.xb + h * head_dim;
            for (int d = 0; d < head_dim; d++) {
                out_head[d] = 0.0f;
            }

            for (int t = 0; t <= pos; t++) {
                float* v_t = cache.v + l * cfg.block_size * n_embd + t * n_embd + h * head_dim;
                float att_weight = att_head[t];

                for (int d = 0; d < head_dim; d++) {
                    out_head[d] += att_weight * v_t[d];
                }
            }
        }

        // Output projection
        matmul_int8(buf.q, buf.xb, layer.o_w, layer.o_s, n_embd, n_embd);

        // Residual connection
        for (int i = 0; i < n_embd; i++) {
            buf.x[i] += buf.q[i];
        }

        // RMSNorm
        rmsnorm(buf.xb, buf.x, layer.norm2_gamma, n_embd);

        // MLP: up projection -> ReLU -> down projection
        matmul_int8(buf.mlp_buf, buf.xb, layer.mlp_up_w, layer.mlp_up_s, 4 * n_embd, n_embd);

        // ReLU activation
        for (int i = 0; i < 4 * n_embd; i++) {
            if (buf.mlp_buf[i] < 0.0f) buf.mlp_buf[i] = 0.0f;
        }

        matmul_int8(buf.q, buf.mlp_buf, layer.mlp_down_w, layer.mlp_down_s, n_embd, 4 * n_embd);

        // Residual connection
        for (int i = 0; i < n_embd; i++) {
            buf.x[i] += buf.q[i];
        }
    }

    // Final norm
    rmsnorm(buf.xb, buf.x, w.final_norm_gamma, n_embd);

    // LM head
    matmul_int8(buf.logits, buf.xb, w.lm_head_w, w.lm_head_s, cfg.vocab_size, n_embd);
}

// Generate text
char* gpt_generate(MiniGPT* model, const char* prompt, int max_tokens,
                   float temperature, GPTStreamCallback cb, void* user_data) {
    Serial.printf("[GPT] Generate: prompt=\"%s\", max_tokens=%d, temp=%.2f\n",
        prompt, max_tokens, temperature);

    // Reset position
    model->pos = 0;

    // Encode prompt (simple greedy matching for now)
    int prompt_tokens[128];
    int prompt_len = 0;

    const char* p = prompt;
    while (*p && prompt_len < 128) {
        bool found = false;

        // Try to match longest token first
        for (int len = 16; len > 0; len--) {
            if (p + len > prompt + strlen(prompt)) continue;

            for (int i = 0; i < model->config.vocab_size; i++) {
                if (strncmp(p, model->tokenMap.tokens[i], len) == 0 &&
                    strlen(model->tokenMap.tokens[i]) == (size_t)len) {
                    prompt_tokens[prompt_len++] = i;
                    p += len;
                    found = true;
                    break;
                }
            }

            if (found) break;
        }

        if (!found) {
            // Skip unknown character
            p++;
        }
    }

    Serial.printf("[GPT] Prompt encoded: %d tokens\n", prompt_len);

    // Process prompt tokens (no sampling)
    for (int i = 0; i < prompt_len; i++) {
        gpt_forward_token(model, prompt_tokens[i]);
        model->pos++;
    }

    // Generate new tokens (start with prompt in result)
    String result = prompt;
    int tokens_generated = 0;

    // Repetition penalty tracking
    static constexpr int REP_WINDOW = 30;
    static constexpr float REP_PENALTY = 1.2f;
    int recent_tokens[REP_WINDOW];
    int recent_count = 0;
    int recent_idx = 0;

    while (tokens_generated < max_tokens && model->pos < model->config.block_size - 1) {
        // Apply repetition penalty
        for (int i = 0; i < recent_count; i++) {
            int tok = recent_tokens[i];
            if (tok >= 0 && tok < model->config.vocab_size) {
                float* logit = &model->buffers.logits[tok];
                // Sign-aware penalty: reduce probability regardless of logit sign
                if (*logit > 0) {
                    *logit /= REP_PENALTY;
                } else {
                    *logit *= REP_PENALTY;  // Makes negative logits MORE negative
                }
            }
        }

        // Sample next token
        int next_token = sample_token(model->buffers.logits, model->config.vocab_size, temperature);

        // Check for EOS (token 2) or PAD (token 0)
        if (next_token == 2 || next_token == 0) {
            Serial.println("[GPT] EOS/PAD token generated");
            break;
        }

        // Append to result
        const char* token_str = model->tokenMap.tokens[next_token];
        result += token_str;

        // Call streaming callback
        if (cb) {
            cb(token_str, user_data);
        }

        // Track token for repetition penalty
        recent_tokens[recent_idx] = next_token;
        recent_idx = (recent_idx + 1) % REP_WINDOW;
        if (recent_count < REP_WINDOW) recent_count++;

        // Forward pass for next token
        gpt_forward_token(model, next_token);
        model->pos++;
        tokens_generated++;

        // Yield to other tasks every 10 tokens
        if (tokens_generated % 10 == 0) {
            vTaskDelay(1);
        }
    }

    Serial.printf("[GPT] Generation complete: %d tokens\n", tokens_generated);

    // Return allocated string (caller must free)
    char* output = (char*)malloc(result.length() + 1);
    if (output) {
        strcpy(output, result.c_str());
    }

    return output;
}
