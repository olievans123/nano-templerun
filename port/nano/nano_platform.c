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
void port_crumb(const char *tag,uint32_t a,uint32_t b){(void)tag;(void)a;(void)b;}

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

/* Each mode runs for PHASE_US, then one report is written and the full scene carries on. */
#define PHASE_US 6000000u
static struct { uint32_t frames,max_period,work_max,over33;uint64_t period,work; } phase[3];

void tr_nano_frame(int w,int h,uint32_t frame) {
    static uint64_t last,start;static uint32_t previous_frame;static int reported;
    if(w<1||h<1||failed)return;
    if(initialized && frame<previous_frame){initialized=0;last=0;}       /* new GL view: textures are gone */
    previous_frame=frame;
    if(!initialized) {
        uint64_t t0=plat_time_us();
        plat_log("Temple Run hardware test: panel %dx%d, heap free %u, largest %u, redraw %s",w,h,hb_os_heap_free(),
                 hb_os_heap_largest(),tr_fast_redraw?"2 ms heartbeat":"16 ms heartbeat");
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
    uint32_t period=last?(uint32_t)(now-last):0;last=now;
    int mode=0;
    if(!reported && now>=start) {
        uint32_t elapsed=(uint32_t)(now-start);
        mode=(int)(elapsed/PHASE_US);
        if(mode>2) {
            reported=1;mode=0;
            static const char *names[3]={"track+lightmap+characters","no lightmap","track only, no lightmap"};
            for(int i=0;i<3;i++) {
                uint32_t n=phase[i].frames?phase[i].frames:1;
                uint32_t fps10=phase[i].period?(uint32_t)((uint64_t)phase[i].frames*10000000u/phase[i].period):0;
                plat_log("%s: frames=%u fps=%u.%u work_us=%u work_max=%u period_max=%u over_33ms=%u",names[i],
                         phase[i].frames,fps10/10,fps10%10,(unsigned)(phase[i].work/n),phase[i].work_max,
                         phase[i].max_period,phase[i].over33);
            }
            plat_log("per frame: %d draws, %d vertices, %d triangles",scene_stat_draws,scene_stat_vertices,scene_stat_triangles);
            port_log_flush(DATA_DIR "/log.txt");
            last=0;
        }
    }
    float dt=period?(float)period*1e-6f:1.f/30.f;
    if(dt>0.1f)dt=0.1f;
    scene_frame(w,h,dt,mode);
    if(!reported && now>=start && period) {
        uint32_t work=(uint32_t)(plat_time_us()-now);
        phase[mode].frames++;phase[mode].period+=period;phase[mode].work+=work;
        if(period>phase[mode].max_period)phase[mode].max_period=period;
        if(work>phase[mode].work_max)phase[mode].work_max=work;
        phase[mode].over33+=period>33333u;
    }
}
