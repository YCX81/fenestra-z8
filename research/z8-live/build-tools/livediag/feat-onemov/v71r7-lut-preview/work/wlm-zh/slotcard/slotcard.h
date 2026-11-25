/* Picture Control extended slots on the memory card: storage prototype.
 * Emulation/host only, never flashed. No IO here: the caller performs each ScIo
 * and passes the reply back, so the host test and the Fenestra module run the
 * same code. Independent of the GPL slots tree: no code copied from it. */
#ifndef SLOTCARD_H
#define SLOTCARD_H
#include <stdint.h>
#define SC_SLOTS 128u            /* 2 banks x 64, same namespace as the C64 layout */
#define SC_RECORD 1024u          /* one slot record, same stride as the existing layout */
#define SC_WIRE 688u             /* FX_SERVICE_BYTES 768 - sizeof(FxFileRequest) 80 */
#define SC_HEADER 592u
#define SC_ENTRY (8u+SC_RECORD)
#define SC_FOOTER 24u
#define SC_FILE_RECORDS 32u
#define SC_FILE_MAX (SC_HEADER+SC_FILE_RECORDS*SC_ENTRY+SC_FOOTER)
#define SC_WALKBACK 64u
#define SC_MAX_GEN 0x7fffffffu
#define SC_NO_SELECTOR 0u

enum {SC_IO_OPEN_READ=1,SC_IO_CREATE,SC_IO_READ,SC_IO_WRITE,SC_IO_CLOSE,SC_IO_SCAN,SC_IO_DELETE};
enum {SC_R_OK,SC_R_MISSING,SC_R_FAILED};     /* MISSING: native 0x80000010, FX_EMPTY */
typedef struct {uint32_t op,gen,bytes;uint8_t *read;const uint8_t *write;} ScIo;
typedef struct {uint32_t result,done,value;} ScReply; /* value: highest gen for SC_IO_SCAN */

enum {SC_RUN,SC_DONE};
enum {SC_OK,SC_EMPTY_STORE,SC_NO_MEDIA,SC_CORRUPT_ALL,SC_WRITE_FAILED,SC_VERIFY_FAILED,
      SC_SAME_CARD,SC_OTHER_CARD,SC_BAD_ARGUMENT};

typedef struct {
    /* persistent view */
    uint64_t store_id;
    uint32_t head,next_gen;
    uint32_t table[SC_SLOTS];       /* generation holding each slot; 0 = empty */
    uint32_t selection[2],proxy[2]; /* wanted selector; stock-legal value kept in settings */
    uint32_t limit,next_limit;      /* slot limit chosen in the menu, 0 = not set */
    uint8_t used[SC_SLOTS],bad[SC_SLOTS];
    uint8_t records[SC_SLOTS][SC_RECORD];
    /* pending commit */
    uint8_t dirty[SC_SLOTS];
    uint32_t next_selection[2],next_proxy[2];
    /* statistics for the report */
    uint32_t files_skipped,bad_slots,last_gen_written,io_count,failed_gens;
    /* machine */
    uint32_t pc,pc2,status,mode,use_scan,gc_floor;
    uint32_t a,b,c,d,cand,tries,offset,total,count,v_ok,v_gen;
    uint64_t entropy;
    ScReply reply;
    uint32_t v_crc;                 /* running CRC32 during validate() */
    uint8_t file[SC_FILE_MAX];
} Sc;

void sc_init(Sc *,uint64_t entropy,uint32_t use_scan);
void sc_begin_load(Sc *);
/* Commit the dirty records and next_selection/next_proxy as one generation. */
void sc_begin_commit(Sc *);
/* Does the inserted card still hold this store? For hot change detection by polling. */
void sc_begin_check(Sc *);
/* Needs delete+scan services: removes generations nothing refers to. */
void sc_begin_gc(Sc *);
uint32_t sc_next(Sc *,const ScReply *,ScIo *); /* first call: reply ignored */
void sc_name(uint32_t gen,char out[16]);
uint32_t sc_crc32(uint32_t crc,const void *,uint32_t);
void sc_put_record(Sc *,uint32_t slot,const uint8_t *data);

/* Selection policy. Native get/set is abstract so the host test can model it. */
typedef struct {
    void *context;
    uint32_t (*get)(void *,uint32_t bank);
    int (*set)(void *,uint32_t bank,uint32_t selector);
} ScNative;
int sc_selector_extended(uint32_t selector,uint32_t *index);
uint32_t sc_selector_of(uint32_t index);
int sc_selector_legal(uint32_t bank,uint32_t selector);
/* Boot-to-mount window closed: apply the card's selection unless the user moved. */
uint32_t sc_apply_after_mount(const Sc *,const ScNative *);
/* A selection the user just made, recorded for the next commit. */
void sc_note_selection(Sc *,uint32_t bank,uint32_t selector);
/* Card left or a different card arrived. Returns slots dropped. */
uint32_t sc_card_changed(Sc *,const ScNative *);
/* Event 7: leave only stock-legal values in settings. Returns banks rewritten, or -1. */
int sc_restore_for_shutdown(const Sc *,const ScNative *);
#endif
