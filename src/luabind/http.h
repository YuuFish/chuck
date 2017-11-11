#include "http/chk_http.h"
#include "http-parser/http_parser.h"

#define HTTP_PACKET_METATABLE "lua_http_packet"

#define HTTP_CONNECTION_METATABLE "lua_http_connection"

/*
*  防止超大header攻击,首先，无法依靠on_header_field和on_header_value统计准确的header长度，
*  因为回调中传入的是有效字节数量，攻击者可以使用在field和value之间插入大量空格实现攻击,例如:
*  filed  100k个空格 : 100k个空格 value,为了防止这样的攻击使用max_header_size作为一个模糊的参考值(>=接收缓冲)
*  每当一个包开始时check_size置0，并随着parser_execute的执行累计处理字节数。当parser_execute
*  执行完毕后，如果还没有出现body则将check_size与max_header_size做比较，如果check_size大于max_header_size
*  则认为出现了超大包，断开连接。
*/

typedef struct {
	http_parser           parser;
    http_parser_settings  settings;  
    chk_http_packet      *packet;
	chk_string           *field;
	chk_string           *value;	
	chk_string           *status;
	chk_string           *url;
	chk_stream_socket    *socket;
	chk_luaRef            cb;
	int32_t               method;
	int32_t               error;
	uint32_t              max_header_size;
	uint32_t              max_content_size;
	uint32_t              check_size;
}http_connection;

typedef struct {
	chk_http_packet *packet;
}lua_http_packet;

#define lua_check_http_packet(L,I)	\
	(lua_http_packet*)luaL_checkudata(L,I,HTTP_PACKET_METATABLE)

#define lua_check_http_connection(L,I)	\
	(http_connection*)luaL_checkudata(L,I,HTTP_CONNECTION_METATABLE)

#ifndef BYTEBUFFER_APPEND_CSTR
#define BYTEBUFFER_APPEND_CSTR(B,STR) chk_bytebuffer_append(B,(uint8_t*)(STR),strlen(STR))
#endif

#define RECV_BUFF_SIZE 8192	

static int32_t lua_http_connection_gc(lua_State *L) {
	http_connection *conn = lua_check_http_connection(L,1);
	if(conn->packet) chk_http_packet_release(conn->packet);
	if(conn->url) chk_string_destroy(conn->url);
	if(conn->status) chk_string_destroy(conn->status);
	if(conn->field) chk_string_destroy(conn->field);
	if(conn->value) chk_string_destroy(conn->value);			
	if(conn->socket) {
		chk_stream_socket_setUd(conn->socket,chk_ud_make_void(NULL));
		//delay 5秒关闭,尽量将数据发送出去		
		chk_stream_socket_close(conn->socket,5000);
	}
	chk_luaRef_release(&conn->cb);
	return 0;
}

static int32_t lua_http_packet_gc(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	chk_http_packet_release(packet->packet);
	return 0;
}
	
static int on_message_begin(http_parser *parser) {
	http_connection *conn = (http_connection*)parser;
	if(conn->packet) chk_http_packet_release(conn->packet);
	if(conn->field) chk_string_destroy(conn->field);
	if(conn->value) chk_string_destroy(conn->value);
	if(conn->url) chk_string_destroy(conn->url);
	if(conn->status) chk_string_destroy(conn->status);

	conn->packet = chk_http_packet_new();
	
	if(!conn->packet) {
		conn->error = chk_error_no_memory;
		CHK_SYSLOG(LOG_ERROR,"chk_http_packet_new() failed");
		return -1;
	}

	conn->error = 0;
	conn->url = conn->status = conn->field = conn->value = NULL;

	return 0;
}

static int on_url(http_parser *parser, const char *at, size_t length) {	
	http_connection *conn = (http_connection*)parser;
	if(conn->url){
		conn->error = chk_string_append(conn->url,at,length);
		if(0 != conn->error) {
			CHK_SYSLOG(LOG_ERROR,"chk_string_append() failed:%d",conn->error);
			return -1;
		}
	}
	else {
		conn->url = chk_string_new(at,length);
		if(!conn->url) {
			conn->error = chk_error_no_memory;
			CHK_SYSLOG(LOG_ERROR,"chk_string_new() failed");
			return -1;
		}
	}
	return 0;
}

