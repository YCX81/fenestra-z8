/* The Z8 USB Linux bridge needs a mapped shared-memory window that is not qualified on Z8.
 * It is left out: the PTP capability is not advertised and these entries are inert. */
#include "fenestra_native_linux.h"
void fx_linux_account(FxLinuxBridge *b,FxAllocationObserver o){(void)b;(void)o;}
void fx_linux_account_packets(FxLinuxBridge *b,FxIndexMemory m,FxAllocationObserver r,FxAllocationObserver d){(void)b;(void)m;(void)r;(void)d;}
void fx_linux_tick(uint64_t session,uint32_t generation,int enabled){(void)session;(void)generation;(void)enabled;}
void fx_linux_revoke(void){}
int fx_linux_quiesced(void){return 1;}
uint32_t fx_linux_ready(void){return 0u;}
