/* Every cancellation changes the generation, even when the producer already
 * closed the gate. Only the holder of that generation can publish a scene. */
static uint64_t pb_close_gate(PbRuntime *r,uint64_t *before) {
    uint64_t old=LOAD(&r->gate),next;
    do {next=(old&~UINT64_C(1))+2u;}
    while(!atomic_compare_exchange_weak_explicit(&r->gate,&old,next,memory_order_seq_cst,memory_order_relaxed));
    if(before)*before=old;
    return next;
}
static void pb_clear_pending(PbRuntime *r) {
    uint32_t pending=LOAD(&r->render_request);
    while((pending==1u||pending==2u)&&!atomic_compare_exchange_weak_explicit(
          &r->render_request,&pending,0u,memory_order_acq_rel,memory_order_relaxed)){}
}
