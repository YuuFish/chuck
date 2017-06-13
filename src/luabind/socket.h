
#include "socket/chk_stream_socket_define.h"
#include "socket/chk_acceptor_define.h"

#define ACCEPTOR_METATABLE "lua_acceptor"

#define STREAM_SOCKET_METATABLE "lua_stream_socket"

#define SOCK_ADDR_METATABLE "lua_sock_addr"

#define SSL_CTX_METATABLE "lua_ssl_ctx"

typedef struct {
	chk_acceptor *c_acceptor;
}lua_acceptor;

typedef struct {
	chk_stream_socket *c_stream_socket;
}lua_stream_socket;

#define lua_checkacceptor(L,I)	\
	(lua_acceptor*)luaL_checkudata(L,I,ACCEPTOR_METATABLE)

#define lua_checkstreamsocket(L,I)	\
	(lua_stream_socket*)luaL_checkudata(L,I,STREAM_SOCKET_METATABLE)

#define lua_check_ssl_ctx(L,I)	\
	(SSL_CTX*)luaL_checkudata(L,I,SSL_CTX_METATABLE)

#define lua_check_sockaddr(L,I) \
	(chk_sockaddr*)luaL_checkudata(L,I,SOCK_ADDR_METATABLE)


static void lua_acceptor_cb(chk_acceptor *_,int32_t fd,chk_sockaddr *addr,chk_ud ud,int32_t err) {
	chk_luaRef   cb = ud.v.lr;
	const char   *error;
	if(0 == err) {
		error = chk_Lua_PCallRef(cb,"i",fd);
	}else {
		error = chk_Lua_PCallRef(cb,"pi",NULL,err);
	}
	if(error) {
		close(fd);
		CHK_SYSLOG(LOG_ERROR,"error on lua_acceptor_cb %s",error);		
	}
}

static int32_t lua_acceptor_gc(lua_State *L) {
	lua_acceptor *a = lua_checkacceptor(L,1);
	if(a->c_acceptor){
		chk_ud ud = chk_acceptor_get_ud(a->c_acceptor);
		chk_luaRef_release(&ud.v.lr);
		chk_acceptor_del(a->c_acceptor);
		a->c_acceptor = NULL;
	}
	return 0;
}

static int32_t lua_acceptor_pause(lua_State *L) {
	lua_acceptor *a = lua_checkacceptor(L,1);
	if(a->c_acceptor){
		chk_acceptor_pause(a->c_acceptor);
	}
	return 0;
}

static int32_t lua_acceptor_resume(lua_State *L) {
	lua_acceptor *a = lua_checkacceptor(L,1);
	if(a->c_acceptor){
		chk_acceptor_resume(a->c_acceptor);
	}
	return 0;
}

static int32_t lua_listen_ip4_ssl(lua_State *L) {
	chk_sockaddr    server;
	int32_t         fd;
	const char     *ip;
	int16_t         port;
	chk_event_loop *event_loop;
	lua_acceptor   *a; 
	SSL_CTX        *ssl_ctx;
	chk_luaRef      accept_cb;

	ssl_ctx = lua_check_ssl_ctx(L,2);

	if(0 > (fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))) {
		CHK_SYSLOG(LOG_ERROR,"socket() failed");
		return 0;
	}

	event_loop = lua_checkeventloop(L,1);
	ip = luaL_checkstring(L,3);
	port = (int16_t)luaL_checkinteger(L,4);
	
	if(0 != easy_sockaddr_ip4(&server,ip,port)) {
		CHK_SYSLOG(LOG_ERROR,"easy_sockaddr_ip4() failed,%s:%d",ip,port);
		close(fd);
		return 0;
	}	

	easy_addr_reuse(fd,1);
	if(0 != easy_listen(fd,&server)){
		CHK_SYSLOG(LOG_ERROR,"easy_listen() failed,%s:%d",ip,port);
		close(fd);
		return 0;
	}	

	if(!lua_isfunction(L,5)) 
		return luaL_error(L,"argument 5 of lua_listen_ip4_ssl must be lua function"); 
	a   = LUA_NEWUSERDATA(L,lua_acceptor);

	if(!a){
		CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA() failed");
		close(fd);	
		return 0;
	}
	memset(a,0,sizeof(*a));

	accept_cb = chk_toluaRef(L,5);

	a->c_acceptor = chk_acceptor_new(fd,chk_ud_make_lr(accept_cb));

	if(!a->c_acceptor){
		chk_luaRef_release(&accept_cb);
		CHK_SYSLOG(LOG_ERROR,"chk_acceptor_new() failed");
		close(fd);
		return 0;
	}

	luaL_getmetatable(L, ACCEPTOR_METATABLE);
	lua_setmetatable(L, -2);
	if(0 != chk_loop_add_handle(event_loop,(chk_handle*)a->c_acceptor,lua_acceptor_cb)) {
		close(fd);
		CHK_SYSLOG(LOG_ERROR,"event_loop add acceptor failed %s:%d",ip,port);
		return 0;
	}
	a->c_acceptor->ctx = ssl_ctx;
	return 1;
}

