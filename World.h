#ifndef R2_WORLD_H
#define R2_WORLD_H
#ifdef __cplusplus
extern "C" {
#endif
/* Initialize persistent folders and grant the initial wallet once. */
int r2_world_init(void);
/* Execute one validated [WORLD] request; caller frees returned string. */
char *r2_world_tool(const char *request);
#ifdef __cplusplus
}
#endif
#endif
