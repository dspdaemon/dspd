#ifndef _DSPD_LIST_H_
#define _DSPD_LIST_H_
struct dspd_slist;
struct dspd_rctx;
struct dspd_slist *dspd_slist_new(uint32_t entries);
void dspd_slist_delete(struct dspd_slist *l);
intptr_t dspd_slist_get_free(struct dspd_slist *list, int32_t whence);
void dspd_slist_entry_get_pointers(struct dspd_slist *list,
				   uint32_t entry,
				   void **data,
				   void **server_ops,
				   void **client_ops);
void dspd_slist_entry_set_pointers(struct dspd_slist *list,
				   uint32_t entry,
				   void *data,
				   void *server_ops,
				   void *client_ops);
void dspd_slist_entry_set_used(struct dspd_slist *list, uint32_t entry, bool used);
//Call this after setting a new slot used.  Returns reference count.
uint32_t dspd_slist_ref(struct dspd_slist *list, uint32_t index);
//Unref and set slot unused and call destructor when reference count is 0.
uint32_t dspd_slist_unref(struct dspd_slist *list, uint32_t index);
uint32_t dspd_slist_refcnt(struct dspd_slist *list, uint32_t index);
void dspd_slist_set_destructor(struct dspd_slist *list,
			       uint32_t index,
			       void (*destructor)(void *data));

void dspd_slist_entry_srvlock(struct dspd_slist *list, uint32_t entry);
uint32_t dspd_slist_entry_get_key(struct dspd_slist *list, uint32_t entry);
void dspd_slist_entry_set_key(struct dspd_slist *list, uint32_t entry, uint32_t key);
void dspd_slist_entry_srvunlock(struct dspd_slist *list, uint32_t entry);

void dspd_slist_entry_wrlock(struct dspd_slist *list, uint32_t entry);
void dspd_slist_entry_rdlock(struct dspd_slist *list, uint32_t entry);
void dspd_slist_entry_rw_unlock(struct dspd_slist *list, uint32_t entry);
void dspd_slist_rdlock(struct dspd_slist *list);
void dspd_slist_wrlock(struct dspd_slist *list);
void dspd_slist_unlock(struct dspd_slist *list);
bool dspd_client_srv_lock(struct dspd_slist *list, uint32_t index, uint32_t key);
bool dspd_client_srv_trylock(struct dspd_slist *list, uint32_t index, uint32_t key);
void dspd_client_srv_unlock(struct dspd_slist *list, uint32_t index);
uintptr_t dspd_slist_get_object_mask(struct dspd_slist *list,
				     uint8_t *mask, 
				     size_t   mask_size,
				     bool server, 
				     bool client);

void dspd_slist_set_ctl(struct dspd_slist *list,
			uint32_t object,
			int32_t (*ctl)(struct dspd_rctx *rctx,
				       uint32_t             req,
				       const void          *inbuf,
				       size_t        inbufsize,
				       void         *outbuf,
				       size_t        outbufsize));
int32_t dspd_slist_ctl(struct dspd_slist *list,
		       struct dspd_rctx *rctx,
		       uint32_t             req,
		       const void          *inbuf,
		       size_t        inbufsize,
		       void         *outbuf,
		       size_t        outbufsize);
uint64_t dspd_slist_id(struct dspd_slist *list, uintptr_t entry);
#endif
