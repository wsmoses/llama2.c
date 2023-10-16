/*
Inference for Llama-2 Transformer model in pure C.

Example compile: (see README for more details)
$ gcc -O3 -o run run.c -lm

Then run with:
$ ./run
*/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define memcpy __builtin_memcpy
// ----------------------------------------------------------------------------
// Transformer and RunState structs, and related memory management

typedef struct {
    int dim; // transformer dimension
    int hidden_dim; // for ffn layers
    int n_layers; // number of layers
    int n_heads; // number of query heads
    int n_kv_heads; // number of key/value heads (can be < query heads because of multiquery)
    int vocab_size; // vocabulary size, usually 256 (byte-level)
    int seq_len; // max sequence length
} Config;

typedef struct {
    // token embedding table
    float* __restrict__ token_embedding_table;    // (vocab_size, dim)
    // weights for rmsnorms
    float*  __restrict__ rms_att_weight; // (layer, dim) rmsnorm weights
    float*  __restrict__ rms_ffn_weight; // (layer, dim)
    // weights for matmuls
    float* __restrict__  wq; // (layer, dim, dim)
    float*  __restrict__ wk; // (layer, dim, dim)
    float*  __restrict__ wv; // (layer, dim, dim)
    float*  __restrict__ wo; // (layer, dim, dim)
    // weights for ffn
    float*  __restrict__ w1; // (layer, hidden_dim, dim)
    float*  __restrict__ w2; // (layer, dim, hidden_dim)
    float*  __restrict__ w3; // (layer, hidden_dim, dim)
    // final rmsnorm
    float*  __restrict__ rms_final_weight; // (dim,)
    // freq_cis for RoPE relatively positional embeddings
    float*  __restrict__ freq_cis_real; // (seq_len, dim/2)
    float*  __restrict__ freq_cis_imag; // (seq_len, dim/2)
    // (optional) classifier weights for the logits, on the last layer
    float*  __restrict__ wcls;
} TransformerWeights;

typedef struct {
    // current wave of activations
    float * __restrict__ x; // activation at current time stamp (dim,)
    float * __restrict__ xb; // same, but inside a residual branch (dim,)
    float * __restrict__ xb2; // an additional buffer just for convenience (dim,)
    float * __restrict__ hb; // buffer for hidden dimension in the ffn (hidden_dim,)
    float * __restrict__ hb2; // buffer for hidden dimension in the ffn (hidden_dim,)
    float * __restrict__ q; // query (dim,)
    float * __restrict__ k; // key (dim,)
    float * __restrict__ v; // value (dim,)
    float * __restrict__ att; // buffer for scores/attention values (n_heads, seq_len)
    float * __restrict__ logits; // output logits
    // kv cache
    float*  __restrict__ key_cache;   // (layer, seq_len, dim)
    float*  __restrict__ value_cache; // (layer, seq_len, dim)
} RunState;

void malloc_run_state(RunState* s, Config* p) {
    // we calloc instead of malloc to keep valgrind happy
    s->x = calloc(p->dim, sizeof(float));
    s->xb = calloc(p->dim, sizeof(float));
    s->xb2 = calloc(p->dim, sizeof(float));
    s->hb = calloc(p->hidden_dim, sizeof(float));
    s->hb2 = calloc(p->hidden_dim, sizeof(float));
    s->q = calloc(p->dim, sizeof(float));
    s->k = calloc(p->dim, sizeof(float));
    s->v = calloc(p->dim, sizeof(float));
    s->att = calloc(p->n_heads * p->seq_len, sizeof(float));
    s->logits = calloc(p->vocab_size, sizeof(float));
    s->key_cache = calloc(p->n_layers * p->seq_len * p->dim, sizeof(float));
    s->value_cache = calloc(p->n_layers * p->seq_len * p->dim, sizeof(float));
    // ensure all mallocs went fine
    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q 
     || !s->k || !s->v || !s->att || !s->logits || !s->key_cache 
     || !s->value_cache) {
        printf("malloc failed!\n");
        exit(1);
    }
}


void zero_run_state(RunState* s, Config* p) {
    // we calloc instead of malloc to keep valgrind happy
    memset(s->x, 0, p->dim * sizeof(float));
    memset(s->xb, 0, p->dim * sizeof(float));
    memset(s->xb2, 0, p->dim * sizeof(float));
    memset(s->hb, 0,p->hidden_dim * sizeof(float));
    memset(s->hb2, 0,p->hidden_dim * sizeof(float));
    memset(s->q, 0,p->dim * sizeof(float));
    memset(s->k, 0,p->dim * sizeof(float));
    memset(s->v, 0,p->dim * sizeof(float));
    memset(s->att, 0,p->n_heads * p->seq_len * sizeof(float));
    memset(s->logits, 0,p->vocab_size * sizeof(float));
    memset(s->key_cache, 0,p->n_layers * p->seq_len * p->dim * sizeof(float));
    memset(s->value_cache, 0,p->n_layers * p->seq_len * p->dim * sizeof(float));
}

