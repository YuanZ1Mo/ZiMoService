#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"

class ServicePortal
{
public:
	ServicePortal() {};
	~ServicePortal() {};


public:
	void JrpcRequsetReadCB(ZM_TAP_CTX* tap, const char* reqData);


private:
	void ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult);
	void ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError);
	void Response(ZM_TAP_CTX* tap, const ZMJSON& jsResult);
};


#endif // SERVICE_PORTAL_H