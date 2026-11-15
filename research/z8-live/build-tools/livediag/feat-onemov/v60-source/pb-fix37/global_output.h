#ifndef PB_GLOBAL_OUTPUT_H
#define PB_GLOBAL_OUTPUT_H
/* Nikon GUI surface tuples are (group, output, target). The descriptor/VG
 * selector accepts that order; backing/enable APIs accept (output,group,target).
 * 40398570 follows Nikon's current LCD/EVF selection (1/4). */
#ifndef PB_OUTPUT_NATIVE
#define PB_OUTPUT_NATIVE(a,t) ((t)(uintptr_t)UINT64_C(a))
#endif
static uint32_t pb_output_current(void) {
    return PB_OUTPUT_NATIVE(0x40398570,uint32_t (*)(void))();
}
uint32_t pb_output_valid(const void *surface) {
    uint32_t output=((const uint32_t *)((const uint8_t *)surface+0x108u))[1];
    return (output==1u || output==4u) && output==pb_output_current();
}
uint32_t pb_output_backing_rectangle(uint32_t group,uint32_t output,uint32_t target,void *rect) {
    if((output!=1u && output!=4u) || output!=pb_output_current())return 0;
    return PB_OUTPUT_NATIVE(0x40340d64,uint32_t (*)(uint32_t,uint32_t,uint32_t,void *))
        (output,group,target,rect);
}
uint32_t pb_output_target_enabled(uint32_t group,uint32_t output,uint32_t target) {
    return PB_OUTPUT_NATIVE(0x403409d4,uint32_t (*)(uint32_t,uint32_t,uint32_t))(output,group,target);
}
void pb_output_target_enable(uint32_t group,uint32_t output,uint32_t target,uint32_t enabled) {
    PB_OUTPUT_NATIVE(0x403409a4,void (*)(uint32_t,uint32_t,uint32_t,uint32_t))(output,group,target,enabled);
}
/* Called under the existing native GUI mutex, before attaching a child.
 * A stock-owned root is never reconstructed or rebound. A Fenestra-created
 * root has id7fdf and both pinned draw callbacks; only an empty one may move.
 * Existing list storage is retained, including its native allocation cache. */
FxResult pb_output_initialize_global(void *surface) {
    uint8_t *bytes=surface;uint32_t nonzero=0;
    for(uint32_t i=0;i<0x118u;i++)nonzero|=bytes[i];
    if(nonzero && *(uint32_t *)(void *)bytes!=0x7fdfu)return FX_OK;
    if(nonzero && (*(uintptr_t *)(void *)(bytes+0x48u)!=UINT64_C(0x40325a80) ||
        *(uintptr_t *)(void *)(bytes+0xb0u)!=UINT64_C(0x4032987c) ||
        *(uintptr_t *)(void *)(bytes+0x40u)))return FX_CAPABILITY;
    uint32_t group=PB_OUTPUT_NATIVE(0x403f1b18,uint32_t (*)(void))();
    uint32_t output=pb_output_current();
    uint32_t target=PB_OUTPUT_NATIVE(0x403f0db8,uint32_t (*)(uint32_t))(4u);
    uint16_t viewport[4];
    if(!(pb_output_backing_rectangle(group,output,target,viewport)&1u) ||
       viewport[0]>=viewport[2] || viewport[1]>=viewport[3] ||
       viewport[2]>4096u || viewport[3]>4096u)return FX_CAPABILITY;
    uint32_t *tuple=(uint32_t *)(void *)(bytes+0x108u);
    uint16_t bounds[4]={0,0,viewport[2],viewport[3]};
    if(nonzero) {
        const uint16_t *old=(const uint16_t *)(const void *)(bytes+0x10u);
        if(tuple[0]==group && tuple[1]==output && tuple[2]==target &&
           old[0]==0 && old[1]==0 && old[2]==bounds[2] && old[3]==bounds[3])return FX_OK;
        if(PB_OUTPUT_NATIVE(0x40c7a304,uint32_t (*)(void *))(bytes+0xc8u))return FX_BUSY;
        /* Native bounds setter updates the public and cached rectangles and
         * dirties this root. No list reset, allocation or foreign-child change. */
        PB_OUTPUT_NATIVE(0x40325e0c,void (*)(void *,const void *))(surface,bounds);
        tuple[0]=group;tuple[1]=output;tuple[2]=target;
    } else {
        _Alignas(8) uint8_t descriptor[0x80]={0};
        *(uint32_t *)(void *)descriptor=0x7fdfu;
        uint16_t *r=(uint16_t *)(void *)(descriptor+0x10u);
        r[2]=bounds[2];r[3]=bounds[3];descriptor[0x20u]=1u;
        uint32_t *binding=(uint32_t *)(void *)(descriptor+0x70u);
        binding[0]=group;binding[1]=output;binding[2]=target;
        PB_OUTPUT_NATIVE(0x403297e4,void (*)(void *,const void *))(surface,descriptor);
    }
    return FX_OK;
}
#endif