static int32_t lua_listen_ip4(lua_State *L) {
	chk_sockaddr    server;
	int32_t         fd;
	const char     *ip;
	int16_t         port;
	chk_event_loop *event_loop;
	lua_acceptor   *a;
	chk_luaRef      accept_cb; 
	if(0 > (fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))) {
		CHK_SYSLOG(LOG_ERROR,"socket() failed");
		return 0;
	}

	event_loop = lua_checkeventloop(L,1);
	ip = luaL_checkstring(L,2);
	port = (int16_t)luaL_checkinteger(L,3);
	
	if(0 != easy_sockaddr_ip4(&server,ip,port)) {
		CHK_SYSLOG(LOG_ERROR,"easy_sockaddr_ip4() failed,%s:%d",ip,port);
		close(fd);
		return 0;
	}	

	easy_addr_reuse(fd,1);
	if(0 != easy_listen(fd,&server)){
		CHK_SYSLOG(LOG_ERROR,"easy_listen() failed,%s:%d",ip,port);
		close(fd);
		return 0;
	}	

	if(!lua_isfunction(L,4)) 
		return luaL_error(L,"argument 4 of dail must be lua function"); 
	

	a = LUA_NEWUSERDATA(L,lua_acceptor);

	if(!a){
		CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA() failed");
		close(fd);	
		return 0;
	}

	memset(a,0,sizeof(*a));

	accept_cb = chk_toluaRef(L,4);

	a->c_acceptor = chk_acceptor_new(fd,chk_ud_make_lr(accept_cb));

	if(!a->c_acceptor){
		chk_luaRef_release(&accept_cb);
		CHK_SYSLOG(LOG_ERROR,"chk_acceptor_new() failed");
		close(fd);
		return 0;
	}

	luaL_getmetatable(L, ACCEPTOR_METATABLE);
	lua_setmetatable(L, -2);
	if(0 != chk_loop_add_handle(event_loop,(chk_handle*)a->c_acceptor,lua_acceptor_cb)) {
		close(fd);
		CHK_SYSLOG(LOG_ERROR,"event_loop add acceptor failed %s:%d",ip,port);
		return 0;
	}
	return 1;	
}


static void dail_ip4_cb(int32_t fd,chk_ud ud,int32_t err) {
	chk_luaRef cb = ud.v.lr;
	const char *error;
	if(0 == err) {
		error = chk_Lua_PCallRef(cb,"i",fd);
	}else {
		error = chk_Lua_PCallRef(cb,"pi",NULL,err);		
	} 
	if(error) {
		close(fd);
		CHK_SYSLOG(LOG_ERROR,"error on dail_ip4_cb %s",error);		
	}
	chk_luaRef_release(&cb);
}

