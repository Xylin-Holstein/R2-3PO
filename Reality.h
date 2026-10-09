#ifndef R2_REALITY_H
#define R2_REALITY_H

#include <stddef.h>



#ifdef __cplusplus
extern "C" {
#endif

/*
 * R2 Reality Engine
 *
 * One persistent source of truth for:
 *   - SELF continuity: R2's changing needs, condition, intentions and state.
 *   - WORLD continuity: room, objects, containers, ownership and elapsed time.
 *
 * The engine owns its tables in r2_reality.db, separate from r2_memory.db.
 * r2_memory.db remains the searchable-memory and Life Log database. A
 * one-time migration imports legacy r2_reality_* tables without replacing
 * the existing memory, Log, or reflective diary.
 */
int r2_reality_init(void);
void r2_reality_shutdown(void);
int r2_reality_is_initialized(void);
int r2_reality_set_location(int home); /* 1=home, 0=away; persistent physical location. */
int r2_reality_tick(void);

/* Caller frees all returned strings. */
char *r2_reality_status(void);
char *r2_reality_room_look(void);
char *r2_reality_list(const char *container);
char *r2_reality_context(void);
char *r2_fridge_list(void);
char *r2_fridge_context(void);
void r2_fridge_shutdown(void);
int r2_fridge_take(const char *food);
int r2_fridge_add_item(const char *name, const char *description, int quantity,
                       double fullness, double energy, const char *ingredients, const char *taste);
int r2_reality_fridge_eat(const char *food, double fullness);

/* Persistent object/container operations. */
int r2_reality_add_item(const char *name, const char *description,
                        const char *container, int quantity);
int r2_reality_move_item(const char *name, const char *container);
int r2_reality_remove_item(const char *name);
int r2_reality_eat(const char *food, double fullness);
char *r2_reality_food_context(const char *food);
int r2_reality_rate_food(const char *food, int satisfaction, const char *notes);
int r2_reality_sleep(double hours);
int r2_reality_record_dream(const char *description);
int r2_reality_set_self(const char *key, const char *value,
                        const char *evidence);
char *r2_reality_get_self(const char *key);
char *r2_reality_money_context(void);
int r2_reality_money_receive(double amount); /* Explicitly received money; updates physical Wallet files. */
int r2_reality_money_deposit(double amount); /* Move physical money from Pockets/Wallet to room/piggybank. */
int r2_reality_money_withdraw(double amount); /* Move physical money from piggybank to Pockets/Wallet. */
int r2_reality_buy_item(const char *name,const char *description,double price,const char *container);

#ifdef __cplusplus
}
#endif

#endif