static int on_status(http_parser *parser, const char *at, size_t length) {
	http_connection *conn = (http_connection*)parser;
	if(conn->status) {
		conn->error = chk_string_append(conn->status,at,length);
		if(0 != conn->error) {
			CHK_SYSLOG(LOG_ERROR,"chk_string_append() failed:%d",conn->error);
			return -1;
		}		
	}
	else {
		conn->status = chk_string_new(at,length);
		if(!conn->status) {
			conn->error = chk_error_no_memory;
			CHK_SYSLOG(LOG_ERROR,"chk_string_new() failed");
			return -1;
		}		
	}
	return 0;			
}

static int on_header_field(http_parser *parser, const char *at, size_t length) {
	http_connection *conn = (http_connection*)parser;
	size_t i;
	char   *ptr = (char*)at;
	//将field转成小写
	for(i = 0; i < length; ++i) {
		if(ptr[i] >= 'A' && ptr[i] <= 'Z') {
			ptr[i] += ('a'-'A');
		}
	}
	if(!conn->value && conn->field){
		conn->error = chk_string_append(conn->field,at,length);
		if(0 != conn->error) {
			CHK_SYSLOG(LOG_ERROR,"chk_string_append() failed:%d",conn->error);
			return -1;
		}		
	}
	else {
		if(conn->value) {
			conn->error = chk_http_set_header(conn->packet,conn->field,conn->value);
			if(0 != conn->error) {
				CHK_SYSLOG(LOG_ERROR,"chk_http_set_header() failed:%d",conn->error);
				return -1;
			}			
			conn->value = NULL;
		}

		conn->field = chk_string_new(at,length);

		if(!conn->field) {
			conn->error = chk_error_no_memory;
			CHK_SYSLOG(LOG_ERROR,"chk_string_new() failed");			
			return -1;
		}

	}
	return 0;				
}

static int on_header_value(http_parser *parser, const char *at, size_t length) {
	http_connection *conn = (http_connection*)parser;
	if(conn->value){
		conn->error = chk_string_append(conn->value,at,length);
		if(0 != conn->error) {
			CHK_SYSLOG(LOG_ERROR,"chk_string_append() failed:%d",conn->error);
			return -1;
		}		
	}
	else{
		conn->value = chk_string_new(at,length);
		if(!conn->value) {
			conn->error = chk_error_no_memory;
			CHK_SYSLOG(LOG_ERROR,"chk_string_new() failed");			
			return -1;
		}		
	}
	return 0;				
}

static int on_headers_complete(http_parser *parser) {	
	http_connection *conn = (http_connection*)parser;

	if(conn->field && conn->value) {
		conn->error = chk_http_set_header(conn->packet,conn->field,conn->value);
		
		if(0 != conn->error) {
			CHK_SYSLOG(LOG_ERROR,"chk_http_set_header() failed:%d",conn->error);
			return -1;
		}	

		conn->field = conn->value = NULL;
	}

	//检查是否有Content-Length,长度是否超限制
	const char *content_length = chk_http_get_header(conn->packet,"content-length");
	if(content_length && atol(content_length) > conn->max_content_size) {
		CHK_SYSLOG(LOG_ERROR,"content_length > conn->max_content_size");		
		conn->error = chk_error_http_packet;
		return -1;
	}
	return 0;		
}

static int on_body(http_parser *parser, const char *at, size_t length) {
	http_connection *conn = (http_connection*)parser;
	conn->error = chk_http_append_body(conn->packet,at,length);

	if(0 != conn->error) {
		CHK_SYSLOG(LOG_ERROR,"chk_http_append_body() failed:%d",conn->error);		
		return -1;
	}
	return 0;
}

typedef struct {
	void (*Push)(chk_luaPushFunctor *self,lua_State *L);
	chk_http_packet *packet;
}lua_http_packet_PushFunctor;