static int32_t lua_dail_ip4(lua_State *L) {
	chk_luaRef      cb = {0};
	chk_sockaddr    remote;
	uint32_t        timeout;
	int32_t         fd,ret;
	const char     *ip;
	int16_t         port;
	chk_event_loop *event_loop;

	if(!lua_isfunction(L,4)) { 
		return luaL_error(L,"argument 4 of dail must be lua function");
	}

	if(0 > (fd = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))) {
		CHK_SYSLOG(LOG_ERROR,"socket() failed");		
		lua_pushstring(L,"create socket error");
		return 1;
	}

	event_loop = lua_checkeventloop(L,1);
	ip = luaL_checkstring(L,2);
	port = (int16_t)luaL_checkinteger(L,3);	

	if(0 != easy_sockaddr_ip4(&remote,ip,port)) {
		CHK_SYSLOG(LOG_ERROR,"easy_sockaddr_ip4() failed,%s:%d",ip,port);		
		close(fd);
		lua_pushstring(L,"lua_dail_ip4 invaild address or port");
		return 1;
	}

	cb = chk_toluaRef(L,4); 
	timeout = (uint32_t)luaL_optinteger(L,5,0);
	ret = chk_connect(fd,&remote,NULL,event_loop,dail_ip4_cb,chk_ud_make_lr(cb),timeout);
	if(ret != 0) {
		chk_luaRef_release(&cb);
		lua_pushstring(L,"connect error");
		return 1;
	}
	return 0;
}

static int32_t lua_stream_socket_gc(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(s->c_stream_socket){
		chk_stream_socket_close(s->c_stream_socket,0);
	}
	return 0;
}

static void data_cb(chk_stream_socket *s,chk_bytebuffer *data,int32_t error) {
	chk_luaRef cb = chk_stream_socket_getUd(s).v.lr;
	luaBufferPusher pusher = {PushBuffer,data};
	const char *error_str;
	if(!cb.L) return;
	if(data) error_str = chk_Lua_PCallRef(cb,"f",(chk_luaPushFunctor*)&pusher);
	else error_str = chk_Lua_PCallRef(cb,"pi",NULL,error);
	if(error_str) CHK_SYSLOG(LOG_ERROR,"error on data_cb %s",error_str);	
}

static int32_t lua_stream_socket_bind(lua_State *L) {
	lua_stream_socket *s;
	chk_event_loop    *event_loop;
	chk_luaRef cb  = {0}; 
	s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	event_loop = lua_checkeventloop(L,2);
	if(!lua_isfunction(L,3)) 
		return luaL_error(L,"argument 3 of stream_socket_bind must be lua function");
	cb = chk_toluaRef(L,3);
	chk_stream_socket_setUd(s->c_stream_socket,chk_ud_make_lr(cb));
	if(0 != chk_loop_add_handle(event_loop,(chk_handle*)s->c_stream_socket,data_cb)) {
		CHK_SYSLOG(LOG_ERROR,"chk_loop_add_handle() failed");
		lua_pushstring(L,"stream_socket_bind failed");
		return 1;
	}
	return 0;
}

static int32_t lua_stream_socket_new(lua_State *L) {
	int32_t fd;
	lua_stream_socket *s;
	chk_stream_socket *c_stream_socket;
	chk_stream_socket_option option = {
		.decoder = NULL
	};
	fd = (int32_t)luaL_checkinteger(L,1);
	option.recv_buffer_size = (uint32_t)luaL_optinteger(L,2,4096);
	if(lua_islightuserdata(L,3)) option.decoder = lua_touserdata(L,3);
	s = LUA_NEWUSERDATA(L,lua_stream_socket);
	if(!s) {
		CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA(lua_stream_socket) failed");
		return 0;
	}

	memset(s,0,sizeof(*s));

	c_stream_socket = chk_stream_socket_new(fd,&option);

	if(!c_stream_socket){
		CHK_SYSLOG(LOG_ERROR,"chk_stream_socket_new() failed");
		return 0;
	}

	s->c_stream_socket = c_stream_socket;
	luaL_getmetatable(L, STREAM_SOCKET_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}	

static int32_t lua_stream_socket_close(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	chk_luaRef cb;
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	uint32_t delay = (uint32_t)luaL_optinteger(L,2,0);		
	chk_stream_socket_close(s->c_stream_socket,delay);
	cb = chk_stream_socket_getUd(s->c_stream_socket).v.lr;
	if(cb.L) {
		chk_luaRef_release(&cb);
	}
	s->c_stream_socket = NULL;
	return 0;
}

static int32_t lua_stream_socket_pause(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	chk_stream_socket_pause(s->c_stream_socket);
	return 0;
}

static int32_t lua_stream_socket_resume(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	chk_stream_socket_resume(s->c_stream_socket);
	return 0;
}

static int32_t lua_close_fd(lua_State *L) {
	int32_t fd = (int32_t)luaL_checkinteger(L,1);
	close(fd);
	return 0;
}

static void chk_lua_send_cb(chk_stream_socket *s,chk_ud ud,int32_t err) {
	chk_luaRef cb = ud.v.lr;
	const char *error; 
	if(NULL != (error = chk_Lua_PCallRef(cb,"i",err))) {
		CHK_SYSLOG(LOG_ERROR,"error on lua_send_cb %s",error);
	}	
	chk_luaRef_release(&cb);
}


static int32_t lua_stream_socket_delay_send(lua_State *L) {
	chk_bytebuffer    *b,*o;
	chk_send_cb        send_cb = NULL;
	chk_luaRef         lua_cb = {0};
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}

	o = lua_checkbytebuffer(L,2);
	if(NULL == o)
		luaL_error(L,"need bytebuffer to send");
	b = chk_bytebuffer_clone(o);
	if(!b) {
		lua_pushstring(L,"send error");		
		return 1;
	}

	if(lua_isfunction(L,3)){
		lua_cb = chk_toluaRef(L,3);
		send_cb = chk_lua_send_cb;
	} 

	if(0 != chk_stream_socket_delay_send(s->c_stream_socket,b,send_cb,chk_ud_make_lr(lua_cb))){
		if(lua_cb.L){
			chk_luaRef_release(&lua_cb);
		}
		lua_pushstring(L,"send error");
		return 1;
	}
	return 0;
}

