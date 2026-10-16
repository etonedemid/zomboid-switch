/* extension loading is compiled out (no dlopen of arbitrary .so on Horizon) */
struct sqlite3;
int sqlite3_enable_load_extension(struct sqlite3 *db, int onoff) { (void)db; (void)onoff; return 1; /* SQLITE_ERROR */ }
