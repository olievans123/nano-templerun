/* NanoApps SDK adapter for the Temple Run hardware test: loads the original models and
 * textures, runs the test scene, and logs frame timings once. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_surface_input.h"
#include "../src/platform.h"
#include "../src/scene_test.h"
#include "../src/tex.h"

#define DATA_DIR "/Apps/Data/TempleRun"
#define MAX_FILES 256
extern void *memalign(size_t,size_t);
extern void port_log_flush(const char *path);
static struct { char name[48];uint32_t size; } files[MAX_FILES];
static int file_count,initialized,failed;
uint8_t tr_fast_redraw;          /* set by the entry: the resident honours a short heartbeat */
uint64_t plat_time_us(void) {
    static uint32_t last;static uint64_t high;
    uint32_t now=hb_time_uptime_us();if(now<last)high+=1ull<<32;last=now;return high|now;
}
/* Same aligned, oversized read pattern used by the tested nano game ports. */
static void *read_file(const char *path,uint32_t expected,uint32_t *size) {
    if(expected>4u*1024u*1024u)return NULL;
    uint32_t cap=((expected+1u+4095u)&~4095u)+4096u;
    unsigned char *data=memalign(64,cap);if(!data)return NULL;
    uint32_t got=hb_fs_read(path,data,cap);
    if(!got||got>expected){free(data);return NULL;}
    data[got]=0;if(size)*size=got;return data;
}
static int manifest(void) {
    uint32_t size;char *text=read_file(DATA_DIR "/files.lst",16384,&size),*p;
    if(!text)return -1;
    file_count=0;
    for(p=text;*p&&file_count<MAX_FILES;) {
        unsigned len=0;uint32_t n=0;
        while(*p&&*p!=' '&&*p!='\n'&&len<47)files[file_count].name[len++]=*p++;
        files[file_count].name[len]=0;
        while(*p==' ')p++;
        while(*p>='0'&&*p<='9'){n=n*10+(unsigned)(*p++-'0');if(n>4u*1024u*1024u){free(text);return -1;}}
        while(*p&&*p!='\n')p++;
        if(*p)p++;
        if(len&&n){files[file_count].size=n;file_count++;}
    }
    free(text);return file_count?0:-1;
}
void *plat_read_file(const char *name,uint32_t *size,int save) {
    char path[112];uint32_t expected=16;
    if(strstr(name,"..")||name[0]=='/')return NULL;
    if(!save) {
        int i;for(i=0;i<file_count;i++)if(!strcmp(files[i].name,name))break;
        if(i==file_count)return NULL;
        expected=files[i].size;
    }
    snprintf(path,sizeof path,DATA_DIR "/%s",name);
    return read_file(path,expected,size);
}
int plat_write_file(const char *name,const void *data,uint32_t size) {
    char path[112];if(!size||size>65536||strstr(name,"..")||name[0]=='/')return -1;
    uint32_t cap=(size+4095u)&~4095u;void *buf=memalign(64,cap);if(!buf)return -1;
    memset(buf,0,cap);memcpy(buf,data,size);snprintf(path,sizeof path,DATA_DIR "/%s",name);
    hb_fs_mkdir(DATA_DIR);int result=hb_fs_write(path,buf,size)?0:-1;free(buf);return result;
}
void plat_log(const char *format,...) {
    va_list args;va_start(args,format);vprintf(format,args);va_end(args);putchar('\n');
}
/* Breadcrumbs, as in the Angry Birds port: a small RAM ring tagged with a magic. RAM
 * survives a panic reboot, so the next launch finds the previous run's ring and saves it
 * as prevboot.txt. Never written to disk while running. */
#define CRUMB_MAGIC0 0x54525542u
#define CRUMB_MAGIC1 0x43524d42u
typedef struct { uint32_t magic0,magic1,len,seq;char text[8176]; } crumb_ring_t;
static crumb_ring_t *s_ring;
void port_crumb(const char *tag,uint32_t a,uint32_t b) {
    static const char hex[]="0123456789abcdef";
    static uint32_t s_t0;
    char line[40];int n=0,i;
    if(!s_t0)s_t0=hb_time_uptime_us();
    if(b==0)b=(hb_time_uptime_us()-s_t0)/1000u;   /* ms since the first crumb */
    while(*tag&&n<12)line[n++]=*tag++;
    line[n++]=' ';
    for(i=28;i>=0;i-=4)line[n++]=hex[(a>>i)&15];
    line[n++]=' ';
    for(i=28;i>=0;i-=4)line[n++]=hex[(b>>i)&15];
    line[n++]='\n';
    if(!s_ring) {
        s_ring=malloc(sizeof(crumb_ring_t));
        if(!s_ring)return;
        s_ring->magic0=CRUMB_MAGIC0;s_ring->magic1=CRUMB_MAGIC1;s_ring->len=0;s_ring->seq=hb_time_uptime_us();
    }
    if(s_ring->len>sizeof(s_ring->text))s_ring->len=0;
    if(s_ring->len+(uint32_t)n>sizeof(s_ring->text)) {
        uint32_t cut=s_ring->len/2;
        while(cut<s_ring->len&&s_ring->text[cut-1]!='\n')cut++;
        memmove(s_ring->text,s_ring->text+cut,s_ring->len-cut);s_ring->len-=cut;
    }
    for(i=0;i<n;i++)s_ring->text[s_ring->len++]=line[i];
    __asm__ volatile("dsb":::"memory");
}
/* Previous run's ring: scan the heap region for the magic (16-byte aligned blocks). */
static void save_previous_crumbs(void) {
    for(uint32_t a=0x08800000u;a<0x0B800000u;a+=16u) {
        crumb_ring_t *r=(crumb_ring_t *)(uintptr_t)a;
        if(r->magic0==CRUMB_MAGIC0&&r->magic1==CRUMB_MAGIC1&&r!=s_ring&&r->len<=sizeof(r->text)) {
            uint32_t cap=(r->len+4095u)&~4095u;char *buf=memalign(64,cap?cap:4096u);
            if(buf) {
                memcpy(buf,r->text,r->len);
                hb_fs_mkdir("/Apps/Data");hb_fs_mkdir(DATA_DIR);
                hb_fs_write(DATA_DIR "/prevboot.txt",buf,r->len);free(buf);
            }
            r->magic0=0;   /* consumed */
            return;
        }
    }
}

