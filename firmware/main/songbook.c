#include "songbook.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs.h"

static const char *TAG = "songbook";

/* ---- 介质布局 ---- */
#define PART_SUBTYPE     ((esp_partition_subtype_t)0x41)
#define SLOT_COUNT       SONGBOOK_SLOTS
#define SLOT_HDR_MAGIC   0x4D42534Eu /* "MBSN" */
#define SLOT_HDR_VALID   0xA5A5A5A5u
#define SLOT_DATA_OFF    0x1000u /* MP3 数据从槽内 4KB 边界开始，槽头独占首扇区 */
#define FLASH_SECTOR     0x1000u

#define NVS_NAMESPACE    "songbook"
#define NVS_KEY_SEQ      "seq"
#define NVS_KEY_CUR      "cur"

typedef struct {
    uint32_t magic;            /* SLOT_HDR_MAGIC */
    uint32_t valid;            /* SLOT_HDR_VALID 表示该槽有效 */
    char     title[SONGBOOK_TITLE_MAX];
    uint32_t size;             /* MP3 数据字节数 */
    uint32_t duration;         /* 秒，0=未知（M2 不写） */
    uint32_t seq;              /* 写入序号，全局递增，越小越旧 */
    uint32_t crc32;            /* MP3 数据 CRC32（esp_rom_crc32_le，初值 0） */
    uint32_t reserved[6];
} slot_header_t;
_Static_assert(sizeof(slot_header_t) == 96, "slot header must stay 96B");

typedef struct {
    bool     valid;
    char     title[SONGBOOK_TITLE_MAX];
    uint32_t size;
    uint32_t duration;
    uint32_t seq;
    uint32_t crc32;
} song_meta_t;

static const esp_partition_t *s_part;
static uint32_t s_slot_region; /* 每槽字节数 = 分区大小 / 4 */
static song_meta_t s_meta[SLOT_COUNT];
static int s_count;
static int s_current = -1;
static uint32_t s_seq_next = 1;

/* 写会话状态 */
static bool     s_wr_active;
static int      s_wr_slot;
static uint32_t s_wr_written;
static uint32_t s_wr_crc;
static uint32_t s_wr_declared;
static char     s_wr_title[SONGBOOK_TITLE_MAX];

static uint32_t slot_base(int slot)
{
    return (uint32_t)slot * s_slot_region;
}

static void nvs_save_current(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_CUR, s_current);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_save_seq(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_SEQ, s_seq_next);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool header_valid(const slot_header_t *h)
{
    return h->magic == SLOT_HDR_MAGIC && h->valid == SLOT_HDR_VALID
        && h->size > 0 && h->size <= s_slot_region - SLOT_DATA_OFF;
}

/* 返回 s 的不超过 max 字节、且以完整 UTF-8 字符结尾的最长前缀长度。
 * 用于 48B 标题截断：硬切会把多字节字符拦腰切断，日志/网页显示乱码 */
static size_t utf8_prefix_len(const char *s, size_t max)
{
    size_t n = 0, boundary = 0;
    while (s[n] != '\0' && n < max) {
        boundary = n;
        uint8_t c = (uint8_t)s[n];
        if (c < 0x80) {
            n += 1;
        } else if ((c & 0xE0) == 0xC0) {
            n += 2;
        } else if ((c & 0xF0) == 0xE0) {
            n += 3;
        } else {
            n += 4;
        }
    }
    if (s[n] == '\0') {
        return n; /* 整串放得下，未被截断 */
    }
    return boundary; /* 截断：回退到最后一个完整字符的起点 */
}

static esp_err_t scan_slot(int slot)
{
    slot_header_t h;
    esp_err_t err = esp_partition_read(s_part, slot_base(slot), &h, sizeof(h));
    if (err != ESP_OK) {
        return err;
    }
    song_meta_t *m = &s_meta[slot];
    memset(m, 0, sizeof(*m));
    if (!header_valid(&h)) {
        ESP_LOGI(TAG, "slot %d: empty", slot);
        return ESP_OK;
    }
    m->valid = true;
    memcpy(m->title, h.title, SONGBOOK_TITLE_MAX);
    m->title[SONGBOOK_TITLE_MAX - 1] = '\0'; /* 兜底截断，防脏数据无 NUL */
    /* 兼容旧固件写入的截断标题：把腰斩的多字节字符残段裁掉（只改 RAM 副本） */
    m->title[utf8_prefix_len(m->title, SONGBOOK_TITLE_MAX - 1)] = '\0';
    m->size = h.size;
    m->duration = h.duration;
    m->seq = h.seq;
    m->crc32 = h.crc32;
    if (h.seq >= s_seq_next) {
        s_seq_next = h.seq + 1;
    }
    ESP_LOGI(TAG, "slot %d: \"%s\" %lu B seq %lu", slot, m->title,
             (unsigned long)m->size, (unsigned long)m->seq);
    return ESP_OK;
}

