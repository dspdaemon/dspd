#include "sslib.h"
void test_chmap_get_default(void)
{
  const struct dspd_pcm_chmap *map;
  int i;
  size_t j;
  const char *name;
  printf("Testing default channel maps...\n");
  DSPD_ASSERT(dspd_pcm_chmap_get_default(0UL) == NULL);
  for ( i = 1; i <= DSPD_CHMAP_LAST; i++ )
    {
      map = dspd_pcm_chmap_get_default(i);
      if ( map )
	{
	  printf("%d: ", i);
	  DSPD_ASSERT(map->ichan == 0);
	  DSPD_ASSERT(map->ochan == 0);
	  for ( j = 0; j < map->count; j++ )
	    {
	      name = dspd_pcm_chmap_channel_name(map->pos[j], true);
	      DSPD_ASSERT(name != NULL);
	      DSPD_ASSERT(strcmp(name, "UNKNOWN") != 0);
	      printf("%s ", name);
	    }
	  printf("\n");
	}
    }
  printf("\n");
}


void test_chmap_test(void)
{
  const struct dspd_pcm_chmap *bigmap, *map, *monomap;
  size_t i, o, j;
  int32_t ret;
  struct dspd_pcm_chmap_container badmap, goodmap;
  printf("Testing channel map verification...");
  bigmap = dspd_pcm_chmap_get_default(8UL);
  DSPD_ASSERT(bigmap != NULL);

  for ( i = 2; i <= 6UL; i++ )
    {
      map = dspd_pcm_chmap_get_default(i);
      if ( map )
	{
	  ret = dspd_pcm_chmap_test(map, bigmap);
	  if ( ret != 0 )
	    {
	      fprintf(stderr, "dspd_pcm_chmap_test(ch=%ld): %d\n", (long)i, ret);
	      DSPD_ASSERT(ret == 0);
	    }
	}
    }

  //Test a map with a channel that is not present
  monomap = dspd_pcm_chmap_get_default(1);
  DSPD_ASSERT(dspd_pcm_chmap_test(monomap, bigmap) == -EBADSLT);
  
  //Test an invalid channel map
  memset(&badmap, 0, sizeof(badmap));
  DSPD_ASSERT(dspd_pcm_chmap_test(&badmap.map, bigmap) == -EINVAL);
  

  //Make sure the struct layout is good
  for ( o = 0, i = DSPD_CHMAP_FL; i <= DSPD_CHMAP_LAST; i++, o++ )
    badmap.pos[o] = i;
  badmap.map.count = o;
  badmap.map.ichan = o;
  for ( i = 0; i < badmap.map.count; i++ )
    {
      //If this one fails then a mistake was made in the headers.
      DSPD_ASSERT(&badmap.map.pos[i] == &badmap.pos[i]);
      //If this one fails then the compiler must be broken
      DSPD_ASSERT(badmap.map.pos[i] == badmap.pos[i]);
    }


  //Reverse each channel map.  The result should indicate that a map is required for
  //conversion and that conversion is possible.
  for ( i = 2; i <= 6UL; i++ )
    {
      map = dspd_pcm_chmap_get_default(i);
      if ( map )
	{
	  memset(&goodmap, 0, sizeof(goodmap));
	  goodmap.map.ichan = map->ichan;
	  goodmap.map.count = map->count;
	  for ( j = map->count; j > 0; j-- )
	    goodmap.map.pos[map->count - j] = map->pos[j-1UL];
	  
	  
	  ret = dspd_pcm_chmap_test(map, &goodmap.map);
	  if ( ret != 1 )
	    {
	      fprintf(stderr, "dspd_pcm_chmap_test(ch=%ld): %d\n", (long)i, ret);
	      DSPD_ASSERT(ret == 1);
	    }
	}
    }

  
  map = dspd_pcm_chmap_get_default(2UL);
  memset(&badmap, 0, sizeof(badmap));
  badmap.map.flags = DSPD_CHMAP_MATRIX | DSPD_CHMAP_MULTI;
  badmap.map.ichan = 2UL;
  badmap.map.ochan = badmap.map.ichan;
  badmap.map.count = 4UL;
  badmap.map.pos[0] = 0;
  badmap.map.pos[1] = 0;
  badmap.map.pos[2] = 1;
  badmap.map.pos[3] = 5;
  ret = dspd_pcm_chmap_test(&badmap.map, map);
  if ( ret != -ECHRNG )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == -ECHRNG);
    }
  
  memset(&badmap, 0, sizeof(badmap));
  badmap.map.flags = DSPD_CHMAP_MATRIX | DSPD_CHMAP_MULTI;
  badmap.map.ichan = 2UL;
  badmap.map.ochan = badmap.map.ichan;
  badmap.map.count = 4UL;
  badmap.map.pos[0] = 0;
  badmap.map.pos[1] = 0;
  badmap.map.pos[2] = 0;
  badmap.map.pos[3] = 5;
  ret = dspd_pcm_chmap_test(&badmap.map, map);
  if ( ret != -ECHRNG )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == -ECHRNG);
    }

  memset(&badmap, 0, sizeof(badmap));
  badmap.map.flags = DSPD_CHMAP_MATRIX;
  badmap.map.ichan = 2UL;
  badmap.map.ochan = badmap.map.ichan;
  badmap.map.count = 2UL;
  badmap.map.pos[0] = 0;
  badmap.map.pos[1] = 2;
  ret = dspd_pcm_chmap_test(&badmap.map, map);
  if ( ret != -ECHRNG )
    {
      fprintf(stderr, "Error!!! %d\n", ret);
      DSPD_ASSERT(ret == -ECHRNG);
    }


  memset(&badmap, 0, sizeof(badmap));
  badmap.map.flags = DSPD_CHMAP_MATRIX;
  badmap.map.ichan = 2UL;
  badmap.map.ochan = 8UL;
  badmap.map.count = 2UL;
  badmap.map.pos[0] = 0;
  badmap.map.pos[1] = 16;
  ret = dspd_pcm_chmap_test(&badmap.map, bigmap);
  if ( ret != -ECHRNG )
    {
      fprintf(stderr, "Error!!!!! %d\n", ret);
      DSPD_ASSERT(ret == -ECHRNG);
    }


  memset(&badmap, 0, sizeof(badmap));
  ret = dspd_pcm_chmap_test(&badmap.map, map);
  if ( ret != -EINVAL )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == -EINVAL);
    }

  badmap.map.flags = DSPD_CHMAP_MATRIX;
  ret = dspd_pcm_chmap_test(&badmap.map, map);
  if ( ret != -EINVAL )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == -EINVAL);
    }

  printf("OK\n");
  
}