void lua_push_http_packet(chk_luaPushFunctor *self,lua_State *L) {
	lua_http_packet_PushFunctor *pushFunctor = (lua_http_packet_PushFunctor*)self;
	lua_http_packet *lua_packet = LUA_NEWUSERDATA(L,lua_http_packet);

	if(lua_packet) {
		lua_packet->packet = chk_http_packet_retain(pushFunctor->packet);
		luaL_getmetatable(L, HTTP_PACKET_METATABLE);
		lua_setmetatable(L, -2);
	}
	else {
		CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA() lua_http_packet failed");				
	}

}

static int on_message_complete(http_parser *parser)
{	
	const char   *error; 
	lua_http_packet_PushFunctor packet;
	http_connection *conn = (http_connection*)parser;
	chk_http_set_method(conn->packet,conn->method);
	chk_http_set_url(conn->packet,conn->url);
	chk_http_set_status(conn->packet,conn->status);

	if(conn->cb.L) {
		packet.packet = conn->packet;
		packet.Push = lua_push_http_packet;
		if(NULL != (error = chk_Lua_PCallRef(conn->cb,"f",&packet))) {
			CHK_SYSLOG(LOG_ERROR,"error on_message_complete %s",error);
		}
	}

	chk_http_packet_release(conn->packet);
	conn->url = NULL;
	conn->status = NULL;
	conn->packet = NULL;
	conn->check_size = 0;
	conn->error = 0;
	return 0;						
}

static void data_event_cb(chk_stream_socket *s,chk_bytebuffer *data,int32_t error) {
	size_t      nparsed;
	char       *buff;
	const char *errmsg;
	uint32_t    spos,size_remain,size;
	chk_bytechunk *chunk;
	http_connection *conn = (http_connection*)chk_stream_socket_getUd(s).v.val;
	if(!conn){ 
		CHK_SYSLOG(LOG_ERROR,"http_connection == NULL");		
		return;
	}
	if(data) {
		chunk = data->head;
		spos  = data->spos;
		size_remain = data->datasize;
		do{
			buff = chunk->data + spos;
			size = MIN(chunk->cap - spos,size_remain);
			nparsed = http_parser_execute(&conn->parser,&conn->settings,buff,size);
			
			/*
			 *  conn->error存在的必要性,on_xxx中返回非0的目的在于终止后续parse_execute处理，向外部通告错误
			 *  但即使在on_xxx中返回-1也存在nparsed == size的情况。例如parse_execute正好分析完size字节的数据
			 *  然后调用on_xxx,在其中返回-1。此时parse_execute已经分析完了size字节所以返回值nparse == size。  
			 *	为了防止这种情况出现在on_xxx中返回-1的时候同时设置conn->error标记。 
			*/
			
			if(!conn->error && nparsed == size) {
				conn->check_size += nparsed;
				
				if(conn->check_size > conn->max_header_size && 
				   conn->packet && !conn->packet->body) {
					CHK_SYSLOG(LOG_ERROR,"conn->check_size > conn->max_header_size,check_size:%d,max_header_size:%d",conn->check_size,conn->max_header_size);
					error = chk_error_http_packet;
					break;					
				}

				size_remain -= nparsed;				
				spos += nparsed;
				if(spos >= chunk->cap){	
					spos = 0;
					chunk = chunk->next;
				}
			}else {
				error = chk_error_http_packet;
				break;		
			}
		}while(size_remain);
	}

	if(!data || error){
		chk_stream_socket_setUd(conn->socket,chk_ud_make_void(NULL));
		chk_stream_socket_close(conn->socket,0);
		conn->socket = NULL;
		//用nil调用lua callback,通告连接断开
		if(NULL != (errmsg = chk_Lua_PCallRef(conn->cb,":"))) {
			CHK_SYSLOG(LOG_ERROR,"error data_event_cb %s",errmsg);
		}
	}	
}