esp_err_t songbook_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, "songs");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "songs partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    s_slot_region = s_part->size / SLOT_COUNT;
    ESP_LOGI(TAG, "partition @0x%lx size 0x%lx, slot region 0x%lx (%lu B payload)",
             (unsigned long)s_part->address, (unsigned long)s_part->size,
             (unsigned long)s_slot_region, (unsigned long)(s_slot_region - SLOT_DATA_OFF));

    /* NVS 恢复全局序号与当前索引；扫描槽头（CRC 留到写入时校验，开机不全量过 13MB） */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint32_t seq;
        if (nvs_get_u32(h, NVS_KEY_SEQ, &seq) == ESP_OK && seq > 0) {
            s_seq_next = seq;
        }
        int32_t cur;
        if (nvs_get_i32(h, NVS_KEY_CUR, &cur) == ESP_OK) {
            s_current = (int)cur;
        }
        nvs_close(h);
    }

    s_count = 0;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (scan_slot(i) == ESP_OK && s_meta[i].valid) {
            s_count++;
        }
    }
    if (s_current < 0 || s_current >= SLOT_COUNT || !s_meta[s_current].valid) {
        /* 当前索引失效：回落到第一个有效槽（不播，仅定位） */
        s_current = -1;
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (s_meta[i].valid) {
                s_current = i;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "library: %d song(s), current=%d", s_count, s_current);
    return ESP_OK;
}

int songbook_count(void)
{
    return s_count;
}

int songbook_current(void)
{
    return s_current;
}

const char *songbook_get_title(int index)
{
    if (index < 0 || index >= SLOT_COUNT || !s_meta[index].valid) {
        return NULL;
    }
    return s_meta[index].title;
}

uint32_t songbook_slot_size(void)
{
    return s_slot_region - SLOT_DATA_OFF;
}

int songbook_next(void)
{
    if (s_count == 0) {
        return -1;
    }
    int start = s_current < 0 ? 0 : s_current;
    for (int step = 1; step <= SLOT_COUNT; step++) {
        int idx = (start + step) % SLOT_COUNT;
        if (s_meta[idx].valid) {
            s_current = idx;
            nvs_save_current();
            return idx;
        }
    }
    return -1; /* 不可达（count>0 必有有效槽） */
}

esp_err_t songbook_read(int index, uint32_t offset, void *buf, size_t len, size_t *out_len)
{
    if (index < 0 || index >= SLOT_COUNT || !s_meta[index].valid || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const song_meta_t *m = &s_meta[index];
    if (offset >= m->size) {
        if (out_len) {
            *out_len = 0;
        }
        return ESP_OK; /* 读到尾 */
    }
    if (offset + len > m->size) {
        len = m->size - offset;
    }
    esp_err_t err = esp_partition_read(s_part, slot_base(index) + SLOT_DATA_OFF + offset, buf, len);
    if (out_len) {
        *out_len = (err == ESP_OK) ? len : 0;
    }
    return err;
}

/* 选槽：优先无效空槽；无空槽覆盖 seq 最小（最旧）的槽 */
static int pick_slot(void)
{
    int oldest = -1;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!s_meta[i].valid) {
            return i;
        }
        if (oldest < 0 || s_meta[i].seq < s_meta[oldest].seq) {
            oldest = i;
        }
    }
    return oldest;
}

esp_err_t songbook_write_begin(const char *title, uint32_t size)
{
    if (s_wr_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_part == NULL || size == 0 || size > s_slot_region - SLOT_DATA_OFF) {
        return ESP_ERR_INVALID_SIZE;
    }
    int slot = pick_slot();
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 擦：槽头首扇区 + 数据区（按声明 size 对齐到扇区）。3MB 全量擦约数十秒，阻塞，
     * 属写板路径（M3 上位机触发 / REPL 验证），不在播放/录音热路径上。 */
    uint32_t erase_len = (size + FLASH_SECTOR - 1) & ~(FLASH_SECTOR - 1);
    ESP_LOGI(TAG, "write begin: slot %d (was %s), erase %lu B...",
             slot, s_meta[slot].valid ? s_meta[slot].title : "empty",
             (unsigned long)(SLOT_DATA_OFF + erase_len));
    esp_err_t err = esp_partition_erase_range(s_part, slot_base(slot),
                                              SLOT_DATA_OFF + erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "slot %d erase failed: %s", slot, esp_err_to_name(err));
        return err;
    }

    s_wr_active = true;
    s_wr_slot = slot;
    s_wr_written = 0;
    s_wr_crc = 0;
    s_wr_declared = size;
    memset(s_wr_title, 0, sizeof(s_wr_title));
    if (title) {
        size_t keep = utf8_prefix_len(title, SONGBOOK_TITLE_MAX - 1);
        memcpy(s_wr_title, title, keep); /* 余字节已置 0，保证 NUL 结尾 */
    }
    return ESP_OK;
}