void free_run_state(RunState* s) {
    free(s->x);
    free(s->xb);
    free(s->xb2);
    free(s->hb);
    free(s->hb2);
    free(s->q);
    free(s->k);
    free(s->v);
    free(s->att);
    free(s->logits);
    free(s->key_cache);
    free(s->value_cache);
}

// ----------------------------------------------------------------------------
// initialization: read from checkpoint

void checkpoint_init_weights(TransformerWeights *w, Config* p, float* f, int shared_weights) {
    float* ptr = f;
    w->token_embedding_table = ptr;
    ptr += p->vocab_size * p->dim;
    w->rms_att_weight = ptr;
    ptr += p->n_layers * p->dim;
    w->wq = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->wk = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->wv = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->wo = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->rms_ffn_weight = ptr;
    ptr += p->n_layers * p->dim;
    w->w1 = ptr;
    ptr += p->n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;
    ptr += p->n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;
    ptr += p->n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;
    ptr += p->dim;
    w->freq_cis_real = ptr;
    int head_size = p->dim / p->n_heads;
    ptr += p->seq_len * head_size / 2;
    w->freq_cis_imag = ptr;
    ptr += p->seq_len * head_size / 2;
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

// ----------------------------------------------------------------------------
// neural net blocks

void accum(float *a, float *b, int size) {
    for (int i = 0; i < size; i++) {
        a[i] += b[i];
    }
}

void rmsnorm(float* o, float* x, float* weight, int size) {
    // calculate sum of squares
    float ss = 0.0f;
    for (int j = 0; j < size; j++) {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    // normalize and scale
    for (int j = 0; j < size; j++) {
        o[j] = weight[j] * (ss * x[j]);
    }
}

void softmax(float* x, int size) {
    // find max value (for numerical stability)
    float max_val = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > max_val) {
            max_val = x[i];
        }
    }
    // exp and sum
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++) {
        x[i] /= sum;
    }
}

void matmul(float* xout, float* x, float* w, int n, int d) {
    // W (d,n) @ x (n,) -> xout (d,)
    #pragma omp parallel for
    for (int i = 0; i < d; i++) {
        float val = 0.0f;
        for (int j = 0; j < n; j++) {
            val += w[i * n + j] * x[j];
        }
        xout[i] = val;
    }
}