static int32_t lua_new_http_connection(lua_State *L) {
	int32_t  fd;
	uint32_t max_header_size,max_content_size;
	chk_stream_socket *s;
	http_connection   *conn;
	chk_stream_socket_option option = {
		.decoder = NULL,
		.recv_buffer_size = RECV_BUFF_SIZE
	};
	fd = (int32_t)luaL_checkinteger(L,1);
	max_header_size = MAX((uint32_t)luaL_checkinteger(L,2),option.recv_buffer_size);
	max_content_size = (uint32_t)luaL_checkinteger(L,3);
	s = chk_stream_socket_new(fd,&option);

	if(!s) {
		CHK_SYSLOG(LOG_ERROR,"chk_stream_socket_new() failed");		
		return 0;
	}

	conn = LUA_NEWUSERDATA(L,http_connection);

	if(!conn) {
		chk_stream_socket_close(s,0);
		CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA() failed");			
		return 0;
	}

	conn->settings.on_message_begin = on_message_begin;
	conn->settings.on_url = on_url;
	conn->settings.on_status = on_status;
	conn->settings.on_header_field = on_header_field;
	conn->settings.on_header_value = on_header_value;
	conn->settings.on_headers_complete = on_headers_complete;
	conn->settings.on_body = on_body;
	conn->settings.on_message_complete = on_message_complete;
	conn->socket = s;
	conn->max_header_size = MIN(max_header_size,MAX_UINT32/2);
	conn->max_content_size = MIN(max_content_size,MAX_UINT32/2);
	conn->check_size = 0;
	chk_stream_socket_setUd(s,chk_ud_make_void(conn));
	http_parser_init(&conn->parser,HTTP_BOTH);
	luaL_getmetatable(L, HTTP_CONNECTION_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}

static int32_t lua_http_connection_bind(lua_State *L) {
	chk_event_loop    *event_loop;
	http_connection   *conn;
	chk_stream_socket *s;
	int32_t            ret;
	
	if(!lua_isfunction(L,3)) { 
		CHK_SYSLOG(LOG_ERROR,"argument 3 of http_connection_bind must be lua function");		
		return luaL_error(L,"argument 3 of http_connection_bind must be lua function");
	}

	event_loop = lua_checkeventloop(L,2);
	conn = lua_check_http_connection(L,1);
	s = conn->socket;
	
	if(!s){
		CHK_SYSLOG(LOG_ERROR,"http connection is close");		 
		return luaL_error(L,"http connection is close");
	}

	conn->cb   = chk_toluaRef(L,3);
	if(0 != (ret = chk_loop_add_handle(event_loop,(chk_handle*)s,data_event_cb))) {
		CHK_SYSLOG(LOG_ERROR,"http_connection_bind failed:%d",ret);			
		lua_pushstring(L,"http_connection_bind failed");
		return 1;
	}
	return 0;
}

static int32_t write_http_header(chk_bytebuffer *b,chk_http_packet *packet) {
	chk_http_header_iterator iterator;
	if(0 == chk_http_header_begin(packet,&iterator)) {
		do {
			
			if(0 != BYTEBUFFER_APPEND_CSTR(b,iterator.field)) {
				return -1;
			}

			if(0 != BYTEBUFFER_APPEND_CSTR(b," : ")) {
				return -1;
			}
			
			if(0 != BYTEBUFFER_APPEND_CSTR(b,iterator.value)) {
				return -1;
			}

			if(0 != BYTEBUFFER_APPEND_CSTR(b,"\r\n")) {
				return -1;
			}

		}while(0 == chk_http_header_iterator_next(&iterator));
	}
	return 0;	
}

static int32_t write_http_body(chk_bytebuffer *b,chk_http_packet *packet) {
	char body_length[64] = {0};
	uint32_t spos,chunk_data_size,size_remain;
	chk_bytechunk  *head;	
	chk_bytebuffer *body = chk_http_get_body(packet);
	
	if(!body || !body->head || 0 == body->datasize) {
		//no body
		if(0 != BYTEBUFFER_APPEND_CSTR(b,"\r\n")) {
			return -1;
		}
		else {
			return 0;
		}
	}
	else {
		do {
			head = body->head;
			snprintf(body_length,sizeof(body_length) - 1,"%u",body->datasize);
			
			if(0 != BYTEBUFFER_APPEND_CSTR(b,"Content-Length: ")) {
				return -1;
			}

			if(0 != BYTEBUFFER_APPEND_CSTR(b,body_length)) {
				return -1;
			}

			if(0 != BYTEBUFFER_APPEND_CSTR(b," \r\n\r\n")) {
				return -1;
			}
			//write the content
			spos = body->spos;
			size_remain = body->datasize;
			do{				
				chunk_data_size = MIN(head->cap - spos,size_remain);
				
				if(0 != chk_bytebuffer_append(b,(uint8_t*)&head->data[spos],chunk_data_size)) {
					return -1;
				}

				head = head->next;
				if(!head) break;
				size_remain -= chunk_data_size;
				spos = 0;		
			}while(size_remain);		
		}while(0);
		return 0;
	}
}

static int32_t push_bytebuffer(lua_State *L,chk_bytebuffer *buffer) {
	uint32_t spos,chunk_data_size,size_remain;
	struct luaL_Buffer b;
	char *in;
	chk_bytechunk   *head;
#if LUA_VERSION_NUM >= 503
	in = luaL_buffinitsize(L, &b, (size_t)buffer->datasize);
#else
 	luaL_buffinit(L, &b);
 	in = luaL_prepbuffsize(&b,(size_t)buffer->datasize);
#endif

 	if(!in) {
 		CHK_SYSLOG(LOG_ERROR,"prepbuff failed:%d",buffer->datasize);	
 		return -1;
 	}


	head = buffer->head;
	spos = buffer->spos;
	size_remain = buffer->datasize;
	do{		
		chunk_data_size = MIN(head->cap - spos,size_remain);
		memcpy(in,&head->data[spos],(size_t)chunk_data_size);
		head = head->next;
		if(!head) break;
		in += chunk_data_size;
		size_remain -= chunk_data_size;
		spos = 0;		
	}while(size_remain);
#if LUA_VERSION_NUM >= 503
	luaL_pushresultsize(&b,(size_t)buffer->datasize);
#else
	luaL_addsize(&b,(size_t)buffer->datasize);	
	luaL_pushresult(&b);
#endif	

	return 0;
}

static int32_t lua_http_connection_send_request(lua_State *L) {	
	http_connection   *conn;
	lua_http_packet   *packet;
	chk_bytebuffer    *b;	
	int32_t            ret = 0;
	const char        *http_version,*path,*host,*method;
	conn   = lua_check_http_connection(L,1);

	if(!conn->socket) {
		CHK_SYSLOG(LOG_ERROR,"http connection is close");			
		lua_pushstring(L,"http connection is close");
		return 1;
	}

	http_version = luaL_checkstring(L,2);
	host = luaL_checkstring(L,3);
	method = luaL_checkstring(L,4);
	path = luaL_checkstring(L,5);
	packet = lua_check_http_packet(L,6);

	if(!packet) {
		CHK_SYSLOG(LOG_ERROR,"lua_check_http_packet() failed");		
		return luaL_error(L,"lua_check_http_packet() failed");		
	}

	b = chk_bytebuffer_new(4096);

	if(!b) {
		CHK_SYSLOG(LOG_ERROR,"chk_bytebuffer_new(4096) failed");		
		lua_pushstring(L,"not enough memory");
		return 1;		
	}

	do{
		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,method))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b," "))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,path))) {
			break;
		}
		
		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b," HTTP/"))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,http_version))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,"\r\n"))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,"Host: "))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,host))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,"\r\n"))) {
			break;
		}

		if(0 != (ret = write_http_header(b,packet->packet))) {
			break;
		}

		if(0 != (ret = write_http_body(b,packet->packet))) {
			break;
		}

	}while(0);

	if(0 != ret) {
		chk_bytebuffer_del(b);
		CHK_SYSLOG(LOG_ERROR,"write packet error");		
		lua_pushstring(L,"send http request failed");
		return 1;		
	}

	if(0 != (ret = chk_stream_socket_send(conn->socket,b))){
		CHK_SYSLOG(LOG_ERROR,"chk_stream_socket_send() failed:%d",ret);		
		lua_pushstring(L,"send http request failed");
		return 1;
	}

	return 0;
}

