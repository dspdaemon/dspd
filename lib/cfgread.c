/*
 *  CFGREAD - Configuration file reader
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdbool.h>
#include <assert.h>
#include "cfgread.h"

void dspd_dict_dump(const struct dspd_dict *sect)
{
  struct dspd_kvpair *curr;
  size_t i;
  fprintf(stderr, "[%s]\n", sect->name);
  for ( i = 0; i < sect->count; i++ )
    {
      curr = &sect->list[i];
      fprintf(stderr, "%s=%s\n", curr->key, curr->value);
    }
}

static char *find_start(const char *buf)
{
  size_t i = 0;
  while ( buf[i] && isspace(buf[i]) )
    i++;
  return (char*)&buf[i];
}

static bool set_end(char *buf, bool checkspace)
{
  size_t i = 0;
  while ( buf[i] )
    {
      if ( (checkspace && isspace(buf[i])) || buf[i] == '\r' || buf[i] == '\n' )
	{
	  buf[i] = 0;
	  return true;
	}
      i++;
    }
  return false;
}

static struct dspd_dict *new_section(const char *buf, struct dspd_dict *sect)
{
  struct dspd_dict *ptr;
  char *p;
  ptr = calloc(1, sizeof(*ptr));
  if ( ptr )
    {
      if ( buf[0] == '[' )
	buf = &buf[1];
      ptr->name = strdup(buf);
      if ( ptr->name )
	{
	  p = strchr(ptr->name, ']');
	  if ( p )
	    *p = 0;
	  if ( sect )
	    {
	      sect->next = ptr;
	      ptr->prev = sect;
	    }
	} else
	{
	  free(ptr);
	  ptr = NULL;
	}
    }
  return ptr;
}

struct dspd_dict *dspd_dict_new(const char *name)
{
  return new_section(name, NULL);
}

bool dspd_dict_insert_value(struct dspd_dict *sect, const char *key, const char *value)
{
  struct dspd_kvpair *kvp;
  size_t len;
  void *ptr;
  if ( sect->count == sect->maxcount )
    {
      sect->maxcount++;
      len = sect->maxcount * sizeof(struct dspd_kvpair);
      ptr = realloc(sect->list, len);
      if ( ! ptr )
	return false;
      sect->list = ptr;
    }
  kvp = &sect->list[sect->count];
  if ( ! value )
    {
      kvp->value = NULL;
      kvp->key = strdup(key);
      if ( ! kvp->key )
	return false;
    } else
    {
      kvp->value = strdup(value);
      kvp->key = strdup(key);
      if ( ! (kvp->value && kvp->key) )
	{
	  free(kvp->value);
	  free(kvp->key);
	  return false;
	}
    }
  sect->count++;
  return true;
}

bool dspd_dict_set_value(struct dspd_dict *sect, 
			   const char *key, 
			   const char *value,
			   bool insert)
{
  struct dspd_kvpair *kvp;
  bool ret = false;
  void *p;
  kvp = dspd_dict_find_pair(sect, key);
  if ( kvp == NULL && insert == true )
    {
      ret = dspd_dict_insert_value(sect, key, value);
    } else if ( kvp )
    {
      p = kvp->value;
      if ( ! value )
	{
	  free(kvp->value);
	  kvp->value = NULL;
	} else
	{
	  kvp->value = strdup(value);
	  if ( ! kvp->value )
	    {
	      kvp->value = p;
	      ret = false;
	    } else
	    {
	      ret = true;
	      free(p);
	    }
	}
    } else
    {
      ret = false;
    }
  return ret;
}

static bool append_string(char *buf, struct dspd_dict *sect)
{
  struct dspd_kvpair pair = { NULL, NULL }, *kvp;
  char *p;
  void *ptr;
  size_t len;
  if ( ! sect )
    return false;
  p = strchr(buf, '=');
  if ( p )
    {
      *p = 0;
      pair.key = strdup(buf);
      if ( ! pair.key )
	goto out;
      pair.value = strdup(&p[1]);
      if ( ! pair.value )
	goto out;
    }
  if ( sect->count == sect->maxcount )
    {
      sect->maxcount += 32;
      len = sect->maxcount * sizeof(struct dspd_kvpair);
      ptr = realloc(sect->list, len);
      if ( ! ptr )
	goto out;
      sect->list = ptr;
      kvp = &sect->list[sect->count];
    } else
    {
      kvp = &sect->list[sect->count];
    }
  sect->count++;
    
  memcpy(kvp, &pair, sizeof(pair));
  return true;
  
 out:
  free(pair.key);
  free(pair.value);
  return false;
}

static void dict_free_section(struct dspd_dict *sect)
{
  size_t i;
  free(sect->name);
  for ( i = 0; i < sect->count; i++ )
    {
      free(sect->list[i].key);
      free(sect->list[i].value);
    }
}

void dspd_dict_free(struct dspd_dict *sect)
{
  struct dspd_dict *curr, *prev = NULL;
  for ( curr = sect; curr; curr = curr->next )
    {
      if ( prev )
	{
	  dict_free_section(prev);
	  free(prev);
	}
      prev = curr;
    }
  if ( prev )
    {
      dict_free_section(prev);
      free(prev);
    }
}

struct dspd_dict *dspd_dict_read(FILE *fp)
{
  char buf[4096];
  char *ptr;
  struct dspd_dict *sect = NULL, *n, *curr, *prev = NULL;
  while ( fgets(buf, sizeof(buf), fp) )
    {
      if ( buf[0] == ';' || buf[0] == '#' )
	continue;
      ptr = find_start(buf);
      if ( ! *ptr )
	continue;
      if ( ptr[0] == '[' )
	{
	  if ( ! set_end(ptr, true) )
	    continue;
	  n = new_section(ptr, sect);
	  if ( ! n )
	    goto out;
	  sect = n;
	} else
	{
	  if ( ! set_end(ptr, false) )
	    continue;
	  if ( ! append_string(buf, sect) )
	    goto out;
	}
    }

  for ( curr = sect; curr; curr = curr->prev )
    prev = curr;
  return prev;

 out:
  dspd_dict_free(sect);
  return NULL;
}


struct dspd_dict *dspd_dict_find_section(struct dspd_dict *sect, const char *name)
{
  struct dspd_dict *curr, *prev = sect;
  for ( curr = sect; curr; curr = curr->next )
    {
      assert(curr->next != prev);
      assert(curr->next != sect);
      assert(curr->next != curr);
      if ( curr->name != NULL && strcmp(curr->name, name) == 0 )
	break;
      prev = curr;
    }
  return curr;
}

struct dspd_kvpair *dspd_dict_find_pair(const struct dspd_dict *sect, const char *key)
{
  size_t i;
  const struct dspd_kvpair *pair, *ret = NULL;
  for ( i = 0; i < sect->count; i++ )
    {
      pair = &sect->list[i];
      if ( pair->key != NULL && strcmp(pair->key, key) == 0 )
	{
	  ret = pair;
	  break;
	}
    }
  return (struct dspd_kvpair*)ret;
}



bool dspd_dict_find_value(const struct dspd_dict *sect, const char *key, char **value)
{
  struct dspd_kvpair *pair;
  pair = dspd_dict_find_pair(sect, key);
  if ( pair )
    {
      if ( value )
	*value = pair->value;
    }
  return !!pair;
}

const char *dspd_dict_value_for_key(const struct dspd_dict *sect, const char *key)
{
  char *ret = NULL;
  dspd_dict_find_value(sect, key, &ret);
  return ret;
}

struct dspd_dict *dspd_dict_dup(const struct dspd_dict *sect)
{
  struct dspd_dict *ptr;
  size_t i;
  struct dspd_kvpair *dest;
  const struct dspd_kvpair *src;
  ptr = calloc(1, sizeof(*ptr));
  if ( ! ptr )
    return NULL;
  ptr->list = calloc(sect->count, sizeof(sect->list[0]));
  if ( ! ptr->list )
    goto out;
  ptr->name = strdup(sect->name);
  if ( ! ptr->name )
    goto out;
  ptr->count = sect->count;
  ptr->maxcount = ptr->count;
  for ( i = 0; i < sect->count; i++ )
    {
      src = &sect->list[i];
      dest = &ptr->list[i];
      dest->key = strdup(src->key);
      if ( src->value )
	{
	  dest->value = strdup(src->value);
	  if ( ! (dest->key && dest->value) )
	    goto out;
	} else if ( ! dest->key )
	{
	  goto out;
	}
    }
  
  return ptr;

 out:
  dspd_dict_free(ptr);
  return NULL;
}

/*
  Simplified exact width string to integer conversions.  See cfgread.h for C type 
  versions.
*/


