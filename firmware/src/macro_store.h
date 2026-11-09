#pragma once
/* Macro blob store on the "media" flash partition via LittleFS.
 * Files: "mNN" (NN = macro id), blob = map_macro_hdr_t + steps + crc16.
 * Writes are staged in RAM (macro_store_stage_write) and flushed to flash
 * by macro_store_stage_commit() in task context. */

#include <stdint.h>
#include <stdbool.h>
#include "ds5map.h"
#include "lfs.h"

bool            macro_store_init(void);
lfs_t          *macro_store_lfs(void);

int             macro_store_count(void);                 /* valid macros */
const map_macro_dir_t *macro_store_dir(void);            /* array[MAP_MACRO_MAX_COUNT], id-indexed */
bool            macro_store_find(uint8_t id, uint32_t *size);
int             macro_store_read(uint8_t id, uint32_t off, uint8_t *buf,
                                 uint16_t len, uint16_t *got);
int             macro_store_erase(uint8_t id);           /* task context */

/* Staging (RAM): sequential/partial chunked writes for one macro id */
int             macro_store_stage_write(uint8_t id, uint32_t off,
                                        const uint8_t *data, uint16_t len);
int             macro_store_stage_commit(void);          /* task context */
bool            macro_store_stage_busy(void);
uint8_t         macro_store_stage_id(void);

uint32_t        macro_store_free_bytes(void);            /* cached */