esp_err_t songbook_write_chunk(const void *buf, size_t len)
{
    if (!s_wr_active || buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wr_written + len > s_wr_declared) {
        return ESP_ERR_INVALID_SIZE; /* 超出声明长度：调用方协议错误 */
    }
    esp_err_t err = esp_partition_write(s_part,
                                        slot_base(s_wr_slot) + SLOT_DATA_OFF + s_wr_written,
                                        buf, len);
    if (err != ESP_OK) {
        return err;
    }
    s_wr_crc = esp_rom_crc32_le(s_wr_crc, buf, (uint32_t)len);
    s_wr_written += (uint32_t)len;
    return ESP_OK;
}

esp_err_t songbook_write_end(uint32_t duration_sec)
{
    if (!s_wr_active) {
        return ESP_ERR_INVALID_STATE;
    }
    int slot = s_wr_slot;
    uint32_t size = s_wr_written;
    s_wr_active = false;

    if (size == 0 || size != s_wr_declared) {
        ESP_LOGW(TAG, "write end: size mismatch (%lu/%lu), slot %d discarded",
                 (unsigned long)size, (unsigned long)s_wr_declared, slot);
        return ESP_ERR_INVALID_SIZE; /* 槽头未写，槽保持无效 */
    }

    /* 槽头最后写：写数据期间掉电时旧头无效，不会留下半首歌被当有效 */
    slot_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = SLOT_HDR_MAGIC;
    h.valid = SLOT_HDR_VALID;
    memcpy(h.title, s_wr_title, SONGBOOK_TITLE_MAX);
    h.size = size;
    h.duration = duration_sec;
    h.seq = s_seq_next;
    h.crc32 = s_wr_crc;
    esp_err_t err = esp_partition_write(s_part, slot_base(slot), &h, sizeof(h));
    if (err != ESP_OK) {
        return err;
    }
    s_seq_next++;
    nvs_save_seq();

    bool was_valid = s_meta[slot].valid;
    s_meta[slot].valid = true;
    memcpy(s_meta[slot].title, s_wr_title, SONGBOOK_TITLE_MAX);
    s_meta[slot].size = size;
    s_meta[slot].duration = duration_sec;
    s_meta[slot].seq = h.seq;
    s_meta[slot].crc32 = s_wr_crc;
    if (!was_valid) {
        s_count++;
    }
    if (s_current < 0) {
        s_current = slot;
        nvs_save_current();
    }
    ESP_LOGI(TAG, "slot %d saved: \"%s\" %lu B seq %lu", slot,
             s_meta[slot].title, (unsigned long)size, (unsigned long)h.seq);
    return ESP_OK;
}

int songbook_write_slot(void)
{
    return s_wr_active ? s_wr_slot : -1;
}

uint32_t songbook_get_size(int index)
{
    if (index < 0 || index >= SLOT_COUNT || !s_meta[index].valid) {
        return 0;
    }
    return s_meta[index].size;
}

uint32_t songbook_get_duration(int index)
{
    if (index < 0 || index >= SLOT_COUNT || !s_meta[index].valid) {
        return 0;
    }
    return s_meta[index].duration;
}

void songbook_write_abort(void)
{
    if (!s_wr_active) {
        return;
    }
    s_wr_active = false;
    ESP_LOGW(TAG, "write aborted, slot %d stays invalid (header never written)", s_wr_slot);
}

esp_err_t songbook_format(void)
{
    if (s_part == NULL || s_wr_active) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只擦 4 个槽头扇区即全库失效（数据残留无妨，逐槽复写时会先擦） */
    for (int i = 0; i < SLOT_COUNT; i++) {
        esp_err_t err = esp_partition_erase_range(s_part, slot_base(i), FLASH_SECTOR);
        if (err != ESP_OK) {
            return err;
        }
        memset(&s_meta[i], 0, sizeof(s_meta[i]));
    }
    s_count = 0;
    s_current = -1;
    nvs_save_current();
    ESP_LOGW(TAG, "formatted (all slot headers erased)");
    return ESP_OK;
}