int dspd_strtoi64(const char *str, int64_t *n, int base)
{
  char *eptr = NULL;
  long long ret;
  int err;
  errno = 0;
  ret = strtoll(str, &eptr, base);
  err = -errno;
  if ( ! (((ret == LLONG_MAX || ret == LLONG_MIN) && err == -ERANGE) || (ret == 0 && err == -EINVAL)))
    {
      if ( str[0] != 0 && eptr[0] == 0 && err == 0 && (!(eptr==str&&ret==0)) )
	{
	  *n = ret;
	} else if ( err == 0 )
	{
	  err = -ERANGE;
	}
    } else if ( err == 0 )
    {
      err = -ERANGE;
    }
  return err;
}

int dspd_strtou64(const char *str, uint64_t *n, int base)
{
  char *eptr = NULL;
  unsigned long long ret;
  int err;
  errno = 0;
  ret = strtoull(str, &eptr, base);
  err = -errno;
  if ( ! ((ret == ULLONG_MAX && err == -ERANGE) || (ret == 0 && err == -EINVAL)) )
    {
      if ( str[0] != 0 && eptr[0] == 0 && err == 0 && (!(eptr==str&&ret==0)) )
	{
	  *n = ret;
	} else if ( err == 0 )
	{
	  err = -ERANGE;
	}
    } else if ( err == 0 )
    {
      err = -ERANGE;
    }
  return err;
}