void fill_chmap(struct dspd_pcm_chmap *map, size_t count)
{
  size_t i;
  uint32_t *p;
  map->ichan = count;
  map->ochan = count;
  map->count = count;
  memset(&map->flags, 0xFF, sizeof(map->flags));
  for ( i = 0; i < count; i++ )
    {
      p = &map->pos[i];
      memset(p, 0xFF, sizeof(*p));
    }
}

void test_chmap_sizeof(void)
{
  size_t i, len, *addr;
  char *p;
  struct dspd_pcm_chmap *map;
  printf("Testing dspd_pcm_chmap_sizeof...");
  for ( i = 1; i <= DSPD_CHMAP_LAST; i++ )
    {
      len = dspd_pcm_chmap_sizeof(i, 0);
      DSPD_ASSERT(len > 0); 
      DSPD_ASSERT(len > sizeof(*map));
      p = calloc(1, len+sizeof(intptr_t));
      addr = (size_t*)&p[len];
      map = (struct dspd_pcm_chmap*)p;
      fill_chmap(map, i);
      DSPD_ASSERT(*addr == 0UL);
      free(map);

      len = dspd_pcm_chmap_sizeof(i, DSPD_CHMAP_MULTI);
      p = calloc(1, len+sizeof(intptr_t));
      addr = (size_t*)&p[len];
      map = (struct dspd_pcm_chmap*)p;
      fill_chmap(map, i * 2);
      DSPD_ASSERT(*addr == 0UL);
      free(map);
    }
  printf("OK\n");
}




void test_chmap_index(void)
{
  //Test index<>name conversion
  size_t i;
  ssize_t index;
  const char *name;
  printf("Testing channel index and names...");
  for ( i = DSPD_CHMAP_UNKNOWN; i <= DSPD_CHMAP_LAST; i++ )
    {
      name = dspd_pcm_chmap_channel_name(i, true);
      DSPD_ASSERT(name != NULL);
      index = dspd_pcm_chmap_index(name);
      DSPD_ASSERT(index >= DSPD_CHMAP_UNKNOWN && index <= DSPD_CHMAP_LAST);
      if ( index != (ssize_t)i )
	{
	  fprintf(stderr, "name='%s' index=%ld i=%ld\n", name, (long)index, (long)i);
	  DSPD_ASSERT(index == (ssize_t)i);
	}
    }
  printf("OK\n");
  
}


