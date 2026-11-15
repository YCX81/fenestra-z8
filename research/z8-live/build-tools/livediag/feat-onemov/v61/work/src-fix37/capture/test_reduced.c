#define main old_capture_test_main
#include "test_capture.c"
#undef main
int main(void) {
 struct la_capture c;struct fixture f;setup(&c,&f);uint64_t t=100000;
 for(unsigned i=0;i<60;i++,t+=41667)tick(&c,t);
 assert(c.phase==LA_READY && c.count<=4 && c.copied>=5);
 assert(la_full_press(&c,t));uint64_t press=t;
 for(unsigned i=0;i<42;i++,t+=41667)tick(&c,t);
 assert(c.phase==LA_FROZEN && c.count>=6 && c.count<=8 && c.press_us==press);
 assert(la_begin_encode(&c));assert(la_frame_at(&c,0,0));la_finish_encode(&c);
 teardown(&c,&f);puts("reduced ring timed press/freeze passed");return 0;
}
