#include "service_portal.h"
#include "zm_net_tap.h"


void ServicePortal::ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
    ZMJSON jsresult;
    jsresult["result"] = jsResult;
    Response(tap, jsresult);
}

void ServicePortal::ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError)
{
    ZMJSON jserror;
    jserror["error"] = jsError;
    Response(tap, jserror);
}

void ServicePortal::Response(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
    ZmTapDelegate* back_delegate = ZmTapContext::BackChainPop(tap);
    if (back_delegate)
    {
        std::string jstr = jsResult.dump();
        ZmTapContext::SetOnBackData(tap, jstr.size(), jstr.c_str());
        back_delegate->OnTapDelegateBackEvent(tap);
    }
    else
    {
        tap->tap_context->Drop(tap);
    }
}

void ServicePortal::JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData)
{

    ZMJSON rsp = { { "isOk", 1 } };


    ResponseResult(tap, rsp);
}