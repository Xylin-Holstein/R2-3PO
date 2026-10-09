#define _GNU_SOURCE
#include "Reality.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef R2_ROOT
#error R2_ROOT must be set to the isolated test directory
#endif
static int bill_count(const char *dir) {
    DIR *dp=opendir(dir); assert(dp);
    int count=0; struct dirent *e;
    while ((e=readdir(dp))) {
        if (!strcmp(e->d_name,"money")) { ++count; continue; }
        size_t n=strlen(e->d_name);
        if (n<8 || strncmp(e->d_name,"money(",6) || e->d_name[n-1]!=')') continue;
        int digits=1;
        for (size_t i=6;i+1<n;++i) if (e->d_name[i]<'0'||e->d_name[i]>'9') digits=0;
        if (digits) ++count;
    }
    closedir(dp); return count;
}
static int change_cents(const char *dir) {
    char path[1024]; snprintf(path,sizeof(path),"%s/change.txt",dir);
    FILE *fp=fopen(path,"r"); if(!fp)return 0;
    int n=0; assert(fscanf(fp,"cents=%d",&n)==1); fclose(fp); return n;
}
static char *money_context(void) {
    char *s=r2_reality_money_context(); assert(s); return s;
}
int main(void) {
    char wallet[1024],bank[1024],bill[1200];
    snprintf(wallet,sizeof(wallet),"%s/Pockets/Wallet",R2_ROOT);
    snprintf(bank,sizeof(bank),"%s/room/piggybank",R2_ROOT);
    assert(mkdir(R2_ROOT,0755)==0);
    char room_root[1200]; snprintf(room_root,sizeof(room_root),"%s/room",R2_ROOT);
    assert(mkdir(room_root,0755)==0);
    /* Upgrade old lowercase and room-local mirror folders without losing portable items. */
    char legacy[1400], legacy_wallet[1500], legacy_room_pockets[1500], legacy_room_wallet[1500], migrated[1500];
    snprintf(legacy_room_pockets,sizeof(legacy_room_pockets),"%s/room/pockets",R2_ROOT);
    snprintf(legacy_room_wallet,sizeof(legacy_room_wallet),"%s/room/wallet",R2_ROOT);
    assert(mkdir(legacy_room_pockets,0755)==0);
    assert(mkdir(legacy_room_wallet,0755)==0);
    snprintf(migrated,sizeof(migrated),"%s/legacy_room_item.r2item",legacy_room_pockets);
    FILE *lf=fopen(migrated,"w"); assert(lf); fputs("old room pocket item\n",lf); fclose(lf);
    snprintf(migrated,sizeof(migrated),"%s/legacy_room_wallet_item.r2item",legacy_room_wallet);
    lf=fopen(migrated,"w"); assert(lf); fputs("old room wallet item\n",lf); fclose(lf);
    snprintf(legacy,sizeof(legacy),"%s/pockets",R2_ROOT);
    snprintf(legacy_wallet,sizeof(legacy_wallet),"%s/wallet",legacy);
    assert(mkdir(legacy,0755)==0);
    assert(mkdir(legacy_wallet,0755)==0);
    snprintf(migrated,sizeof(migrated),"%s/legacy_item.r2item",legacy);
    lf=fopen(migrated,"w"); assert(lf); fputs("legacy pocket item\n",lf); fclose(lf);
    snprintf(migrated,sizeof(migrated),"%s/legacy_wallet_item.r2item",legacy_wallet);
    lf=fopen(migrated,"w"); assert(lf); fputs("legacy wallet item\n",lf); fclose(lf);
    snprintf(migrated,sizeof(migrated),"%s/cash.txt",legacy_wallet);
    lf=fopen(migrated,"w"); assert(lf); fputs("cash=999.00\n",lf); fclose(lf);
    assert(r2_reality_init()==0);
    assert(access(migrated,F_OK)!=0); /* The old cash.txt was only a stale derived summary. */
    assert(access(legacy,F_OK)!=0); /* Empty duplicate lowercase pocket folders are removed. */
    snprintf(migrated,sizeof(migrated),"%s/Pockets/legacy_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    snprintf(migrated,sizeof(migrated),"%s/Pockets/Wallet/legacy_wallet_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    snprintf(migrated,sizeof(migrated),"%s/Pockets/legacy_room_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    snprintf(migrated,sizeof(migrated),"%s/Pockets/Wallet/legacy_room_wallet_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    assert(bill_count(wallet)==5);
    assert(r2_reality_money_receive(1000000.01)!=0); /* Every money operation has the same hard cap. */
    assert(r2_reality_money_receive(1000000.00)!=0); /* A wallet cannot exceed its $1M balance cap. */
    assert(bill_count(wallet)==5);
    char *ctx=money_context(); assert(strstr(ctx,"carried cash=$5.00")); free(ctx);

    snprintf(bill,sizeof(bill),"%s/money",wallet);
    assert(unlink(bill)==0);
    ctx=money_context(); assert(strstr(ctx,"carried cash=$4.00")); free(ctx);
    r2_reality_shutdown();
    assert(r2_reality_init()==0);
    assert(bill_count(wallet)==4); /* Restart must not refill deleted money. */

    assert(r2_reality_money_deposit(1.50)==0);
    assert(bill_count(wallet)==2 && change_cents(wallet)==50);
    assert(bill_count(bank)==1 && change_cents(bank)==50);
    assert(r2_reality_money_withdraw(0.50)==0);
    assert(bill_count(wallet)==3 && change_cents(wallet)==0);
    assert(bill_count(bank)==1 && change_cents(bank)==0);
    assert(r2_reality_buy_item("soda","A purchased drink",3.50,"fridge")==0);
    /* Pockets travel with R2; room storage and the fixed fridge do not. */
    assert(r2_reality_set_location(0)==0);
    assert(r2_reality_add_item("travel toy","Portable toy","pockets",1)==0);
    char pocket_item[1200];
    snprintf(pocket_item,sizeof(pocket_item),"%s/Pockets/travel_toy.r2item",R2_ROOT);
    assert(access(pocket_item,F_OK)==0); /* Item mirrors share the authoritative Pockets tree. */
    assert(r2_reality_add_item("room-only toy","Toy left at home","room",1)!=0);
    assert(r2_fridge_take("soda")!=0);
    assert(r2_reality_set_location(1)==0);
    assert(bill_count(wallet)==0 && change_cents(wallet)==0);
    assert(bill_count(bank)==0 && change_cents(bank)==50);
    assert(r2_reality_buy_item("expensive item","Not affordable",1.00,"pockets")!=0);
    ctx=money_context(); assert(strstr(ctx,"total=$0.50")); free(ctx);
    r2_reality_shutdown();
    assert(r2_reality_init()==0);
    assert(bill_count(wallet)==0 && bill_count(bank)==0);
    assert(change_cents(bank)==50); /* Fractional change persists without a seed. */
    snprintf(bill,sizeof(bill),"%s/change.txt",bank);
    assert(unlink(bill)==0);
    ctx=money_context(); assert(strstr(ctx,"total=$0.00")); free(ctx);
    r2_reality_shutdown();
    puts("Physical money wallet smoke test passed.");
    return 0;
}
