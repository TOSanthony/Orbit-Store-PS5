/* Opt-in, short-lived PS5 benchmark. Never installed by Orbit or release builds. */
#include "transfer_benchmark.h"
#include "benchmark-secret.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <microhttpd.h>
#include <openssl/crypto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy, pending;
static unsigned runs;
static cJSON *results;
static struct {
    Release *release;
    unsigned count, duration;
    int64_t bytes;
    long socket_buffer, curl_buffer;
} task;
static volatile sig_atomic_t interrupted;
static void stop_signal(int value) { (void)value; interrupted = 1; }
void orbit_request_stop(void) { atomic_store(&orbit.stop, true); }
static enum MHD_Result reply(struct MHD_Connection *c, unsigned status, cJSON *value) {
    char *body = cJSON_PrintUnformatted(value);
    cJSON_Delete(value);
    if (!body) return MHD_NO;
    struct MHD_Response *r = MHD_create_response_from_buffer(strlen(body), body, MHD_RESPMEM_MUST_FREE);
    if (!r) { free(body); return MHD_NO; }
    MHD_add_response_header(r, "Content-Type", "application/json");
    MHD_add_response_header(r, "Cache-Control", "no-store");
    enum MHD_Result rc = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return rc;
}
static enum MHD_Result error(struct MHD_Connection *c, unsigned code, const char *message) {
    cJSON *v=cJSON_CreateObject(); cJSON_AddStringToObject(v,"error",message);
    return reply(c,code,v);
}
typedef struct { char body[1025]; size_t used; } Request;
static void completed(void *cls, struct MHD_Connection *c, void **context,
                      enum MHD_RequestTerminationCode why) {
    (void)cls; (void)c; (void)why; free(*context); *context=NULL;
}
static enum MHD_Result handle(void *cls, struct MHD_Connection *c, const char *url,
                             const char *method, const char *version, const char *data,
                             size_t *size, void **context) {
    (void)cls; (void)version;
    const char *auth=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Authorization");
    if (!auth || strncmp(auth,"Bearer ",7) || strlen(auth+7)!=64 ||
        CRYPTO_memcmp(auth+7,ORBIT_BENCHMARK_SECRET,64)) return error(c,401,"Not authorized.");
    if (MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Origin"))
        return error(c,403,"This developer endpoint is not available to web pages.");
    if (!*context) { *context=calloc(1,sizeof(Request)); return *context ? MHD_YES : MHD_NO; }
    Request *req=*context;
    if (*size) {
        if (*size>sizeof(req->body)-1-req->used) return error(c,413,"Request too large.");
        memcpy(req->body+req->used,data,*size); req->used+=*size; *size=0;
        return MHD_YES;
    }
    if (!strcmp(method,"GET") && !strcmp(url,"/status")) {
        cJSON *v=cJSON_CreateObject();
        pthread_mutex_lock(&lock);
        cJSON_AddStringToObject(v,"name","Orbit isolated transfer benchmark");
        cJSON_AddBoolToObject(v,"busy",busy);
        cJSON_AddNumberToObject(v,"runs",runs);
        cJSON_AddItemToObject(v,"results",cJSON_Duplicate(results,true));
        pthread_mutex_unlock(&lock);
        return reply(c,200,v);
    }
    if (strcmp(method,"POST")) return error(c,405,"POST required.");
    if (!strcmp(url,"/stop")) {
        orbit_request_stop();
        return reply(c,200,cJSON_CreateTrue());
    }
    if (strcmp(url,"/run")) return error(c,404,"Not found.");
    cJSON *input=cJSON_Parse(req->body);
    Release *r=release_find(json_text(input,"releaseId"));
    int64_t count=json_int(input,"connections"), bytes=json_int(input,"bytes"),
            duration=json_int(input,"seconds"), sock=json_int(input,"socketBuffer"),
            buf=json_int(input,"curlBuffer");
    bool valid=r && provider_supported(r) && (count==2 || count==4 || count==8 || count==16) &&
        bytes>=1024*1024 && bytes<=512LL*1024*1024 && bytes<r->size && duration>=1 && duration<=90 &&
        (sock==0 || sock==256*1024 || sock==1024*1024) && (buf==0 || buf==256*1024);
    cJSON_Delete(input);
    if (!valid) return error(c,400,"Only curated files and bounded benchmark settings are accepted.");
    pthread_mutex_lock(&lock);
    if (busy || runs>=24 || atomic_load(&orbit.stop)) {
        pthread_mutex_unlock(&lock); return error(c,409,"Busy, stopping or test budget exhausted.");
    }
    task.release=r; task.count=(unsigned)count; task.bytes=bytes; task.duration=(unsigned)duration;
    task.socket_buffer=(long)sock; task.curl_buffer=(long)buf;
    pending=busy=true; runs++;
    pthread_mutex_unlock(&lock);
    return reply(c,202,cJSON_CreateTrue());
}
static void *worker(void *unused) {
    (void)unused;
    while (!atomic_load(&orbit.stop)) {
        pthread_mutex_lock(&lock);
        bool run=pending;
        pending=false;
        pthread_mutex_unlock(&lock);
        if (!run) { usleep(100000); continue; }
        Storage drives[ORBIT_MAX_STORAGE], *storage=NULL;
        size_t n=storage_list(drives);
        for(size_t i=0;i<n;i++)
            if (!strcmp(drives[i].id,orbit.desktop ? "desktop" : "internal")) storage=&drives[i];
        cJSON *result=NULL;
        if (storage && storage->free_bytes > (uint64_t)task.bytes + 64*1024*1024) {
            Job *j=&orbit.jobs[0]; memset(j,0,sizeof *j);
            orbit.job_count=1; j->release=*task.release;
            copy_text(j->id,sizeof j->id,"bounded-speed-sample");
            copy_text(j->release_id,sizeof j->release_id,j->release.id);
            copy_text(j->filename,sizeof j->filename,"sample.bin");
            copy_text(j->root,sizeof j->root,storage->root);
            copy_text(j->storage_id,sizeof j->storage_id,storage->id);
            copy_text(j->status,sizeof j->status,"downloading");
            j->device=storage->device; j->inode=storage->inode;
            int fd=openat(orbit.state_fd,"sample.bin",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
            if (fd>=0) {
                /* Unlink before networking: even process failure releases the bytes.
                 * Keep the fd open for real pwrite/fsync on the destination disk. */
                bool removed=unlinkat(orbit.state_fd,"sample.bin",0)==0;
                if (removed) result=transfer_benchmark(j,fd,task.count,task.bytes,task.duration,
                                                      task.socket_buffer,task.curl_buffer);
                close(fd);
                if (result) cJSON_AddBoolToObject(result,"temporaryFileRemoved",removed);
            }
        }
        if (!result) { result=cJSON_CreateObject(); cJSON_AddStringToObject(result,"error","Safe temporary storage unavailable."); }
        cJSON_AddStringToObject(result,"releaseId",task.release->id);
        cJSON_AddStringToObject(result,"provider",task.release->source);
        pthread_mutex_lock(&lock);
        cJSON_AddItemToArray(results,result); busy=false;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
int main(int argc,char **argv) {
    signal(SIGINT,stop_signal); signal(SIGTERM,stop_signal); signal(SIGPIPE,SIG_IGN);
    copy_text(orbit.state_dir,sizeof orbit.state_dir,"/data/orbit-speedtest-" ORBIT_BENCHMARK_ID);
    int port=34179;
#ifdef ORBIT_TEST
    if (argc!=4) return 1;
    orbit.desktop=true;
    copy_text(orbit.state_dir,sizeof orbit.state_dir,argv[1]);
    copy_text(orbit.desktop_storage,sizeof orbit.desktop_storage,argv[2]);
    port=atoi(argv[3]);
#else
    (void)argc; (void)argv;
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) || catalog_init()) return 1;
#ifdef ORBIT_TEST
    const char *url=getenv("ORBIT_TEST_URL"), *size=getenv("ORBIT_TEST_SIZE");
    if (!url || strncmp(url,"http://127.0.0.1:",17) || !size) return 1;
    copy_text(orbit.releases[0].url,sizeof orbit.releases[0].url,url);
    orbit.releases[0].size=strtoll(size,NULL,10);
#endif
    /* An exclusive new directory also refuses a duplicate launch of this build. */
    if (mkdir(orbit.state_dir,0700)) return 1;
    orbit.state_fd=open(orbit.state_dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if (orbit.state_fd<0) return 1;
    orbit.sources=(SourceSettings){ORBIT_SOURCE_ARCHIVE|ORBIT_SOURCE_VIKINGFILE,1,time(NULL)};
    results=cJSON_CreateArray();
    struct MHD_Daemon *server=MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,port,NULL,NULL,handle,NULL,
        MHD_OPTION_CONNECTION_LIMIT,4U,MHD_OPTION_CONNECTION_TIMEOUT,5U,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT,(size_t)8192,
        MHD_OPTION_THREAD_STACK_SIZE,(size_t)ORBIT_THREAD_STACK,
        MHD_OPTION_NOTIFY_COMPLETED,completed,NULL,MHD_OPTION_END);
    pthread_t thread; pthread_attr_t attr;
    bool launched=false;
    if (server && !pthread_attr_init(&attr)) {
        if (!pthread_attr_setstacksize(&attr,ORBIT_THREAD_STACK) && !pthread_create(&thread,&attr,worker,NULL)) launched=true;
        pthread_attr_destroy(&attr);
    }
    if (launched) {
        for(unsigned seconds=0;seconds<1200 && !interrupted && !atomic_load(&orbit.stop);seconds++) sleep(1);
        atomic_store(&orbit.stop,true); pthread_join(thread,NULL);
    }
    if(server) MHD_stop_daemon(server);
    /* Only this fresh directory's known checkpoint files, never the app's state. */
    unlinkat(orbit.state_fd,"state.tmp",0); unlinkat(orbit.state_fd,"state.json",0);
    close(orbit.state_fd); rmdir(orbit.state_dir);
    cJSON_Delete(results); cJSON_Delete(orbit.catalog); curl_global_cleanup();
    return launched ? 0 : 1;
}