void test_chmap_from_string(void)
{
  char buf[1024];
  char buf2[1024];
  const struct dspd_pcm_chmap *map;
  struct dspd_pcm_chmap_container m;
  size_t i;
  ssize_t ret;
  printf("Testing string conversions...");

  //Convert layouts
  for ( i = 1UL; i <= 8UL; i++ )
    {
      map = dspd_pcm_chmap_get_default(i);
      if ( ! map )
	continue;
      ret = dspd_pcm_chmap_to_string(map, buf, sizeof(buf));
      DSPD_ASSERT(ret >= 0);
      memset(&m, 0, sizeof(m));
      m.map.count = i;
      ret = dspd_pcm_chmap_from_string(buf, &m);
      DSPD_ASSERT(ret >= 0);
      ret = dspd_pcm_chmap_to_string(&m.map, buf2, sizeof(buf2));
      DSPD_ASSERT(ret >= 0);
      DSPD_ASSERT(strcmp(buf2, buf) == 0);
    }

  //Test invalid buffer size
  map = dspd_pcm_chmap_get_default(2UL);
  DSPD_ASSERT(map != NULL);
  memset(buf, 0, sizeof(buf));
  ret = dspd_pcm_chmap_to_string(map, buf, 1);
  DSPD_ASSERT(ret == -ENOSPC);
  DSPD_ASSERT(buf[1] == 0);
  
  //Test matrix conversions
  memset(&m, 0, sizeof(m));
  ret = dspd_pcm_chmap_from_string("0=>0,1=>1", &m);
  DSPD_ASSERT(ret == 0);
  printf("OK\n");
  

}

void test_chmap_translate(void)
{
  //Test translation
  //Need to test various correct and incorrect maps
  //against both predefined results and with dspd_pcm_chmap_test
  struct dspd_pcm_chmap_container m, m2;
  const struct dspd_pcm_chmap *in, *out;
  int32_t ret;
  uint32_t n;
  printf("Testing translation...");

  //Stereo to mono
  in = dspd_pcm_chmap_get_default(2UL);
  DSPD_ASSERT(in->count > 0);
  out = dspd_pcm_chmap_get_default(1UL);
  DSPD_ASSERT(out->count > 0);
  memset(&m, 0, sizeof(m));
  m.map.count = 2UL;
  m.map.ichan = m.map.count;
  ret = dspd_pcm_chmap_translate(in, out, &m.map);
  if ( ret != 0 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 0);
    }
  DSPD_ASSERT(m.map.ichan > 0);
  DSPD_ASSERT(m.map.ochan > 0);
  DSPD_ASSERT(m.map.count > 0);
  DSPD_ASSERT(m.map.flags & DSPD_CHMAP_MATRIX);
  ret = dspd_pcm_chmap_test(&m.map, out);
  if ( ret != 1 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 1);
    }


  //Mono to stereo
  in = dspd_pcm_chmap_get_default(1UL);
  out = dspd_pcm_chmap_get_default(2UL);
  memset(&m, 0, sizeof(m));
  m.map.count = 4UL;
  m.map.ichan = m.map.count;
  ret = dspd_pcm_chmap_translate(in, out, &m.map);
  if ( ret != 0 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 0);
    }
  DSPD_ASSERT(m.map.flags & DSPD_CHMAP_MATRIX);

  ret = dspd_pcm_chmap_test(&m.map, out);
  if ( ret != 1 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 1);
    }
  

  in = dspd_pcm_chmap_get_default(2UL);
  out = dspd_pcm_chmap_get_default(2UL);
  memset(&m, 0, sizeof(m));
  m.map.count = 4UL;
  m.map.ichan = m.map.count;
  ret = dspd_pcm_chmap_translate(in, out, &m.map);
  if ( ret != 0 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 0);
    }
  DSPD_ASSERT(m.map.flags & DSPD_CHMAP_MATRIX);
  DSPD_ASSERT(m.map.count != 0);
  DSPD_ASSERT(m.map.ichan != 0);
  DSPD_ASSERT(m.map.ochan != 0);
  

  ret = dspd_pcm_chmap_test(&m.map, out);
  if ( ret != 1 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 1);
    }

  
  in = dspd_pcm_chmap_get_default(2UL);
  memset(&m2, 0, sizeof(m2));
  memcpy(&m2.map, in, dspd_pcm_chmap_sizeof(in->count, in->flags));
  n = m2.map.pos[0];
  m2.map.pos[0] = m2.map.pos[1];
  m2.map.pos[1] = n;
  out = dspd_pcm_chmap_get_default(8UL);
  memset(&m, 0, sizeof(m));
  m.map.count = 4UL;
  m.map.ichan = m.map.count;
  ret = dspd_pcm_chmap_translate(in, out, &m.map);
  if ( ret != 0 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 0);
    }
  DSPD_ASSERT(m.map.flags & DSPD_CHMAP_MATRIX);

  ret = dspd_pcm_chmap_test(&m.map, out);
  if ( ret != 1 )
    {
      fprintf(stderr, "Error %d\n", ret);
      DSPD_ASSERT(ret == 1);
    }
  printf("OK\n");
}




int main(void)
{
  test_chmap_get_default(); fflush(NULL);
  test_chmap_test(); fflush(NULL);
  test_chmap_sizeof(); fflush(NULL);
  test_chmap_index(); fflush(NULL);
  test_chmap_from_string(); fflush(NULL);
  test_chmap_translate(); fflush(NULL);
  return 0;
}
