#ifndef _DSPD_CFGREAD_H_
#define _DSPD_CFGREAD_H_
#include <stdio.h>
#include <stdint.h>

int dspd_strtoi64(const char *str, int64_t *n, int base);
int dspd_strtou64(const char *str, uint64_t *n, int base);
int dspd_strtoi32(const char *str, int32_t *n, int base);
int dspd_strtou32(const char *str, uint32_t *n, int base);
int dspd_strtoi16(const char *str, int16_t *n, int base);
int dspd_strtou16(const char *str, uint16_t *n, int base);
int dspd_strtoi8(const char *str, int8_t *n, int base);
int dspd_strtou8(const char *str, uint8_t *n, int base);


#if DSPD_WORDSIZE == 64
#define dspd_strtol(_s,_n,_b) dspd_strtoi64(_s,_n,_b)
#define dspd_strtoul(_s,_n,_b) dspd_strtou64(_s,_n,_b)
#else
static inline int dspd_strtol(const char *str, long *n, int base)
{
  return dspd_strtoi32(str, (int32_t*)n, base);
}
static inline int dspd_strtoul(const char *str, unsigned long *n, int base)
{
  return dspd_strtou32(str, (uint32_t*)n, base);
}
#endif
static inline int dspd_strtoll(const char *str, 
			       long long *n,
			       int base)
{
  return dspd_strtoi64(str, (int64_t*)n, base);
}
static inline int dspd_strtoull(const char *str, 
				unsigned long long *n,
				int base)
{
  return dspd_strtou64(str, (uint64_t*)n, base);
}

int dspd_strtoidef(const char *str, int defaultvalue);

struct dspd_kvpair {
  char *key;
  char *value;
};


struct dspd_dict {
  char          *name;
  size_t         count;
  size_t         maxcount;
  struct dspd_kvpair *list;
  struct dspd_dict *prev, *next;
};
void dspd_dict_dump(const struct dspd_dict *sect);
struct dspd_dict *dspd_dict_read(FILE *fp);
void dspd_dict_free(struct dspd_dict *sect);
struct dspd_dict *dspd_dict_find_section(struct dspd_dict *sect, const char *name);
struct dspd_kvpair *dspd_dict_find_pair(const struct dspd_dict *sect, const char *key);
bool dspd_dict_find_value(const struct dspd_dict *sect, const char *key, char **val);
struct dspd_dict *dspd_dict_new(const char *name);
bool dspd_dict_insert_value(struct dspd_dict *sect, const char *key, const char *value);
struct dspd_dict *dspd_dict_dup(const struct dspd_dict *sect);
bool dspd_dict_compare(const struct dspd_dict *sect1, const struct dspd_dict *sect2);

const char *dspd_dict_name(const struct dspd_dict *sect);

bool dspd_dict_set_value(struct dspd_dict *sect, 
			   const char *key, 
			   const char *value,
			   bool insert);
bool dspd_dict_test_value(const struct dspd_dict *sect,
			    const char *key,
			    const char *value);
const char *dspd_dict_value_for_key(const struct dspd_dict *sect, const char *key);
#endif