static int32_t lua_http_connection_send_response(lua_State *L) {
	http_connection   *conn;
	lua_http_packet   *packet;
	chk_bytebuffer    *b;
	int32_t            ret = 0;	
	const char        *http_version,*status,*phase;
	conn   = lua_check_http_connection(L,1);
	if(!conn->socket) {
		lua_pushstring(L,"http connection is close");
		return 1;
	}	
	http_version = luaL_checkstring(L,2);
	status = luaL_checkstring(L,3);
	phase = luaL_checkstring(L,4);
	packet = lua_check_http_packet(L,5);	
	b = chk_bytebuffer_new(4096);

	if(!b) {
		CHK_SYSLOG(LOG_ERROR,"chk_bytebuffer_new(4096) failed");		
		lua_pushstring(L,"not enough memory");
		return 1;		
	}	
	
	do{

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,"HTTP/"))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,http_version))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b," "))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,status))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b," "))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,phase))) {
			break;
		}

		if(0 != (ret = BYTEBUFFER_APPEND_CSTR(b,"\r\n"))) {
			break;
		}

		if(0 != (ret = write_http_header(b,packet->packet))) {
			break;
		}
		if(!chk_http_get_body(packet->packet)){
			ret = BYTEBUFFER_APPEND_CSTR(b,"Content-Length:0\r\n\r\n");
		}
		else {	
			ret = write_http_body(b,packet->packet);
		}
	
	}while(0);

	if(0 != ret) {
		chk_bytebuffer_del(b);
		CHK_SYSLOG(LOG_ERROR,"write packet error");		
		lua_pushstring(L,"send http response failed");
		return 1;				
	}

	if(0 != chk_stream_socket_send(conn->socket,b)){
		lua_pushstring(L,"send http response failed");
		return 1;
	}

	return 0;
}