int dspd_strtoi32(const char *str, int32_t *n, int base)
{
  int64_t tmp;
  int ret;
  ret = dspd_strtoi64(str, &tmp, base);
  if ( ret == 0 )
    {
      if (tmp > INT32_MAX || tmp < INT32_MIN)
	ret = -ERANGE;
      else
	*n = tmp;
    }
  return ret;
}

int dspd_strtou32(const char *str, uint32_t *n, int base)
{
  uint64_t tmp;
  int ret;
  ret = dspd_strtou64(str, &tmp, base);
  if ( ret == 0 )
    {
      if ( tmp > UINT32_MAX )
	ret = -ERANGE;
      else
	*n = tmp;
    }
  return ret;
}




int dspd_strtoi16(const char *str, int16_t *n, int base)
{
  int64_t tmp;
  int ret;
  ret = dspd_strtoi64(str, &tmp, base);
  if ( ret == 0 )
    {
      if (tmp > INT16_MAX || tmp < INT16_MIN)
	ret = -ERANGE;
      else
	*n = tmp;
    }
  return ret;
}

int dspd_strtou16(const char *str, uint16_t *n, int base)
{
  uint64_t tmp;
  int ret;
  ret = dspd_strtou64(str, &tmp, base);
  if ( ret == 0 )
    {
      if ( tmp > UINT16_MAX )
	ret = -ERANGE;
      else
	*n = tmp;
    }
  return ret;
}



int dspd_strtoi8(const char *str, int8_t *n, int base)
{
  int64_t tmp;
  int ret;
  ret = dspd_strtoi64(str, &tmp, base);
  if ( ret == 0 )
    {
      if (tmp > INT8_MAX || tmp < INT8_MIN)
	ret = -ERANGE;
      else
	*n = tmp;
    }
  return ret;
}

int dspd_strtou8(const char *str, uint8_t *n, int base)
{
  uint64_t tmp;
  int ret;
  ret = dspd_strtou64(str, &tmp, base);
  if ( ret == 0 )
    {
      if ( tmp > UINT8_MAX )
	ret = -ERANGE;
      else
	*n = tmp;
    }
  return ret;
}

int dspd_strtoidef(const char *str, int defaultvalue)
{
  int ret, val;
  if ( ! str )
    return defaultvalue;
  ret = dspd_strtoi32(str, &val, 0);
  if ( ret == 0 )
    ret = val;
  else
    ret = defaultvalue;
  return ret;
}


bool dspd_dict_compare(const struct dspd_dict *sect1, const struct dspd_dict *sect2)
{
  size_t i;
  bool ret = 1;
  char *val;
  if ( strcmp(sect1->name, sect2->name) == 0 )
    {
      for ( i = 0; i < sect1->count; i++ )
	{
	  if ( ! dspd_dict_find_value(sect2, sect1->list[i].key, &val) )
	    {
	      ret = 0;
	      break;
	    }
	  if ( val == NULL || sect1->list[i].value == NULL )
	    {
	      if ( val != sect1->list[i].value )
		{
		  ret = 0;
		  break;
		}
	    } else if ( strcmp(sect1->list[i].value, val) )
	    {
	      ret = 0;
	      break;
	    }
	}
    } else
    {
      ret = 0;
    }
  return ret;
}

bool dspd_dict_test_value(const struct dspd_dict *sect,
			    const char *key,
			    const char *value)
{
  char *val;
  bool ret;
  if ( ! dspd_dict_find_value(sect, key, &val) )
    return 0;
  
  if ( val == NULL && value == NULL )
    {
      ret = 1;
    } else if ( value == NULL || val == NULL )
    {
      ret = 0;
    } else
    {
      ret = ( strcmp(value, val) == 0 );
    }
  return ret;
}

const char *dspd_dict_name(const struct dspd_dict *sect)
{
  return sect->name;
}
