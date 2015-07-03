#include "packet/luapacket.h"
#include "engine/engine.h"
#include "socket/wrap/connection.h"
#include "socket/socket_helper.h"
#include "socket/connector.h"
#include "socket/acceptor.h"
#include "packet/decoder.h"
#include "util/time.h"
#include "db/redis/client.h"
#include "util/signaler.h"
#include "util/timewheel.h"
#include "util/log.h"


#define SET_CONST(L,N) do{\
		lua_pushstring(L, #N);\
		lua_pushinteger(L, N);\
		lua_settable(L, -3);\
}while(0)

#define SET_FUNCTION(L,NAME,FUNC) do{\
	lua_pushstring(L,NAME);\
	lua_pushcfunction(L,FUNC);\
	lua_settable(L, -3);\
}while(0)

int bytecount = 0;


void lua_regerrcode(lua_State *L)
{

	lua_newtable(L);

	SET_CONST(L,EPERM);
	SET_CONST(L,ENOENT);
	SET_CONST(L,ESRCH);
	SET_CONST(L,EINTR); 
	SET_CONST(L,EIO);
	SET_CONST(L,ENXIO);
	SET_CONST(L,E2BIG);
	SET_CONST(L,ENOEXEC);
	SET_CONST(L,EBADF);
	SET_CONST(L,ECHILD);
	SET_CONST(L,EAGAIN);
	SET_CONST(L,ENOMEM);
	SET_CONST(L,EACCES);
	SET_CONST(L,EFAULT);
	SET_CONST(L,ENOTBLK);
	SET_CONST(L,EBUSY);
	SET_CONST(L,EEXIST);
	SET_CONST(L,EXDEV);
	SET_CONST(L,ENODEV);
	SET_CONST(L,ENOTDIR);
	SET_CONST(L,EISDIR);
	SET_CONST(L,EINVAL);
	SET_CONST(L,ENFILE);
	SET_CONST(L,EMFILE);
	SET_CONST(L,ENOTTY);
	SET_CONST(L,ETXTBSY);
	SET_CONST(L,EFBIG);
	SET_CONST(L,ENOSPC);
	SET_CONST(L,ESPIPE);
	SET_CONST(L,EROFS);
	SET_CONST(L,EMLINK);
	SET_CONST(L,EPIPE);
	SET_CONST(L,EDOM);
	SET_CONST(L,ERANGE);
	SET_CONST(L,EDEADLK);
	SET_CONST(L,ENAMETOOLONG);
	SET_CONST(L,ENOLCK);
	SET_CONST(L,ENOSYS);
	SET_CONST(L,ENOTEMPTY);
	SET_CONST(L,ELOOP);
	SET_CONST(L,EWOULDBLOCK);
	SET_CONST(L,ENOMSG);
	SET_CONST(L,EIDRM);
	SET_CONST(L,ECHRNG);
	SET_CONST(L,EL2NSYNC);
	SET_CONST(L,EL3HLT);
	SET_CONST(L,EL3RST);
	SET_CONST(L,ELNRNG);
	SET_CONST(L,EUNATCH);
	SET_CONST(L,ENOCSI);
	SET_CONST(L,EL2HLT);
	SET_CONST(L,EBADE);
	SET_CONST(L,EBADR);
	SET_CONST(L,EXFULL);
	SET_CONST(L,ENOANO);
	SET_CONST(L,EBADRQC);
	SET_CONST(L,EBADSLT);
	SET_CONST(L,EDEADLOCK);
	SET_CONST(L,EBFONT);
	SET_CONST(L,ENOSTR);
	SET_CONST(L,ENODATA);
	SET_CONST(L,ETIME);
	SET_CONST(L,ENOSR);
	SET_CONST(L,ENONET);
	SET_CONST(L,ENOPKG);
	SET_CONST(L,EREMOTE);
	SET_CONST(L,ENOLINK);
	SET_CONST(L,EADV);
	SET_CONST(L,ESRMNT);
	SET_CONST(L,ECOMM);
	SET_CONST(L,EPROTO);
	SET_CONST(L,EMULTIHOP);
	SET_CONST(L,EDOTDOT);
	SET_CONST(L,EBADMSG);
	SET_CONST(L,EOVERFLOW);
	SET_CONST(L,ENOTUNIQ);
	SET_CONST(L,EBADFD);
	SET_CONST(L,EREMCHG);
	SET_CONST(L,ELIBACC);
	SET_CONST(L,ELIBBAD);
	SET_CONST(L,ELIBSCN);
	SET_CONST(L,ELIBMAX);
	SET_CONST(L,ELIBEXEC);
	SET_CONST(L,EILSEQ);
	SET_CONST(L,ERESTART);
	SET_CONST(L,ESTRPIPE);
	SET_CONST(L,EUSERS);
	SET_CONST(L,ENOTSOCK);
	SET_CONST(L,EDESTADDRREQ);
	SET_CONST(L,EMSGSIZE);
	SET_CONST(L,EPROTOTYPE);
	SET_CONST(L,ENOPROTOOPT);
	SET_CONST(L,EPROTONOSUPPORT);
	SET_CONST(L,ESOCKTNOSUPPORT);
	SET_CONST(L,EOPNOTSUPP);
	SET_CONST(L,EPFNOSUPPORT);
	SET_CONST(L,EAFNOSUPPORT);
	SET_CONST(L,EADDRINUSE);
	SET_CONST(L,EADDRNOTAVAIL);
	SET_CONST(L,ENETDOWN);
	SET_CONST(L,ENETUNREACH);
	SET_CONST(L,ENETRESET);
	SET_CONST(L,ECONNABORTED);
	SET_CONST(L,ECONNRESET);
	SET_CONST(L,ENOBUFS);
	SET_CONST(L,EISCONN);
	SET_CONST(L,ENOTCONN);
	SET_CONST(L,ESHUTDOWN);
	SET_CONST(L,ETOOMANYREFS);
	SET_CONST(L,ETIMEDOUT);
	SET_CONST(L,ECONNREFUSED);
	SET_CONST(L,EHOSTDOWN);
	SET_CONST(L,EHOSTUNREACH);
	SET_CONST(L,EALREADY);
	SET_CONST(L,EINPROGRESS);
	SET_CONST(L,ESTALE);
	SET_CONST(L,EUCLEAN);
	SET_CONST(L,ENOTNAM);
	SET_CONST(L,ENAVAIL);
	SET_CONST(L,EISNAM);
	SET_CONST(L,EREMOTEIO);
	SET_CONST(L,EDQUOT);
	SET_CONST(L,ENOMEDIUM);
	SET_CONST(L,EMEDIUMTYPE);
	SET_CONST(L,ECANCELED);
	SET_CONST(L,ENOKEY);
	SET_CONST(L,EKEYEXPIRED);
	SET_CONST(L,EKEYREVOKED);
	SET_CONST(L,EKEYREJECTED);
	SET_CONST(L,EOWNERDEAD);
	SET_CONST(L,ENOTRECOVERABLE);
	//extend errno
	SET_CONST(L,ESOCKCLOSE);
	SET_CONST(L,EUNSPPLAT);
	SET_CONST(L,ENOASSENG);
	SET_CONST(L,EINVISOKTYPE);
	SET_CONST(L,EASSENG);
	SET_CONST(L,EINVIPK);
	SET_CONST(L,ERDISPERROR);
	SET_CONST(L,EALSETMBOX);
	SET_CONST(L,EMAXTHD);
	SET_CONST(L,EPIPECRERR);
	SET_CONST(L,EENASSERR);
	SET_CONST(L,ETMCLOSE);
	SET_CONST(L,EPKTOOLARGE);
	SET_CONST(L,ERVTIMEOUT);
	SET_CONST(L,ESNTIMEOUT);
	SET_CONST(L,EENGCLOSE);
	SET_CONST(L,EHTTPPARSE);
}

