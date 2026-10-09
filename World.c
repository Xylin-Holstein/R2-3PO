#define _POSIX_C_SOURCE 200809L
#include "World.h"
#include "r2_diary.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define WMAX PATH_MAX
#define WOUT 8192
static int in_room;

static char *dupstr(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
    return s;
}
static int make_dir(const char *p) {
    if (mkdir(p, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        return lstat(p, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}
static int money_name(const char *s) {
    if (!strcmp(s, "money")) return 1;
    size_t n = strlen(s);
    if (n < 8 || strncmp(s, "money(", 6) || s[n-1] != ')') return 0;
    for (size_t i=6; i+1<n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}
static int wallet_count(const char *p) {
    DIR *d = opendir(p); if (!d) return -1;
    int n=0; struct dirent *e;
    while ((e=readdir(d))) {
        if (!money_name(e->d_name)) continue;
        char f[WMAX]; struct stat st;
        if (snprintf(f,sizeof(f),"%s/%s",p,e->d_name) >= (int)sizeof(f)) continue;
        if (lstat(f,&st)==0 && S_ISREG(st.st_mode)) n++;
    }
    closedir(d); return n;
}
static int seed_wallet(const char *p) {
    char marker[WMAX];
    if (snprintf(marker,sizeof(marker),"%s/.initial_five_dollars_granted",p)>=(int)sizeof(marker)) return -1;
    struct stat st;
    if (lstat(marker,&st)==0) return 0;
    if (errno!=ENOENT) return -1;
    int n=wallet_count(p); if (n<0) return -1;
    unsigned int suffix=0;
    while (n<5) {
        char name[64], path[WMAX];
        if (!suffix) snprintf(name,sizeof(name),"money");
        else snprintf(name,sizeof(name),"money(%u)",suffix);
        if (snprintf(path,sizeof(path),"%s/%s",p,name)>=(int)sizeof(path)) return -1;
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
        if (fd>=0) { close(fd); n++; }
        else if (errno!=EEXIST) return -1;
        suffix++;
    }
    int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0644);
    if (fd>=0) {
        const char *note="Initial five-dollar wallet grant completed. Never replenish on restart.\n";
        (void)write(fd,note,strlen(note)); close(fd); return 0;
    }
    return errno==EEXIST ? 0 : -1;
}
int r2_world_init(void) {
    char p[WMAX],w[WMAX],r[WMAX],s[WMAX],b[WMAX];
    if (snprintf(p,sizeof(p),"%s/Pockets",R2_ROOT)>=(int)sizeof(p) ||
        snprintf(w,sizeof(w),"%s/Pockets/Wallet",R2_ROOT)>=(int)sizeof(w) ||
        snprintf(r,sizeof(r),"%s/Room",R2_ROOT)>=(int)sizeof(r) ||
        snprintf(s,sizeof(s),"%s/Room/shelf",R2_ROOT)>=(int)sizeof(s) ||
        snprintf(b,sizeof(b),"%s/Room/box",R2_ROOT)>=(int)sizeof(b)) return -1;
    if (make_dir(p)||make_dir(w)||make_dir(r)||make_dir(s)||make_dir(b)) return -1;
    if (seed_wallet(w)) return -1;
    in_room=0; return 0;
}

/* Only relative paths inside Pockets/ or Room/ are accepted. */
static int safe_path(const char *rel, char *abs, size_t cap) {
    if (!rel||!*rel||rel[0]=='/'||strchr(rel,'\\')) return 0;
    const char *p=rel;
    while (*p) {
        const char *start=p; while (*p&&*p!='/') p++;
        size_t n=(size_t)(p-start);
        if (!n||(n==1&&start[0]=='.')||(n==2&&start[0]=='.'&&start[1]=='.')) return 0;
        if (*p=='/') p++;
    }
    int pocket=!strncmp(rel,"Pockets",7)&&(rel[7]=='\0'||rel[7]=='/');
    int room=!strncmp(rel,"Room",4)&&(rel[4]=='\0'||rel[4]=='/');
    if (!pocket&&!room) return 0;
    if (snprintf(abs,cap,"%s/%s",R2_ROOT,rel)>=(int)cap) return 0;
    char walk[WMAX]; if (snprintf(walk,sizeof(walk),"%s",abs)>=(int)sizeof(walk)) return 0;
    char *cur=walk+strlen(R2_ROOT); while (*cur=='/') cur++;
    while (*cur) {
        char *slash=strchr(cur,'/'); if (slash) *slash='\0';
        struct stat st; if (lstat(walk,&st)==0&&S_ISLNK(st.st_mode)) return 0;
        if (!slash) break; *slash='/'; cur=slash+1;
    }
    return 1;
}
static int room_path(const char *p) { return !strncmp(p,"Room",4)&&(p[4]=='\0'||p[4]=='/'); }
static int accessible(const char *p) { return !room_path(p)||in_room; }
static int matches_item(const char *file,const char *item) {
    size_t a=strlen(item), n=strlen(file);
    if (n==a&&!strcmp(file,item)) return 1;
    if (n<=a+2||strncmp(file,item,a)||file[a]!='('||file[n-1]!=')') return 0;
    for (size_t i=a+1;i+1<n;i++) if (!isdigit((unsigned char)file[i])) return 0;
    return 1;
}
static void list_dir(const char *rel,const char *abs,char *out,size_t cap) {
    DIR *d=opendir(abs); if (!d) { snprintf(out,cap,"Could not inspect %s: %s",rel,strerror(errno)); return; }
    size_t used=(size_t)snprintf(out,cap,"Contents of %s:\n",rel); int visible=0; struct dirent *e;
    while ((e=readdir(d))&&used+128<cap) {
        if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||e->d_name[0]=='.') continue;
        char f[WMAX]; struct stat st;
        if (snprintf(f,sizeof(f),"%s/%s",abs,e->d_name)>=(int)sizeof(f)||lstat(f,&st)||S_ISLNK(st.st_mode)) continue;
        int k=snprintf(out+used,cap-used,"- %s%s\n",e->d_name,S_ISDIR(st.st_mode)?"/":"");
        if (k>0) used+=(size_t)k; visible++;
    }
    closedir(d); if (!visible&&used+16<cap) snprintf(out+used,cap-used,"(empty)\n");
}
static int count_tree(const char *abs,const char *item) {
    DIR *d=opendir(abs); if (!d) return 0;
    int n=0; struct dirent *e;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||e->d_name[0]=='.') continue;
        char f[WMAX]; struct stat st;
        if (snprintf(f,sizeof(f),"%s/%s",abs,e->d_name)>=(int)sizeof(f)||lstat(f,&st)||S_ISLNK(st.st_mode)) continue;
        if (S_ISREG(st.st_mode)&&matches_item(e->d_name,item)) n++;
        else if (S_ISDIR(st.st_mode)) n+=count_tree(f,item);
    }
    closedir(d); return n;
}
static char *look(const char *where) {
    char rel[WMAX];
    if (!where||!*where||!strcasecmp(where,"here")) snprintf(rel,sizeof(rel),"%s",in_room?"Room":"Pockets");
    else if (!strcasecmp(where,"pockets")) snprintf(rel,sizeof(rel),"Pockets");
    else if (!strcasecmp(where,"wallet")) snprintf(rel,sizeof(rel),"Pockets/Wallet");
    else if (!strcasecmp(where,"room")) snprintf(rel,sizeof(rel),"Room");
    else if (!strcasecmp(where,"shelf")) snprintf(rel,sizeof(rel),"Room/shelf");
    else if (!strcasecmp(where,"box")) snprintf(rel,sizeof(rel),"Room/box");
    else snprintf(rel,sizeof(rel),"%s",where);
    char abs[WMAX]; if (!safe_path(rel,abs,sizeof(abs))) return dupstr("WORLD ERROR: Invalid location path.");
    if (!accessible(rel)) return dupstr("WORLD ACCESS: Enter your room before inspecting its contents.");
    struct stat st; if (lstat(abs,&st)||!S_ISDIR(st.st_mode)) return dupstr("WORLD ERROR: Location is missing or is not a directory.");
    char *out=calloc(WOUT,1); if (out) list_dir(rel,abs,out,WOUT); return out;
}
static char *wallet(void) {
    char p[WMAX]; if (snprintf(p,sizeof(p),"%s/Pockets/Wallet",R2_ROOT)>=(int)sizeof(p)) return dupstr("WORLD ERROR: Wallet path too long.");
    int n=wallet_count(p); if (n<0) return dupstr("WORLD ERROR: Could not inspect wallet.");
    char out[160]; snprintf(out,sizeof(out),"Wallet balance: $%d. Each valid money file represents one dollar.",n); return dupstr(out);
}
static char *move_item(char *args) {
    char *arrow=strstr(args,"->"); if (!arrow) return dupstr("WORLD ERROR: Use move <source> -> <destination folder>.");
    *arrow='\0'; char *sr=trim(args), *dr=trim(arrow+2); char src[WMAX],dst[WMAX],target[WMAX];
    if (!safe_path(sr,src,sizeof(src))||!safe_path(dr,dst,sizeof(dst))) return dupstr("WORLD ERROR: Paths must stay inside Pockets/ or Room/; '..' and absolute paths are forbidden.");
    if (!accessible(sr)||!accessible(dr)) return dupstr("WORLD ACCESS: Enter your room before moving things to or from it.");
    struct stat ss,ds;
    if (lstat(src,&ss)||!S_ISREG(ss.st_mode)||S_ISLNK(ss.st_mode)) return dupstr("WORLD ERROR: Source must be an existing regular object file.");
    if (lstat(dst,&ds)||!S_ISDIR(ds.st_mode)) return dupstr("WORLD ERROR: Destination must be an existing folder.");
    const char *base=strrchr(sr,'/'); base=base?base+1:sr;
    if (snprintf(target,sizeof(target),"%s/%s",dst,base)>=(int)sizeof(target)) return dupstr("WORLD ERROR: Destination path too long.");
    if (lstat(target,&ds)==0) return dupstr("WORLD ERROR: An item with that name already exists; nothing was moved.");
    if (errno!=ENOENT) return dupstr("WORLD ERROR: Could not verify destination.");
    if (rename(src,target)) { char out[512]; snprintf(out,sizeof(out),"WORLD ERROR: Move failed: %s",strerror(errno)); return dupstr(out); }
    char out[WMAX+128]; snprintf(out,sizeof(out),"Moved %s to %s. The filesystem now records its new location.",sr,target+strlen(R2_ROOT)+1); return dupstr(out);
}
char *r2_world_tool(const char *request) {
    if (!request) return dupstr("WORLD ERROR: Empty request.");
    char *copy=dupstr(request); if (!copy) return NULL; char *cmd=trim(copy), *result=NULL;
    if (!strncasecmp(cmd,"enter room",10)&&(cmd[10]=='\0'||isspace((unsigned char)cmd[10]))) {
        in_room=1; result=dupstr("You entered your room. Room, shelf, and box are now accessible; pockets remain accessible.");
    } else if (!strncasecmp(cmd,"leave room",10)&&(cmd[10]=='\0'||isspace((unsigned char)cmd[10]))) {
        in_room=0; result=dupstr("You left your room. Stored possessions remain there but are no longer directly accessible.");
    } else if (!strcasecmp(cmd,"location")||!strcasecmp(cmd,"status")) {
        result=dupstr(in_room?"Current location: Room. Pockets and room storage are accessible.":"Current location: Home, outside your room. Pockets are accessible; enter the room to access stored items.");
    } else if (!strncasecmp(cmd,"look",4)&&(cmd[4]=='\0'||isspace((unsigned char)cmd[4]))) result=look(trim(cmd+4));
    else if (!strncasecmp(cmd,"inspect ",8)) result=look(trim(cmd+8));
    else if (!strcasecmp(cmd,"wallet")||!strcasecmp(cmd,"count money")) result=wallet();
    else if (!strncasecmp(cmd,"count ",6)) {
        char *item=trim(cmd+6);
        if (!*item||strchr(item,'/')||strchr(item,'\\')||strchr(item,' ')) result=dupstr("WORLD ERROR: Count one item name at a time.");
        else {
            char p[WMAX],r[WMAX]; snprintf(p,sizeof(p),"%s/Pockets",R2_ROOT); int n=count_tree(p,item);
            if (in_room) { snprintf(r,sizeof(r),"%s/Room",R2_ROOT); n+=count_tree(r,item); }
            char out[512]; snprintf(out,sizeof(out),"I can currently account for %d item(s) named '%s' in accessible locations.",n,item); result=dupstr(out);
        }
    } else if (!strncasecmp(cmd,"move ",5)) result=move_item(trim(cmd+5));
    else result=dupstr("WORLD ERROR: Actions: location, enter room, leave room, look <here|pockets|wallet|room|shelf|box>, wallet, count <item>, move <source> -> <destination folder>.");
    free(copy); return result;
}