static int32_t lua_http_connection_close(lua_State *L) {
	http_connection   *conn = lua_check_http_connection(L,1);
	if(conn->socket) {
		chk_stream_socket_setUd(conn->socket,chk_ud_make_void(NULL));
		//delay 5秒关闭,尽量将数据发送出去				
		chk_stream_socket_close(conn->socket,5000);
		conn->socket = NULL;
	}
	return 0;
}

static int32_t lua_http_packet_get_method(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	const char *method = chk_http_method2name(chk_http_get_method(packet->packet));
	if(!method) return 0;
	lua_pushstring(L,method);
	return 1;
}

static int32_t lua_http_packet_get_status(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	const char *status = chk_http_get_status(packet->packet);
	if(!status) return 0;
	lua_pushstring(L,status);
	return 1;
}

static int32_t lua_http_packet_get_url(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	const char *url = chk_http_get_url(packet->packet);
	if(!url) return 0;
	lua_pushstring(L,url);
	return 1;	
}

static int32_t lua_http_packet_get_body(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	chk_bytebuffer  *body   = chk_http_get_body(packet->packet);
	if(!body || !body->head) return 0;
	if(0 == push_bytebuffer(L,body)) {
		return 1;
	}
	else {
		return 0;
	}
}

static int32_t lua_http_packet_get_header(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	const char *field = luaL_checkstring(L,2);
	const char *value = chk_http_get_header(packet->packet,field);	
	if(!value) return 0;
	lua_pushstring(L,value);
	return 1;
}