static int32_t lua_stream_socket_send(lua_State *L) {
	chk_bytebuffer    *b,*o;
	chk_send_cb        send_cb = NULL;
	chk_luaRef         lua_cb = {0};
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		lua_pushstring(L,"socket close");		
		return 1;
	}
	o = lua_checkbytebuffer(L,2);
	if(NULL == o)
		luaL_error(L,"need bytebuffer to send");
	b = chk_bytebuffer_clone(o);
	if(!b) {
		lua_pushstring(L,"send error");		
		return 1;
	}

	if(lua_isfunction(L,3)){
		lua_cb = chk_toluaRef(L,3);
		send_cb = chk_lua_send_cb;
	} 

	if(0 != chk_stream_socket_send(s->c_stream_socket,b,send_cb,chk_ud_make_lr(lua_cb))){
		if(lua_cb.L){
			chk_luaRef_release(&lua_cb);
		}
		lua_pushstring(L,"send error");
		return 1;
	}
	return 0;
}

static int32_t lua_stream_socket_send_urgent(lua_State *L) {
	chk_bytebuffer    *b,*o;
	chk_send_cb        send_cb = NULL;
	chk_luaRef         lua_cb = {0};	
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		lua_pushstring(L,"socket close");		
		return 1;
	}

	o = lua_checkbytebuffer(L,2);
	if(NULL == o)
		luaL_error(L,"need bytebuffer to send");
	b = chk_bytebuffer_clone(o);
	if(!b) {
		lua_pushstring(L,"send error");		
		return 1;
	}

	if(lua_isfunction(L,3)){
		lua_cb = chk_toluaRef(L,3);
		send_cb = chk_lua_send_cb;
	}

	if(0 != chk_stream_socket_send_urgent(s->c_stream_socket,b,send_cb,chk_ud_make_lr(lua_cb))){
		if(lua_cb.L){
			chk_luaRef_release(&lua_cb);
		}
		lua_pushstring(L,"send error");				
		return 1;
	}
	return 0;
}

static int32_t lua_stream_socket_flush(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		lua_pushstring(L,"socket close");		
		return 1;
	}
	if(0 != chk_stream_socket_flush(s->c_stream_socket)) {
		lua_pushstring(L,"flush error");
		return 1;
	}
	return 0;
}

static int32_t lua_stream_socket_pending_send_size(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	lua_pushinteger(L,chk_stream_socket_pending_send_size(s->c_stream_socket));
	return 1;
}
//lua_check_sockaddr
static int32_t lua_stream_socket_getsockaddr(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	chk_sockaddr *addr = LUA_NEWUSERDATA(L,chk_sockaddr);
	if(NULL == addr) {
		return 0;
	}

	if(0 != chk_stream_socket_getsockaddr(s->c_stream_socket,addr)){
		return 0;
	}

	luaL_getmetatable(L, SOCK_ADDR_METATABLE);
	lua_setmetatable(L, -2);

	return 1;	
}