int32_t lua_get_bytebuffer_count(lua_State *L)
{
	lua_pushinteger(L,bytecount);
	return 1;
}

int32_t lua_systick(lua_State *L)
{
	lua_pushinteger(L,systick64());
	return 1;
}

int32_t luaopen_chuck(lua_State *L)
{
	
	signal(SIGPIPE,SIG_IGN);

	lua_newtable(L);
	
	reg_luaconnection(L);
	reg_luaconnector(L);
	reg_luaacceptor(L);
	reg_luaengine(L);
	reg_luatimewheel(L);

	SET_FUNCTION(L,"buffercount",lua_get_bytebuffer_count);
	SET_FUNCTION(L,"systick",lua_systick);

	lua_pushstring(L,"packet");
	reg_luapacket(L);
	lua_settable(L,-3);
			
	lua_pushstring(L,"socket_helper");
	reg_socket_helper(L);
	lua_settable(L,-3);

	lua_pushstring(L,"decoder");
	reg_luadecoder(L);
	lua_settable(L,-3);

	lua_pushstring(L,"error");
	lua_regerrcode(L);
	lua_settable(L,-3);

	lua_pushstring(L,"redis");
	reg_luaredis(L);
	lua_settable(L,-3);			

	lua_pushstring(L,"signal");
	reg_luasignaler(L);
	lua_settable(L,-3);

	lua_pushstring(L,"log");
	lua_reglog(L);
	lua_settable(L,-3);
		
	return 1;
}