void transformer(int token, int pos, Config* __restrict__ p, RunState* __restrict__ s, TransformerWeights* __restrict__ w) {
    
    // a few convenience variables
    float *x = s->x;
    int dim = p->dim;
    int hidden_dim =  p->hidden_dim;
    int head_size = dim / p->n_heads;

    // copy the token embedding into x
    float* content_row = &(w->token_embedding_table[token * dim]);
    memcpy(x, content_row, dim*sizeof(*x));

    // pluck out the "pos" row of freq_cis_real and freq_cis_imag
    float* freq_cis_real_row = w->freq_cis_real + pos * head_size / 2;
    float* freq_cis_imag_row = w->freq_cis_imag + pos * head_size / 2;

    // forward all the layers
    for(int l = 0; l < p->n_layers; l++) {
    
        // attention rmsnorm
        rmsnorm(s->xb, x, w->rms_att_weight + l*dim, dim);

        // qkv matmuls for this position
        matmul(s->q, s->xb, w->wq + l*dim*dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l*dim*dim, dim, dim);
        matmul(s->v, s->xb, w->wv + l*dim*dim, dim, dim);

        // apply RoPE rotation to the q and k vectors for each head
        for (int h = 0; h < p->n_heads; h++) {
            // get the q and k vectors for this head
            float* q = s->q + h * head_size;
            float* k = s->k + h * head_size;
            // rotate q and k by the freq_cis_real and freq_cis_imag
            for (int i = 0; i < head_size; i+=2) {
                float q0 = q[i];
                float q1 = q[i+1];
                float k0 = k[i];
                float k1 = k[i+1];
                float fcr = freq_cis_real_row[i/2];
                float fci = freq_cis_imag_row[i/2];
                q[i]   = q0 * fcr - q1 * fci;
                q[i+1] = q0 * fci + q1 * fcr;
                k[i]   = k0 * fcr - k1 * fci;
                k[i+1] = k0 * fci + k1 * fcr;
            }
        }

        // save key,value at this time step (pos) to our kv cache
        int loff = l * p->seq_len * dim; // kv cache layer offset for convenience
        float* key_cache_row = s->key_cache + loff + pos * dim;
        float* value_cache_row = s->value_cache + loff + pos * dim;
        memcpy(key_cache_row, s->k, dim*sizeof(*key_cache_row));
        memcpy(value_cache_row, s->v, dim*sizeof(*value_cache_row));
        
        // multihead attention. iterate over all heads
        #pragma omp parallel for
        for (int h = 0; h < p->n_heads; h++) {
            // get the query vector for this head
            float* q = s->q + h * head_size;
            // attention scores for this head
            float* att = s->att + h * p->seq_len;
            // iterate over all timesteps, including the current one
            for (int t = 0; t <= pos; t++) {
                // get the key vector for this head and at this timestep
                float* k = s->key_cache + loff + t * dim + h * head_size;
                // calculate the attention score as the dot product of q and k
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    score += q[i] * k[i];
                }
                score /= sqrtf(head_size);
                // save the score to the attention buffer
                att[t] = score;
            }

            // softmax the scores to get attention weights, from 0..pos inclusively
            softmax(att, pos + 1);
            
            // weighted sum of the values, store back into xb
            for (int i = 0; i < head_size; i++) {
                float val = 0.0f;
                for (int t = 0; t <= pos; t++) {
                    val += att[t] * s->value_cache[loff + t * dim + h * head_size + i]; // note bad locality
                }
                s->xb[h * head_size + i] = val;
            }
        }

        // final matmul to get the output of the attention
        matmul(s->xb2, s->xb, w->wo + l*dim*dim, dim, dim);

        // residual connection back into x
        accum(x, s->xb2, dim);

        // ffn rmsnorm
        rmsnorm(s->xb, x, w->rms_ffn_weight + l*dim, dim);

        // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
        // first calculate self.w1(x) and self.w3(x)
        matmul(s->hb, s->xb, w->w1 + l*dim*hidden_dim, dim, hidden_dim);
        matmul(s->hb2, s->xb, w->w3 + l*dim*hidden_dim, dim, hidden_dim);
        
        // F.silu; silu(x)=x*σ(x),where σ(x) is the logistic sigmoid
        for (int i = 0; i < hidden_dim; i++) {
            s->hb[i] = s->hb[i] * (1.0f / (1.0f + expf(-s->hb[i])));
        }
        
        // elementwise multiply with w3(x)
        for (int i = 0; i < hidden_dim; i++) {
            s->hb[i] = s->hb[i] * s->hb2[i];
        }

        // final matmul to get the output of the ffn
        matmul(s->xb, s->hb, w->w2 + l*dim*hidden_dim, hidden_dim, dim);

        // residual connection
        accum(x, s->xb, dim);
    }
    
    // final rmsnorm
    rmsnorm(x, x, w->rms_final_weight, dim);

    // classifier into logits
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
}

