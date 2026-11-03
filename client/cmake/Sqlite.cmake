# SQLite3MultipleCiphers: SQLCipher v4 scheme only, no extension loading, temporary data in memory
# (add-client-foundation design D6). FTS5 is used by Thread search and client/knowledge-graph.
add_library(sqlite3mc STATIC ${TP}/sqlite3mc/sqlite3mc_amalgamation.c)
target_include_directories(sqlite3mc SYSTEM PUBLIC ${TP}/sqlite3mc)
target_compile_definitions(sqlite3mc PUBLIC
  SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_TEMP_STORE=3 SQLITE_ENABLE_FTS5 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  HAVE_CIPHER_SQLCIPHER=1 HAVE_CIPHER_AES_128_CBC=0 HAVE_CIPHER_AES_256_CBC=0 HAVE_CIPHER_CHACHA20=0
  HAVE_CIPHER_RC4=0 HAVE_CIPHER_ASCON128=0 HAVE_CIPHER_AEGIS=0 CODEC_TYPE=CODEC_TYPE_SQLCIPHER)
target_compile_options(sqlite3mc PRIVATE -O2 -w -maes -msse4.2)
