#include "httppacket.h"

static packet*
httppacket_clone(packet*);

#define INIT_CONSTROUCTOR(p){\
	cast(packet*,p)->construct_write = NULL;             	\
	cast(packet*,p)->construct_read  = httppacket_clone;    \
	cast(packet*,p)->clone           = httppacket_clone;    \
}

void 
httppacket_dctor(void *_)
{
	st_header  *h;
	httppacket *p = cast(httppacket*,_);
	while((h = cast(st_header*,list_pop(&p->headers))))
		free(h);
}

httppacket*
httppacket_new(bytebuffer *b)
{
	httppacket *p = calloc(1,sizeof(*p));
	cast(packet*,p)->type = HTTPPACKET;
	p->method     = -1;
	if(b){
		cast(packet*,p)->head  = b;
		cast(packet*,p)->dctor = httppacket_dctor;
		refobj_inc(cast(refobj*,b));
	}
	INIT_CONSTROUCTOR(p);
	return p;		
}

static packet*
httppacket_clone(packet *_){
	st_header *h,*hh;
	listnode  *cur,*end;
	httppacket *o = cast(httppacket*,_);
	httppacket *p = calloc(1,sizeof(*p));
	cast(packet*,p)->type = HTTPPACKET;
	if(_->head){
		cast(packet*,p)->head  = _->head;
		cast(packet*,p)->dctor = httppacket_dctor;
		p->method              = o->method;
		refobj_inc(cast(refobj*,_->head));
		cur  = list_begin(&o->headers);
		end  = list_end(&o->headers);
		for(; cur != end;cur = cur->next){
			h    = cast(st_header*,cur);
			hh   = calloc(1,sizeof(*hh));
			hh->field = h->field;
			hh->value = h->value;
			list_pushback(&p->headers,cast(listnode*,hh));
		}
	}
	INIT_CONSTROUCTOR(p);
	return cast(packet*,p);	
}

int32_t
httppacket_onurl(httppacket *p,char *at, size_t length)
{
	p->url = at;
	return 0;
}

int32_t
httppacket_onstatus(httppacket *p,char *at, size_t length)
{
	p->status = at;
	return 0;
}

int32_t
httppacket_on_header_field(httppacket *p,char *at, size_t length)
{
	st_header  *h = malloc(sizeof(*h));
	h->field      = at;
	list_pushback(&p->headers,cast(listnode*,h));
	return 0;
}	

int32_t
httppacket_on_header_value(httppacket *p,char *at, size_t length)
{
	st_header *h = cast(st_header*,p->headers.tail);
	h->value     = at;
	return 0;
}

int32_t
httppacket_on_body(httppacket *p,char *at, size_t length)
{
	p->body     = at;
	p->bodysize = length;
	return 0;
}

void 
httppacket_set_method(httppacket *p,int32_t method)
{
	p->method = method;
}

const char *httppacket_get_value(httppacket *p,const char *field)
{
	st_header *h;
	listnode    *cur = list_begin(&p->headers);
	listnode    *end = list_end(&p->headers);
	for(; cur != end;cur = cur->next){
		h = cast(st_header*,cur);
		if(strcmp(field,h->field) == 0)
			return h->value;
	}
	return NULL;
}