static void keep_awake(void) {          /* as the other nano game ports (firmware 39579dba addresses) */
    static uint64_t last;
    uint64_t now=plat_time_us();
    if(last && now-last<10000000ull)return;
    last=now;
    typedef void *(*instance_fn)(void);
    typedef void (*event_fn)(void *,int);
    void *manager=((instance_fn)(0x0842ae80u|1u))();
    if(manager)((event_fn)(0x084069d8u|1u))(manager,4);
}

/* Each mode runs for PHASE_US; a line is logged as each one ends (three writes in all),
 * then the full scene carries on. */
#define PHASE_US 3000000u
#define PHASES 4
/* the scene mode of each phase: with lightmap, without, with again, track only */
static const uint8_t phase_mode[PHASES]={0,1,0,2};
static struct { uint32_t frames,max_period,work_max,over33;uint64_t period,work; } phase[PHASES];

void tr_nano_frame(int w,int h,uint32_t frame) {
    static uint64_t last,start;static uint32_t previous_frame;static int reported;
    if(w<1||h<1||failed)return;
    if(initialized && frame<previous_frame){initialized=0;last=0;}       /* new GL view: textures are gone */
    previous_frame=frame;
    if(!initialized) {
        uint64_t t0=plat_time_us();
        plat_log("Temple Run hardware test: panel %dx%d, heap free %u, largest %u, redraw %s",w,h,hb_os_heap_free(),
                 hb_os_heap_largest(),tr_fast_redraw?"2 ms heartbeat":"16 ms heartbeat");
        save_previous_crumbs();
        port_crumb("init",(uint32_t)hb_os_heap_free(),0);
        if(manifest() || scene_init()) {
            plat_log("initialization failed");port_log_flush(DATA_DIR "/log.txt");failed=1;return;
        }
        initialized=1;
        plat_log("loaded in %u ms: heap free %u, largest %u, textures %u KiB",(unsigned)((plat_time_us()-t0)/1000u),
                 hb_os_heap_free(),hb_os_heap_largest(),tex_bytes/1024u);
        port_log_flush(DATA_DIR "/log.txt");
        start=0;last=0;
    }
    keep_awake();
    uint64_t now=plat_time_us();
    if(!start)start=now+500000u;                    /* let the first frames settle */
    uint32_t period=last?(uint32_t)(now-last):0;
    int index=0,mode=0;
    static int logged;
    static uint32_t count;
    if(!reported && now>=start) {
        uint32_t elapsed=(uint32_t)(now-start);
        index=(int)(elapsed/PHASE_US);
        while(logged<index && logged<PHASES) {
            int i=logged++;
            uint32_t n=phase[i].frames?phase[i].frames:1;
            uint32_t fps10=phase[i].period?(uint32_t)((uint64_t)phase[i].frames*10000000u/phase[i].period):0;
            port_crumb("log",(uint32_t)i,0);
            plat_log("phase %d mode %d: frames=%u fps=%u.%u work_us=%u work_max=%u period_max=%u over_33ms=%u; %d draws, %d vertices",
                     i,(int)phase_mode[i],phase[i].frames,fps10/10,fps10%10,(unsigned)(phase[i].work/n),phase[i].work_max,
                     phase[i].max_period,phase[i].over33,scene_stat_draws,scene_stat_vertices);
            port_crumb("flush",(uint32_t)i,0);
            port_log_flush(DATA_DIR "/log.txt");
            port_crumb("flushed",(uint32_t)i,0);
            last=0;
        }
        if(index>=PHASES){reported=1;index=0;}
        mode=phase_mode[index];
    }
    last=plat_time_us();now=last;        /* a log write above must not count as a frame */
    float dt=period?(float)period*1e-6f:1.f/30.f;
    if(dt>0.1f)dt=0.1f;
    port_crumb("draw",(count<<4)|(uint32_t)mode,0);
    scene_frame(w,h,dt,mode);
    port_crumb("drawn",count++,0);
    if(!reported && now>=start && period) {
        uint32_t work=(uint32_t)(plat_time_us()-now);
        phase[index].frames++;phase[index].period+=period;phase[index].work+=work;
        if(period>phase[index].max_period)phase[index].max_period=period;
        if(work>phase[index].work_max)phase[index].work_max=work;
        phase[index].over33+=period>33333u;
    }
}