static int32_t lua_http_packet_get_headers(lua_State *L) {
	int32_t c;
	chk_http_header_iterator iterator;
	lua_http_packet *packet = lua_check_http_packet(L,1);
	if(0 == chk_http_header_begin(packet->packet,&iterator)) {
		//{{f,v},{f,v}...}
		lua_newtable(L);
		c = 1;
		do {
			lua_newtable(L);
			lua_pushstring(L,iterator.field);
			lua_rawseti(L,-2,1);
			lua_pushstring(L,iterator.value);
			lua_rawseti(L,-2,2);
			lua_rawseti(L,-2,c++);
		}while(0 == chk_http_header_iterator_next(&iterator));
		return 1;
	}
	return 0;
}

static int32_t lua_http_packet_set_header(lua_State *L) {
	lua_http_packet *packet = lua_check_http_packet(L,1);
	const char *field = luaL_checkstring(L,2);
	const char *value = luaL_checkstring(L,3);
	chk_string *str_field = chk_string_new(field,strlen(field)); 
	
	if(!str_field) {
		CHK_SYSLOG(LOG_ERROR,"chk_string_new() failed");	
		return 0;
	}

	chk_string *str_value = chk_string_new(value,strlen(value));

	if(!str_value) {
		chk_string_destroy(str_field);
		CHK_SYSLOG(LOG_ERROR,"chk_string_new() failed");	
		return 0;		
	} 
	
	if(0 != chk_http_set_header(packet->packet,str_field,str_value)) {
		chk_string_destroy(str_field);
		chk_string_destroy(str_value);
		CHK_SYSLOG(LOG_ERROR,"chk_http_set_header() failed");
	}
	return 0;
}

static int32_t lua_http_packet_append_body(lua_State *L) {
	size_t len;
	lua_http_packet *packet = lua_check_http_packet(L,1);
	const char *str = luaL_checklstring(L,2,&len);
	if(0 != chk_http_append_body(packet->packet,str,len)) {
		CHK_SYSLOG(LOG_ERROR,"chk_http_append_body() failed");
	}
	return 0;
}

static int32_t lua_http_packet_new(lua_State *L) {
	lua_http_packet *packet = LUA_NEWUSERDATA(L,lua_http_packet);

	if(!packet) {
		CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA(lua_http_packet) failed");		
		return 0;
	}

	packet->packet = chk_http_packet_new();

	if(!packet->packet) {
		CHK_SYSLOG(LOG_ERROR,"chk_http_packet_new() failed");		
		return 0;
	}

	luaL_getmetatable(L, HTTP_PACKET_METATABLE);
	lua_setmetatable(L, -2);
	return 1;	
}

static void register_http(lua_State *L) {
	luaL_Reg http_packet_mt[] = {
		{"__gc", lua_http_packet_gc},
		{NULL, NULL}
	};

	luaL_Reg http_packet_methods[] = {
		{"GetMethod",    lua_http_packet_get_method},
		{"GetStatus",    lua_http_packet_get_status},
		{"GetURL",       lua_http_packet_get_url},
		{"GetBody",      lua_http_packet_get_body},
		{"GetHeader",    lua_http_packet_get_header},
		{"GetAllHeaders",lua_http_packet_get_headers},
		{"SetHeader",    lua_http_packet_set_header},
		{"AppendBody",	 lua_http_packet_append_body},															
		{NULL,     NULL}
	};

	luaL_Reg http_connection_mt[] = {
		{"__gc", lua_http_connection_gc},
		{NULL, NULL}
	};

	luaL_Reg http_connection_methods[] = {
		{"SendRequest",    lua_http_connection_send_request},
		{"SendResponse",   lua_http_connection_send_response},
		{"Close",   	   lua_http_connection_close},
		{"Start",           lua_http_connection_bind},		
		{NULL,     NULL}
	};

	luaL_newmetatable(L, HTTP_PACKET_METATABLE);
	luaL_setfuncs(L, http_packet_mt, 0);

	luaL_newlib(L, http_packet_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, HTTP_CONNECTION_METATABLE);
	luaL_setfuncs(L, http_connection_mt, 0);

	luaL_newlib(L, http_connection_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
	lua_newtable(L);	
	SET_FUNCTION(L,"Connection",lua_new_http_connection);
	SET_FUNCTION(L,"HttpPacket",lua_http_packet_new);
}


