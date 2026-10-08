#include "channel_source_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

typedef struct {
    FILE *fp;
} fake_ctx_t;

static esp_err_t fake_open(void *ctx) {
    fake_ctx_t *c = (fake_ctx_t *)ctx;
    return c->fp ? ESP_OK : ESP_FAIL;
}

static int fake_read(void *ctx, uint8_t *buf, size_t maxlen, uint32_t timeout_ms) {
    (void)timeout_ms; // Fake source has no flow control to simulate yet.
    fake_ctx_t *c = (fake_ctx_t *)ctx;
    if (!c->fp) {
        return -1;
    }
    size_t n = fread(buf, 1, maxlen, c->fp);
    if (n == 0) {
        return feof(c->fp) ? -1 : 0;
    }
    return (int)n;
}

static bool fake_is_connected(void *ctx) {
    fake_ctx_t *c = (fake_ctx_t *)ctx;
    return c->fp != NULL && !feof(c->fp);
}

esp_err_t channel_source_fake_create(const char *backing_path, channel_source_t *out_source) {
    fake_ctx_t *ctx = calloc(1, sizeof(fake_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->fp = fopen(backing_path, "rb");
    if (!ctx->fp) {
        free(ctx);
        return ESP_FAIL;
    }
    out_source->open = fake_open;
    out_source->read = fake_read;
    out_source->is_connected = fake_is_connected;
    out_source->ctx = ctx;
    return ESP_OK;
}

void channel_source_fake_destroy(channel_source_t *source) {
    if (!source || !source->ctx) {
        return;
    }
    fake_ctx_t *ctx = (fake_ctx_t *)source->ctx;
    if (ctx->fp) {
        fclose(ctx->fp);
    }
    free(ctx);
    source->ctx = NULL;
}
