static int ready_to_write(PbRuntime *r,uint32_t key) {
    uint32_t idle=0;
    if(LOAD(&r->ui_leaving)||!atomic_compare_exchange_strong_explicit(
       &r->render_request,&idle,4u,memory_order_acq_rel,memory_order_relaxed))return 0;
    uint64_t before;
    r->compose_epoch=pb_close_gate(r,&before);
    if(LOAD(&r->shutdown)||LOAD(&r->ui_leaving)||key!=LOAD(&r->key_epoch)) {
        STORE(&r->render_request,0);return 0;
    }
    if(atomic_load_explicit(&r->display.display.readers,memory_order_seq_cst)) {
        /* No bytes changed. Restore the old frame only if no cancellation
         * intervened; publish it under a new epoch, never by reusing a ticket. */
        uint64_t expected=r->compose_epoch;
        if(before&1u)atomic_compare_exchange_strong_explicit(&r->gate,&expected,
                     expected|1u,memory_order_seq_cst,memory_order_relaxed);
        STORE(&r->render_request,0);return 0;
    }
    return 1;
}
static int present(PbRuntime *r,uint32_t key) {
    uint32_t id=0;
    uint64_t expected=r->compose_epoch;
    if(LOAD(&r->ui_leaving)||LOAD(&r->shutdown)||key!=LOAD(&r->key_epoch)||
       !current(&id)||id!=r->current_id||!atomic_compare_exchange_strong_explicit(
       &r->gate,&expected,expected|1u,memory_order_seq_cst,memory_order_relaxed)) {
        STORE(&r->render_request,0);return 0;
    }
    STORE(&r->render_request,1);return 1;
}