int sample(float* probabilities, int n) {
    // sample index from probabilities, they must sum to 1
    float r = (float)rand() / (float)RAND_MAX;
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (r < cdf) {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}


int argmax(float* v, int n) {
    // return argmax of v in elements 0..n
    int max_i = 0;
    float max_p = v[0];
    for (int i = 1; i < n; i++) {
        if (v[i] > max_p) {
            max_i = i;
            max_p = v[i];
        }
    }
    return max_i;
}


float loss(int token, int pos, Config* __restrict__ config, RunState* __restrict__ s, TransformerWeights* __restrict__ w, int nexttok, float temperature) {
    transformer(token, pos, config, s, w);

    // apply the temperature to the logits
    for (int q=0; q<config->vocab_size; q++) { s->logits[q] /= temperature; }

    // apply softmax to the logits to get the probabilities for next token
    softmax(s->logits, config->vocab_size);

    // we now want to sample from this distribution to get the next token
    //next = sample(state.logits, config.vocab_size);
    // https://github.com/keras-team/keras/blob/21c25fd38023a3783950c5577383ffe51a62f650/keras/backend_config.py#L34
    return -log(s->logits[nexttok] + 1e-7);
}

// ----------------------------------------------------------------------------

long time_in_ms() {
    struct timespec time;
    // Get the current time with nanosecond precision
    if (clock_gettime(CLOCK_REALTIME, &time) == 0) {
        return time.tv_sec * 1000 + time.tv_nsec / 1000000;
    } else {
        perror("clock_gettime");
        return -1; // Return -1 to indicate an error
    }
}

int enzyme_const;
int enzyme_primal_return;
int enzyme_dup;
float __enzyme_autodiff(void*, 
        int,
        int, int,
        int, int,
        int, Config*,
        int, RunState*, RunState*,
        int, TransformerWeights*, TransformerWeights*,
        int,
        int, float);

int main(int argc, char *argv[]) {

    // poor man's C argparse
    char *checkpoint = NULL;  // e.g. out/model.bin
    float temperature = 0.9f; // e.g. 1.0, or 0.0
    int steps = 256;          // max number of steps to run for, 0: use seq_len
    char *training_data = NULL;
    // 'checkpoint' is necessary arg
    if (argc < 2) {
        printf("Usage: %s <checkpoint_file> [temperature] [steps] [training_data]\n", argv[0]);
        return 1;
    }
    if (argc >= 2) {
        checkpoint = argv[1];
    }
    if (argc >= 3) {
        // optional temperature. 0.0 = (deterministic) argmax sampling. 1.0 = baseline
        temperature = atof(argv[2]);
    }
    if (argc >= 4) {
        steps = atoi(argv[3]);
    }
    if(argc >= 5) {
        training_data = argv[4];
    }

    // seed rng with time. if you want deterministic behavior use temperature 0.0
    srand((unsigned int)1337);//time(NULL)); 
    
    // read in the model.bin file
    Config config;
    TransformerWeights weights;
    TransformerWeights dweights;
    int fd = 0;
    float* data = NULL;
    float* ddata = NULL;
    float* weights_ptr;
    float* dweights_ptr;
    long file_size;
    {
        FILE *file = fopen(checkpoint, "rb");
        if (!file) {
            printf("Unable to open the checkpoint file %s!\n", checkpoint);
            return 1;
        }
        // read in the config header
        if(fread(&config, sizeof(Config), 1, file) != 1) { return 1; }
        // negative vocab size is hacky way of signaling unshared weights. bit yikes.
        int shared_weights = config.vocab_size > 0 ? 1 : 0;
        config.vocab_size = abs(config.vocab_size);
        // figure out the file size
        fseek(file, 0, SEEK_END); // move file pointer to end of file
        file_size = ftell(file); // get the file size, in bytes
        fclose(file);
        // memory map the Transformer weights into the data pointer
        fd = open(checkpoint, O_RDONLY); // open in read only mode
        if (fd == -1) { printf("open failed!\n"); return 1; }
        data = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) { printf("mmap failed!\n"); return 1; }
        ddata = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ddata == MAP_FAILED) { printf("mmap failed!\n"); return 1; }
        memset(ddata, 0, file_size);
        weights_ptr = data + sizeof(Config)/sizeof(float);
        dweights_ptr = ddata + sizeof(Config)/sizeof(float);
        checkpoint_init_weights(&weights, &config, weights_ptr, shared_weights);
        checkpoint_init_weights(&dweights, &config, dweights_ptr, shared_weights);
    }
    // right now we cannot run for more than config.seq_len steps
    if (steps <= 0 || steps > config.seq_len) { steps = config.seq_len; }

    // read in the tokenizer.bin file
    char** vocab = (char**)malloc(config.vocab_size * sizeof(char*));
    {
        FILE *file = fopen("tokenizer.bin", "rb");
        if (!file) {
            printf("Unable to open the tokenizer file tokenizer.bin! Run "
            "python tokenizer.py to convert tokenizer.model -> tokenizer.bin\n");
            return 1;
        }
        int len;
        for (int i = 0; i < config.vocab_size; i++) {
            if(fread(&len, sizeof(int), 1, file) != 1) { return 1; }
            vocab[i] = (char *)malloc(len + 1);
            if(fread(vocab[i], len, 1, file) != 1) { return 1; }
            vocab[i][len] = '\0'; // add the string terminating token
        }
        fclose(file);
    }

    // create and init the application RunState
    RunState state;
    malloc_run_state(&state, &config);
    RunState dstate;
    malloc_run_state(&dstate, &config);



    

    // the current position we are in
    long start = time_in_ms();
    int next;
    int token = 1; // 1 = BOS token in Llama-2 sentencepiece
    int pos = 0;
    printf("<s>\n"); // explicit print the initial BOS token (=1), stylistically symmetric


    double alpha = 1.0;

    if(training_data){

        // read the train.txt file
        FILE *train_file = fopen(training_data, "r");
        if (!train_file) {
            printf("Unable to open train.txt\n");
            return 1;
        }
        fseek(train_file, 0, SEEK_END);
        long length = ftell(train_file);
        fseek(train_file, 0, SEEK_SET);
        char* train_text = malloc(length);
        if (fread(train_text, 1, length, train_file) != length) {
            printf("Failed to read train.txt\n");
            return 1;
        }
        fclose(train_file);
        
        // float* dweightsacc_ptr = calloc(file_size - sizeof(Config)/sizeof(float));

        // greedily match with vocab
        for (int i = 0; i < length && pos < steps; ) {
            int maxlen = -1;
            int maxj = -1;

            for (int j = 0; j < config.vocab_size; j++) {
                int len = strlen(vocab[j]);
                // 
                if (strncmp(&train_text[i], vocab[j], len) == 0 && len > maxlen) {
                    maxlen = len;
                    maxj = j;
                }
            }
            // printf("Matched token %d = '%s' at position %d with length %d\n", maxj, vocab[maxj], i, maxlen);

            i += maxlen;
            
            
            int nexttok = maxj;

            // transformer(token, pos, &config, &state, &weights);

            double lres = __enzyme_autodiff((void*)loss,
                                enzyme_primal_return,
                                enzyme_const, token,
                                enzyme_const, pos,
                                enzyme_const, &config,
                                enzyme_dup, &state, &dstate, 
                                enzyme_dup, &weights , &dweights,
                                nexttok,
                                enzyme_const, temperature);

            printf("%s %d %f\n", vocab[nexttok], pos, lres);
            fflush(stdout);

            for (size_t i =0, end=(file_size - sizeof(Config))/sizeof(float); i<end; i++) {
                if (fabs(dweights_ptr[i]) > 1000 || isnan(dweights_ptr[i])) {
                    printf("%i %f\n", i, dweights_ptr[i]);
                    exit(1);
                }
                if (fabs(dweights_ptr[i]) > 1e-2) {
                    printf("%i %f %d\n", i, dweights_ptr[i], pos);
                }
                weights_ptr[i] += alpha * dweights_ptr[i];
                dweights_ptr[i] = 0;
            }
            zero_run_state(&dstate, &config);

            token = maxj;
            pos++;
            // break;

        }

        printf("\n\nFinished fine-tuning.\n\n");

        pos = 0;
        zero_run_state(&state, &config);
        token = 1;
        printf("<s>\n"); // explicit print the initial BOS token (=1), stylistically symmetric
    }

    // free_run_state(&state);
    // malloc_run_state(&state, &config);

    // while (pos < steps) {

    //     // forward the transformer to get logits for the next token
    //     transformer(token, pos, &config, &state, &weights);
    //     // sample the next token
    //     if(temperature == 0.0f) {
    //         // greedy argmax sampling
    //         next = argmax(state.logits, config.vocab_size);
    //     } else {
    //         // apply the temperature to the logits
    //         for (int q=0; q<config.vocab_size; q++) { state.logits[q] /= temperature; }
    //         // apply softmax to the logits to get the probabilities for next token
    //         softmax(state.logits, config.vocab_size);
    //         // we now want to sample from this distribution to get the next token
    //         next = sample(state.logits, config.vocab_size);
    //     }
    //     // printf("%d\n", next);
    //     printf("%s", vocab[next]);
    //     fflush(stdout);

    //     token = next;
    //     pos++;

    // }

    while (pos < steps) {

        // forward the transformer to get logits for the next token
        transformer(token, pos, &config, &state, &weights);
        // sample the next token
        if(temperature == 0.0f) {
            // greedy argmax sampling
            next = argmax(state.logits, config.vocab_size);
        } else {
            // apply the temperature to the logits
            for (int q=0; q<config.vocab_size; q++) { state.logits[q] /= temperature; }
            // apply softmax to the logits to get the probabilities for next token
            softmax(state.logits, config.vocab_size);
            // we now want to sample from this distribution to get the next token
            next = sample(state.logits, config.vocab_size);
        }
        // printf("%d\n", next);
        printf("%s", vocab[next]);
        fflush(stdout);
        token = next;
        pos++;

    }



    // report achieved tok/s
    long end = time_in_ms();
    printf("\nachieved tok/s: %f\n", steps / (double)(end-start)*1000);

    // memory and file handles cleanup
    free_run_state(&state);
    for (int i = 0; i < config.vocab_size; i++) { free(vocab[i]); }
    free(vocab);
    if (data != MAP_FAILED) munmap(data, file_size);
    if (fd != -1) close(fd);
    return 0;
}