static int32_t lua_stream_socket_getpeeraddr(lua_State *L) {
	lua_stream_socket *s = lua_checkstreamsocket(L,1);
	if(!s->c_stream_socket){
		return luaL_error(L,"invaild lua_stream_socket");
	}
	chk_sockaddr *addr = LUA_NEWUSERDATA(L,chk_sockaddr);
	if(NULL == addr) {
		return 0;
	}

	if(0 != chk_stream_socket_getpeeraddr(s->c_stream_socket,addr)){
		return 0;
	}

	luaL_getmetatable(L, SOCK_ADDR_METATABLE);
	lua_setmetatable(L, -2);

	return 1;	
}

//int32_t easy_sockaddr_inet_ntop(chk_sockaddr *addr,char *out,int len);

//int32_t easy_sockaddr_port(chk_sockaddr *addr,uint16_t *port);

static int32_t lua_inet_ntop(lua_State *L) {
	chk_sockaddr *addr = lua_check_sockaddr(L,1);
	if(NULL == addr) {
		return luaL_error(L,"invaild chk_sockaddr");
	}
	char buff[46];
	if(0 != easy_sockaddr_inet_ntop(addr,buff,sizeof(buff))) {
		return 0;
	}
	lua_pushstring(L,buff);
	return 1;
}

static int32_t lua_inet_port(lua_State *L) {
	chk_sockaddr *addr = lua_check_sockaddr(L,1);
	if(NULL == addr) {
		return luaL_error(L,"invaild chk_sockaddr");
	}
	uint16_t port;
	if(0 != easy_sockaddr_port(addr,&port)) {
		return 0;
	}
	lua_pushinteger(L,port);
	return 1;
}

static void register_socket(lua_State *L) {
	luaL_Reg acceptor_mt[] = {
		{"__gc", lua_acceptor_gc},
		{NULL, NULL}
	};

	luaL_Reg acceptor_methods[] = {
		{"Pause",    lua_acceptor_pause},
		{"Resume",	 lua_acceptor_resume},
		{"Close",    lua_acceptor_gc},
		{NULL,		 NULL}
	};

	luaL_Reg stream_socket_mt[] = {
		{"__gc", lua_stream_socket_gc},
		{NULL, NULL}
	};

	luaL_Reg stream_socket_methods[] = {
		{"Send",    	lua_stream_socket_send},
		{"SendUrgent",	lua_stream_socket_send_urgent},
		{"DelaySend",	lua_stream_socket_delay_send},
		{"Flush",		lua_stream_socket_flush},
		{"Start",   	lua_stream_socket_bind},
		{"Pause",   	lua_stream_socket_pause},
		{"Resume",		lua_stream_socket_resume},		
		{"Close",   	lua_stream_socket_close},
		{"GetSockAddr", lua_stream_socket_getsockaddr},
		{"GetPeerAddr", lua_stream_socket_getpeeraddr},	
		{"PendingSendSize",lua_stream_socket_pending_send_size},
		{NULL,     		NULL}
	};

	luaL_newmetatable(L, ACCEPTOR_METATABLE);
	luaL_setfuncs(L, acceptor_mt, 0);

	luaL_newlib(L, acceptor_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, STREAM_SOCKET_METATABLE);
	luaL_setfuncs(L, stream_socket_mt, 0);

	luaL_newlib(L, stream_socket_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, SOCK_ADDR_METATABLE);
	lua_pop(L, 1);

	lua_newtable(L);

	lua_pushstring(L,"util");
	lua_newtable(L);
	SET_FUNCTION(L,"inet_ntop",lua_inet_ntop);
	SET_FUNCTION(L,"inet_port",lua_inet_port);
	lua_settable(L,-3);	

	lua_pushstring(L,"stream");
	lua_newtable(L);
	
	SET_FUNCTION(L,"New",lua_stream_socket_new);

	lua_pushstring(L,"ip4");
	lua_newtable(L);
	SET_FUNCTION(L,"dail",lua_dail_ip4);
	SET_FUNCTION(L,"listen",lua_listen_ip4);
	SET_FUNCTION(L,"listen_ssl",lua_listen_ip4_ssl);
	lua_settable(L,-3);

	lua_settable(L,-3);

	SET_FUNCTION(L,"closefd",lua_close_fd);


}

