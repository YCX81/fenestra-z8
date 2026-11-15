#include "queue_prototype.h"
#include <assert.h>
#include <stdio.h>
static unsigned paused,starts,resumes,stops;static int ack,fail_start;static uint64_t token;
static int pause_(void*p){(void)p;paused++;return !ack;}
static int start_(void*p,const struct sq_request*j){(void)p;starts++;token=j->ticket;assert(j->tone==1||j->tone==2);return !fail_start;}
static int cancel_(void*p){(void)p;stops++;return 1;}
static int resume_(void*p){(void)p;resumes++;return 0;}
static void init(struct sq*q){struct sq_ops o={0,pause_,start_,cancel_,resume_};sq_init(q,&o);paused=starts=resumes=stops=ack=fail_start=0;token=0;}
int main(void){struct sq q;
 init(&q);assert(!sq_submit(&q,0,0,0,0,1)&&!sq_submit(&q,12,1,0,0,1));assert(!sq_submit(&q,1,1,0,0,0));sq_step(&q,0);assert(!paused&&!starts); /* mute */
 assert(sq_submit(&q,1,1,0,0,1));assert(sq_submit(&q,2,1,0,1,1));assert(!sq_submit(&q,1,1,0,0,1)&&q.overflow==1);
 sq_step(&q,0);assert(q.state==SQ_PAUSE&&!starts);ack=1;sq_step(&q,1000);assert(q.state==SQ_PLAY&&starts==1);
 sq_done(&q,token+9);sq_step(&q,2000);assert(q.state==SQ_PLAY&&!resumes);sq_done(&q,token);sq_step(&q,3000);assert(q.state==SQ_PAUSE&&!resumes);
 uint64_t old=token;sq_step(&q,4000);assert(starts==2&&q.state==SQ_PLAY);sq_done(&q,old);sq_step(&q,5000);assert(q.state==SQ_PLAY&&!resumes);
 sq_done(&q,token);sq_step(&q,6000);assert(q.state==SQ_IDLE&&resumes==1);
 init(&q);assert(sq_submit(&q,1,1,0,0,1));sq_step(&q,0);sq_cancel(&q);ack=1;sq_step(&q,1000);assert(!starts&&!resumes&&q.state==SQ_IDLE&&!sq_submit(&q,1,1,0,0,1));
 init(&q);ack=1;assert(sq_submit(&q,1,1,0,0,1));sq_step(&q,0);sq_cancel(&q);sq_step(&q,1000);sq_step(&q,2000);assert(stops==1&&!resumes&&q.state==SQ_PLAY);sq_done(&q,token);sq_step(&q,3000);assert(!resumes&&q.state==SQ_IDLE);
 init(&q);ack=1;assert(sq_submit(&q,1,1,0,0,1));sq_step(&q,0);sq_step(&q,2000001);assert(q.state==SQ_RETAINED&&!resumes);sq_done(&q,token);sq_step(&q,2000002);assert(q.state==SQ_RETAINED&&!resumes);
 init(&q);ack=1;fail_start=1;assert(sq_submit(&q,1,1,0,0,1));sq_step(&q,0);assert(q.state==SQ_IDLE&&resumes==1&&!atomic_load(&q.active));
 puts("PASS private finite2 queue: muted/no-admission; ordered tones; overflow; stale completion; cancel before/after ownership; timeout retains; failed post resumes. Native hook/lifetime adapter remains UNIMPLEMENTED.");return 0;}
